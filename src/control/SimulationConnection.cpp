#include "control/SimulationConnection.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/ControlSessions.h"
#include "control/FrameInspection.h"
#include "control/RecordingControl.h"
#include "control/SimulationControl.h"
#include "runtime/Session.h"
#include "simulation/GameBridge.h"

#include <algorithm>
#include <stdexcept>

namespace simxr::control
{
namespace
{
InspectionValue ReadDiagnostics(Session& session, const Message& request)
{
	const auto after = request.Find(tag::Offset) ? RequireRequestU64(request, tag::Offset) : 0;
	const auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 128;
	if (count > 256)
		throw ControlRequestError{ErrorCode::Invalid, "Diagnostic pages contain at most 256 events"};
	std::optional<FrameKey> frame;
	if (request.Find(tag::FrameId))
		frame = FrameKey{session.Snapshot().generation, FrameId{RequireRequestU64(request, tag::FrameId)}};
	return InspectDiagnostics(session.History().Diagnostics(after, count, frame));
}
InspectionValue ReadRecording(Session& session, const Message& request, Message& response)
{
	const auto offset = request.Find(tag::Offset) ? RequireRequestU64(request, tag::Offset) : 0;
	auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 16;
	if (offset > 1024 || count > 16)
		throw ControlRequestError{ErrorCode::Invalid, "Recording pages contain at most 16 transactions"};
	for (;;)
	{
		const auto page = session.Commands().Recorder().Snapshot(offset, count);
		auto document = SimulationRecording(page);
		bool fits = false;
		try
		{
			fits = document.Encode().size() <= MaxPayload - 4096;
		}
		catch (const std::length_error&)
		{
		}
		if (fits)
		{
			response.fields.push_back(Field::U64(tag::Offset, offset));
			response.fields.push_back(Field::U64(tag::Length, page.transactions.size()));
			response.fields.push_back(Field::Bool(tag::End, offset + page.transactions.size() >= page.total));
			return document;
		}
		if (count <= 1)
			throw ControlRequestError{ErrorCode::Capacity, "Recording transaction exceeds the wire page budget"};
		count /= 2;
	}
}
} // namespace

SimulationConnection::SimulationConnection(ConnectionId connection, RecordingControl& recordings, GameBridge& game)
	: m_connection(connection), m_recordings(recordings), m_game(game)
{
	if (!connection.value)
		throw std::invalid_argument("A simulation source requires a connection identity");
	m_uses.reserve(64);
}
SimulationConnection::~SimulationConnection()
{
	for (const auto& use : m_uses)
	{
		try
		{
			use.session->ReleaseCommandSource(use.source, true);
		}
		catch (...)
		{
		}
		m_recordings.Disconnect(m_connection, use.session);
	}
}
std::optional<CommandSource> SimulationConnection::Source(std::optional<CommandSourceKind> requested) const
{
	const auto kind = requested.value_or(m_kind.value_or(CommandSourceKind::Mcp));
	if (kind != CommandSourceKind::Manual && kind != CommandSourceKind::Mcp && kind != CommandSourceKind::Rig)
		return {};
	if (m_kind && *m_kind != kind)
		return {};
	return CommandSource{kind, m_connection.value};
}
CommandSource SimulationConnection::RequestSource(const Message& request) const
{
	std::optional<CommandSourceKind> kind;
	if (request.Find(tag::SourceKind))
		kind = static_cast<CommandSourceKind>(RequireRequestU32(request, tag::SourceKind));
	const auto source = Source(kind);
	if (!source)
		throw ControlRequestError{ErrorCode::Conflict, "A connection cannot change its simulation source kind"};
	return *source;
}
void SimulationConnection::Track(const std::shared_ptr<Session>& session, CommandSource source)
{
	if (!session || source.id != m_connection.value || Source(source.kind) != source)
		throw std::invalid_argument("Simulation source does not belong to this connection");
	const auto found = std::find_if(m_uses.begin(), m_uses.end(),
									[&](const auto& use)
									{
										return use.session == session;
									});
	if (found == m_uses.end())
	{
		if (m_uses.size() == 64)
			throw ControlRequestError{ErrorCode::Capacity, "Connection simulation session capacity exhausted"};
		m_uses.push_back({session, source});
	}
	m_kind = source.kind;
}
Message SimulationConnection::Dispatch(const Message& request, const ControlSession& binding, Message response)
{
	const auto& session = binding.session;
	const auto opcode = static_cast<OpCode>(request.opcode);

	response.fields.push_back(Field::U64(tag::Session, binding.id.value));
	response.fields.push_back(Field::U64(tag::Generation, session->Snapshot().generation.value));
	auto& scheduler = session->Commands();
	const auto source = RequestSource(request);
	InspectionValue document;
	const auto execute = [&]
	{
		switch (static_cast<OpCode>(request.opcode))
		{
		case OpCode::ReadDiagnostics:
			document = ReadDiagnostics(*session, request);
			break;
		case OpCode::SubmitSimulation:
		{
			auto transaction = DecodeSimulationTransaction(request, m_connection.value);
			transaction.source = source;
			Track(session, source);
			const auto outcome = scheduler.Submit(std::move(transaction));
			response.fields.push_back(Field::U64(tag::TransactionId, outcome.transaction.id));
			response.fields.push_back(Field::Text(tag::Phase, SimulationResultName(outcome.result)));
			document = SimulationOutcome(outcome, false);
			break;
		}
		case OpCode::SimulationStatus:
			if (request.Find(tag::TransactionId))
			{
				const auto outcome = scheduler.Read(source, RequireRequestU64(request, tag::TransactionId));
				if (!outcome)
					throw ControlRequestError{ErrorCode::NotFound, "Simulation transaction is absent or retired"};
				response.fields.push_back(Field::Text(tag::Phase, SimulationResultName(outcome->result)));
				document = SimulationOutcome(*outcome, false);
			}
			else
				document = SimulationState(scheduler.Snapshot());
			break;
		case OpCode::CancelSimulation:
			response.fields.push_back(Field::Text(
				tag::Phase,
				SimulationResultName(scheduler.Cancel(source, RequireRequestU64(request, tag::TransactionId)))));
			document = SimulationState(scheduler.Snapshot());
			break;
		case OpCode::ReleaseControl:
			session->ReleaseCommandSource(source);
			m_game.Disconnect(m_connection.value);
			response.fields.push_back(Field::Bool(tag::Released, true));
			document = SimulationState(scheduler.Snapshot());
			break;
		case OpCode::StartRecording:
		case OpCode::StopRecording:
		{
			Track(session, source);
			const auto result = opcode == OpCode::StartRecording ? m_recordings.Start(m_connection, session)
																 : m_recordings.Stop(m_connection, session);
			if (result == ErrorCode::Busy)
				throw ControlRequestError{result, "Another connection owns recording control"};
			if (result == ErrorCode::Conflict)
				throw ControlRequestError{result, "Recording is already active"};
			if (result != ErrorCode::Ok)
				throw ControlRequestError{result, "Recording control capacity exhausted"};
			document = SimulationRecording(scheduler.Recorder().Snapshot(0, 0));
			break;
		}
		case OpCode::ReadRecording:
			document = ReadRecording(*session, request, response);
			break;
		default:
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported simulation operation"};
		}
	};
	const bool mutating = opcode == OpCode::SubmitSimulation || opcode == OpCode::CancelSimulation ||
						  opcode == OpCode::ReleaseControl || opcode == OpCode::StartRecording ||
						  opcode == OpCode::StopRecording;
	if (mutating)
	{
		if (!session->WithCommandGeneration({RequireRequestU64(request, tag::Generation)}, execute))
			throw ControlRequestError{ErrorCode::StaleGeneration,
									  "Session ended or generation changed before command admission"};
	}
	else
		execute();
	response.fields.push_back(Field::Bytes(tag::Document, document.Encode(), true));
	return response;
}
} // namespace simxr::control
