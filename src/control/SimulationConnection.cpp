#include "control/SimulationConnection.h"
#include "Pch.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/ControlSessions.h"
#include "control/FrameInspection.h"
#include "control/HapticObservation.h"
#include "control/LiveInputControl.h"
#include "control/RecordingControl.h"
#include "control/SimulationControl.h"
#include "runtime/Session.h"
#include "simulation/GameBridge.h"

namespace simxr::control
{
static InspectionValue ReadDiagnostics(Session& session, const Message& request)
{
	const auto after = request.Find(tag::Offset) ? RequireRequestU64(request, tag::Offset) : 0;
	const auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 128;
	if (count > 256)
		throw ControlRequestError{ErrorCode::Invalid, "Diagnostic pages contain at most 256 events"};
	std::optional<FrameKey> frame;
	if (request.Find(tag::FrameId))
		frame = FrameKey{session.Snapshot().generation, FrameId{RequireRequestU64(request, tag::FrameId)}};
	return InspectDiagnostics(session.History().Diagnostics(after, static_cast<std::size_t>(count), frame));
}
static InspectionValue ReadRecording(Session& session, const Message& request, Message& response)
{
	if (response.minor < 14 && request.Find(tag::RecordingId))
		throw ControlRequestError{ErrorCode::UnsupportedField, "Recording identities require protocol 1.14"};
	const auto offset = request.Find(tag::Offset) ? RequireRequestU64(request, tag::Offset) : 0;
	auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 16;
	if (offset > std::numeric_limits<std::size_t>::max() || count > 16)
		throw ControlRequestError{ErrorCode::Invalid, "Recording pages contain at most 16 transactions"};
	for (;;)
	{
		CommandRecording page;
		try
		{
			page = session.Commands().Recorder().Snapshot(
				request.Find(tag::Offset) ? std::optional<std::size_t>{static_cast<std::size_t>(offset)} : std::nullopt,
				static_cast<std::size_t>(count));
		}
		catch (const std::out_of_range&)
		{
			throw ControlRequestError{ErrorCode::NotFound, "Recording offset has been acknowledged"};
		}
		if (page.drain && response.minor < 14)
			throw ControlRequestError{ErrorCode::UnsupportedField, "Draining recordings require protocol 1.14"};
		if ((page.drain || request.Find(tag::RecordingId)) &&
			RequireRequestU64(request, tag::RecordingId) != page.identity)
			throw ControlRequestError{ErrorCode::Conflict, "Recording identity changed"};
		if (!page.drain && offset > 1024)
			throw ControlRequestError{ErrorCode::Invalid, "Legacy recording offset exceeds 1024"};
		const auto actualOffset = request.Find(tag::Offset) ? offset : page.floor;
		InspectionValue document;
		bool fits = false;
		try
		{
			document = SimulationRecording(page);
			fits = document.Encode().size() <= MaxPayload - 4096;
		}
		catch (const std::length_error&)
		{
		}
		if (fits)
		{
			response.fields.push_back(Field::U64(tag::Offset, actualOffset));
			response.fields.push_back(Field::U64(tag::Length, page.transactions.size()));
			response.fields.push_back(Field::Bool(tag::End, actualOffset + page.transactions.size() >= page.total));
			return document;
		}
		if (count <= 1)
			throw ControlRequestError{ErrorCode::Capacity, "Recording transaction exceeds the wire page budget"};
		count /= 2;
	}
}

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
	if (kind != CommandSourceKind::Manual && kind != CommandSourceKind::Mcp && kind != CommandSourceKind::Rig &&
		!(kind == CommandSourceKind::Device && m_kind == CommandSourceKind::Device))
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
bool SimulationConnection::OwnsSequenceCapture(const SnapshotRecord& capture) const noexcept
{
	if (!m_kind || !capture.metadata || !capture.metadata->sequenceCapture)
		return false;
	const auto& receipt = *capture.metadata->sequenceCapture;
	return receipt.request.owner == CommandSource{*m_kind, m_connection.value} && receipt.reservation.id == capture.id;
}
Message SimulationConnection::Dispatch(const Message& request, const ControlSession& binding, Message response,
									   std::vector<SnapshotRecord>& held)
{
	const auto& session = binding.session;
	const auto opcode = static_cast<OpCode>(request.opcode);

	response.fields.push_back(Field::U64(tag::Session, binding.id.value));
	response.fields.push_back(Field::U64(tag::Generation, session->Snapshot().generation.value));
	if (opcode == OpCode::AttachLiveInput || opcode == OpCode::SubmitLiveInput || opcode == OpCode::DetachLiveInput ||
		opcode == OpCode::ReadLiveClock || opcode == OpCode::PublishDisplayPrediction)
		return DispatchLiveInput(request, binding, std::move(response));
	if (opcode == OpCode::ReadHaptics)
	{
		auto snapshot = session->ObserveHaptics({RequireRequestU64(request, tag::Generation)});
		if (!snapshot)
			throw ControlRequestError{ErrorCode::StaleGeneration, "Haptic session generation changed"};
		if (m_kind == CommandSourceKind::Device)
		{
			const auto applied = session->Commands().Snapshot().applied;
			const CommandSource source{CommandSourceKind::Device, m_connection.value};
			std::erase_if(
				snapshot->outputs,
				[&](const HapticOutput& output)
				{
					if (!applied || output.device.kind != InputDeviceKind::Controller || output.device.value >= 2)
						return true;
					const auto hand = static_cast<std::size_t>(output.device.value);
					return applied->owners[static_cast<std::size_t>(ControlChannel::LeftControls) + hand] != source ||
						   applied->owners[static_cast<std::size_t>(ControlChannel::LeftPose) + hand] != source;
				});
		}
		response.fields.push_back(Field::I64(tag::Time, snapshot->time.value.count()));
		response.fields.push_back(EncodeHaptics(snapshot->outputs));
		response.fields.push_back(Field::Bool(tag::Overflow, snapshot->outputs.size() > MaxObservedHaptics));
		return response;
	}
	if (m_kind == CommandSourceKind::Device &&
		(m_liveSession != session || m_liveGeneration != session->Snapshot().generation))
		throw ControlRequestError{ErrorCode::Conflict, "Device connection has no attachment to this session"};
	auto& scheduler = session->Commands();
	const bool trackerInteraction = session->Extensions().Contains(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
	const auto source = RequestSource(request);
	const auto simulationState = [&]
	{
		std::vector<std::uint64_t> bound;
		if (trackerInteraction)
			for (const auto& action : session->Actions().Snapshot().actions)
				if (action.definition.live && action.definition.type == XR_ACTION_TYPE_POSE_INPUT)
					for (const auto& resolved : action.sources)
						if (resolved.device.kind == InputDeviceKind::Tracker &&
							std::find(bound.begin(), bound.end(), resolved.device.value) == bound.end())
							bound.push_back(resolved.device.value);
		return SimulationState(
			scheduler.Snapshot(), false, trackerInteraction, bound, session->Actions().ControllerProfiles(),
			std::array{session->Extensions().Contains(XR_FB_BODY_TRACKING_EXTENSION_NAME),
					   session->Extensions().Contains(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME),
					   session->Extensions().Contains(XR_BD_BODY_TRACKING_EXTENSION_NAME)});
	};
	InspectionValue document;
	const auto execute = [&]
	{
		switch (static_cast<OpCode>(request.opcode))
		{
		case OpCode::UploadSequence:
		case OpCode::StartSequence:
		case OpCode::ReadSequence:
		case OpCode::StopSequence:
			Track(session, source);
			document = m_sequences.Dispatch(request, binding, source, response, held);
			break;
		case OpCode::ReadDiagnostics:
			document = ReadDiagnostics(*session, request);
			break;
		case OpCode::SubmitSimulation:
		{
			if (request.Find(tag::ExpectedInputs) && response.minor < 17)
				throw ControlRequestError{ErrorCode::UnsupportedField, "Expected inputs require protocol minor 17"};
			auto transaction = DecodeSimulationTransaction(request, m_connection.value);
			if (response.minor < 10 && std::any_of(
										   transaction.commands.begin(), transaction.commands.end(),
										   [](const auto& command)
										   {
											   const auto* tracker = std::get_if<SetTracker>(&command);
											   return std::holds_alternative<SetTrackers>(command) ||
													  (tracker && (tracker->value.poseFlags != TrackedPoseFlags ||
																   !tracker->value.sourcePath.View().empty()));
										   }))
				throw ControlRequestError{ErrorCode::UnsupportedField, "Tracker collections require protocol minor 10"};
			if (response.minor < 11 && std::any_of(
										   transaction.commands.begin(), transaction.commands.end(),
										   [](const auto& command)
										   {
											   const auto* body = std::get_if<SetBodyControls>(&command);
											   return body && body->observation.has_value();
										   }))
				throw ControlRequestError{ErrorCode::UnsupportedField, "Body observations require protocol minor 11"};
			if (response.minor < 12 && std::any_of(
										   transaction.commands.begin(), transaction.commands.end(),
										   [](const auto& command)
										   {
											   const auto* body = std::get_if<SetBodyControls>(&command);
											   return body && body->observationOrigin.has_value();
										   }))
				throw ControlRequestError{ErrorCode::UnsupportedField, "Body replay timing requires protocol minor 12"};
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
				document = simulationState();
			break;
		case OpCode::CancelSimulation:
			response.fields.push_back(Field::Text(
				tag::Phase,
				SimulationResultName(scheduler.Cancel(source, RequireRequestU64(request, tag::TransactionId)))));
			document = simulationState();
			break;
		case OpCode::ReleaseControl:
			session->ReleaseCommandSource(source);
			m_game.Disconnect(m_connection.value);
			response.fields.push_back(Field::Bool(tag::Released, true));
			document = simulationState();
			break;
		case OpCode::StartRecording:
		case OpCode::StopRecording:
		{
			Track(session, source);
			if (response.minor < 14 && (request.Find(tag::RecordingDrain) || request.Find(tag::RecordingId)))
				throw ControlRequestError{ErrorCode::UnsupportedField, "Recording drain requires protocol 1.14"};
			bool drain = false;
			if (const auto field = request.Find(tag::RecordingDrain))
			{
				if (!field->AsBool())
					throw ControlRequestError{ErrorCode::Invalid, "RecordingDrain must be Boolean"};
				drain = *field->AsBool();
			}
			const auto identity = request.Find(tag::RecordingId) ? RequireRequestU64(request, tag::RecordingId) : 0;
			const auto result = opcode == OpCode::StartRecording ? m_recordings.Start(m_connection, session, drain)
																 : m_recordings.Stop(m_connection, session, identity);
			if (result == ErrorCode::Busy)
				throw ControlRequestError{result, "Another connection owns recording control"};
			if (result == ErrorCode::Conflict)
				throw ControlRequestError{result, "Recording is active or its identity changed"};
			if (result != ErrorCode::Ok)
				throw ControlRequestError{result, "Recording control capacity exhausted"};
			document = SimulationRecording(scheduler.Recorder().Snapshot({}, 0));
			break;
		}
		case OpCode::AcknowledgeRecording:
		{
			const auto through = RequireRequestU64(request, tag::Offset);
			if (through > std::numeric_limits<std::size_t>::max())
				throw ControlRequestError{ErrorCode::Invalid, "Recording offset exceeds native capacity"};
			const auto result = m_recordings.Acknowledge(
				m_connection, session, RequireRequestU64(request, tag::RecordingId), static_cast<std::size_t>(through));
			if (result != ErrorCode::Ok)
				throw ControlRequestError{result, "Recording acknowledgement owner, identity or offset is invalid"};
			document = SimulationRecording(scheduler.Recorder().Snapshot({}, 0));
			break;
		}
		case OpCode::ReadRecording:
			document = ReadRecording(*session, request, response);
			break;
		default:
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported simulation operation"};
		}
	};
	const bool mutating =
		opcode == OpCode::SubmitSimulation || opcode == OpCode::CancelSimulation || opcode == OpCode::ReleaseControl ||
		opcode == OpCode::StartRecording || opcode == OpCode::StopRecording || opcode == OpCode::AcknowledgeRecording ||
		opcode == OpCode::UploadSequence || opcode == OpCode::StartSequence || opcode == OpCode::StopSequence;
	if (mutating)
	{
		if (!session->WithCommandGeneration({RequireRequestU64(request, tag::Generation)}, execute))
			throw ControlRequestError{ErrorCode::StaleGeneration,
									  "Session ended or generation changed before command admission"};
	}
	else if (opcode == OpCode::ReadSequence)
	{
		if (!session->WithCommandGeneration({RequireRequestU64(request, tag::Generation)}, execute, false))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Sequence generation changed before observation"};
	}
	else
		execute();
	response.fields.push_back(Field::Bytes(tag::Document, document.Encode(), true));
	return response;
}
} // namespace simxr::control
