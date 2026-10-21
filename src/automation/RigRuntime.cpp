#include "automation/RigRuntime.h"
#include "automation/RigProgress.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>

namespace simxr::automation
{
namespace
{
namespace wire = control;
namespace tag = wire::tag;
std::string Text(const wire::Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	return field && field->AsText() ? std::string{*field->AsText()} : std::string{};
}
std::uint64_t Number(const wire::Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	if (!field || !field->AsU64())
		throw std::runtime_error("Runtime omitted an expected identifier");
	return *field->AsU64();
}
std::vector<std::byte> Document(const wire::Message& message)
{
	const auto* document = message.Find(tag::Document);
	if (!document)
		throw std::runtime_error("Runtime omitted an inspection document");
	return document->value;
}
std::uint64_t Next(std::uint64_t& counter)
{
	if (counter == std::numeric_limits<std::uint64_t>::max())
		throw std::length_error("Rig command identifiers exhausted");
	return counter++;
}
void PollAgain()
{
	std::this_thread::sleep_for(std::chrono::milliseconds{2});
}
} // namespace

class RigRuntime::RemoteResource
{
  public:
	RemoteResource(RigRuntime& runtime, control::OpCode release, std::uint16_t field, std::uint64_t id)
		: m_runtime(runtime), m_release(release), m_field(field), m_id(id)
	{
	}
	~RemoteResource()
	{
		if (m_id)
			m_runtime.ReleaseUnchecked(m_release, m_field, m_id);
	}
	RemoteResource(const RemoteResource&) = delete;
	RemoteResource& operator=(const RemoteResource&) = delete;
	void Release()
	{
		m_runtime.Call(m_release, {control::Field::U64(m_field, m_id)});
		m_id = 0;
	}

  private:
	RigRuntime& m_runtime;
	control::OpCode m_release;
	std::uint16_t m_field;
	std::uint64_t m_id;
};

RigRuntime::RigRuntime(RigExchange exchange, RigProgress& progress)
	: m_exchange(std::move(exchange)), m_progress(progress)
{
	if (!m_exchange)
		throw std::invalid_argument("A rig transport is required");
}
void RigRuntime::Configure(control::ControlSessionId session, std::uint64_t generation)
{
	if (m_lease || !m_captures.empty())
		throw std::logic_error("Release rig runtime resources before configuring another run");
	m_session = session;
	m_generation = generation;
	m_lease = 0;
	m_captures.clear();
}
RigRuntime::Fields RigRuntime::Session() const
{
	return {control::Field::U64(tag::Session, m_session.value, true),
			control::Field::U64(tag::Generation, m_generation, true)};
}
RigRuntime::Fields RigRuntime::Source() const
{
	auto fields = Session();
	fields.push_back(control::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Rig)));
	return fields;
}
control::Message RigRuntime::Call(control::OpCode opcode, Fields fields)
{
	m_progress.Check();
	auto response = m_exchange(opcode, std::move(fields));
	const auto* error = response.Find(tag::Error);
	if (!error || !error->AsU32() || *error->AsU32() != static_cast<std::uint32_t>(control::ErrorCode::Ok))
		throw std::runtime_error("Runtime control: " + Text(response, tag::Detail));
	return response;
}
control::Message RigRuntime::Debug(control::CommandKind kind, Fields extra)
{
	if (!m_lease)
		m_lease = Number(Call(control::OpCode::Acquire, Session()), tag::Lease);
	const auto command = Next(m_nextCommand);
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::Lease, m_lease, true));
	fields.push_back(control::Field::U64(tag::CommandId, command, true));
	fields.push_back(control::Field::U32(tag::CommandKind, static_cast<std::uint32_t>(kind), true));
	fields.insert(fields.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
	const auto submitted = Call(control::OpCode::Submit, std::move(fields));
	const auto operation = Number(submitted, tag::OperationId);
	for (;;)
	{
		auto query = Session();
		query.push_back(control::Field::U64(tag::OperationId, operation, true));
		auto response = Call(control::OpCode::Operation, std::move(query));
		const auto phase = Text(response, tag::Phase);
		if (phase == "complete")
		{
			auto acknowledged = Session();
			acknowledged.push_back(control::Field::U64(tag::Lease, m_lease));
			acknowledged.push_back(control::Field::U64(tag::CommandId, command));
			Call(control::OpCode::Acknowledge, std::move(acknowledged));
			return response;
		}
		if (phase != "queued" && phase != "pending")
			throw std::runtime_error("Debug operation " + phase + ": " + Text(response, tag::Progress));
		PollAgain();
	}
}
std::uint64_t RigRuntime::Schedule(CommandTarget target, CommandLatePolicy policy,
								   std::vector<SimulationCommand> commands)
{
	const auto transaction = Next(m_nextTransaction);
	auto fields = Source();
	fields.push_back(control::Field::U64(tag::TransactionId, transaction, true));
	fields.push_back(control::Field::U32(tag::LatePolicy, static_cast<std::uint32_t>(policy), true));
	if (target.frame)
		fields.push_back(control::Field::U64(tag::ApplyFrame, target.frame->value, true));
	if (target.time)
		fields.push_back(control::Field::I64(tag::ApplyTime, target.time->value.count(), true));
	Fields encoded;
	for (std::size_t index = 0; index < commands.size(); ++index)
		encoded.push_back(control::EncodeSimulationCommand(commands[index], static_cast<std::uint16_t>(index + 1)));
	fields.push_back(control::Field::Bytes(tag::Commands, control::EncodeFields(encoded), true));
	const auto response = Call(control::OpCode::SubmitSimulation, std::move(fields));
	const auto phase = Text(response, tag::Phase);
	if (phase != "queued" && phase != "rescheduled" && phase != "applied")
		throw std::runtime_error("Simulation transaction " + phase);
	m_progress.TransactionScheduled();
	return transaction;
}
std::vector<std::byte> RigRuntime::AwaitTransaction(std::uint64_t transaction)
{
	for (;;)
	{
		auto fields = Source();
		fields.push_back(control::Field::U64(tag::TransactionId, transaction, true));
		const auto response = Call(control::OpCode::SimulationStatus, std::move(fields));
		const auto phase = Text(response, tag::Phase);
		if (phase == "applied")
			return Document(response);
		if (phase != "queued" && phase != "rescheduled")
			throw std::runtime_error("Simulation transaction " + phase);
		PollAgain();
	}
}
void RigRuntime::PauseApplication()
{
	Debug(control::CommandKind::Pause);
}
void RigRuntime::ResumeApplication()
{
	Debug(control::CommandKind::Resume);
}
ScriptStep RigRuntime::StepApplication(std::optional<std::uint32_t> layer, std::uint32_t view)
{
	Fields target{control::Field::U32(tag::CaptureKind, layer ? 2 : 1, true)};
	if (layer)
	{
		target.push_back(control::Field::U32(tag::Layer, *layer));
		target.push_back(control::Field::U32(tag::View, view));
	}
	return CaptureStep(control::CommandKind::Step, {control::Field::Bytes(tag::Target, control::EncodeFields(target)),
													control::Field::U32(tag::Lookahead, 1)});
}
ScriptStep RigRuntime::StepDisplay()
{
	return CaptureStep(control::CommandKind::DisplayStep);
}
ScriptStep RigRuntime::CaptureStep(control::CommandKind kind, Fields fields)
{
	if (m_captures.size() == m_captures.capacity())
		m_captures.reserve(std::max<std::size_t>(8, m_captures.capacity() * 2));
	const auto response = Debug(kind, std::move(fields));
	ScriptStep step;
	step.progress = Text(response, tag::Progress);
	if (const auto frame = response.Find(tag::Frame))
	{
		control::Message identity;
		auto decoded = control::DecodeFields(frame->value);
		if (decoded.error != control::DecodeError::None)
			throw std::runtime_error("Step returned an invalid frame identity");
		identity.fields = std::move(decoded.fields);
		step.frame = FrameId{Number(identity, tag::FrameId)};
	}
	if (const auto capture = response.Find(tag::CaptureId))
		step.capture = capture->AsU64();
	if (const auto* captures = response.Find(tag::Captures))
	{
		const auto entries = control::DecodeFields(captures->value);
		if (entries.error == control::DecodeError::None && !entries.fields.empty() && entries.fields[0].AsU64())
		{
			step.capture = *entries.fields[0].AsU64();
		}
	}
	if (step.capture && std::find(m_captures.begin(), m_captures.end(), *step.capture) == m_captures.end())
	{
		m_captures.push_back(*step.capture);
		m_progress.CaptureRetained();
	}
	return step;
}
std::vector<std::byte> RigRuntime::Inspect(std::uint64_t capture)
{
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::CaptureId, capture, true));
	const auto response = Call(control::OpCode::InspectFrame, std::move(fields));
	RemoteResource inspection{*this, control::OpCode::ReleaseInspection, tag::InspectionId,
							  Number(response, tag::InspectionId)};
	auto document = Document(response);
	inspection.Release();
	return document;
}
std::vector<std::byte> RigRuntime::State()
{
	return Document(Call(control::OpCode::SimulationStatus, Source()));
}
void RigRuntime::ReleaseInputs()
{
	Call(control::OpCode::ReleaseControl, Source());
}
void RigRuntime::Export(std::uint64_t capture, const std::string& path)
{
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::CaptureId, capture, true));
	const auto image = Number(Call(control::OpCode::RetainImage, std::move(fields)), tag::ImageReaderId);
	RemoteResource reader{*this, control::OpCode::ReleaseImage, tag::ImageReaderId, image};
	const auto start = Call(control::OpCode::StartExport, {control::Field::U64(tag::ImageReaderId, image, true)});
	const auto exported = Number(start, tag::ExportId);
	RemoteResource ticket{*this, control::OpCode::ReleaseExport, tag::ExportId, exported};
	const auto total = Number(start, tag::TotalBytes);
	for (;;)
	{
		const auto info = Call(control::OpCode::ExportInfo, {control::Field::U64(tag::ExportId, exported, true)});
		const auto phase = Text(info, tag::Phase);
		if (phase == "complete")
			break;
		if (phase != "pending")
			throw std::runtime_error("Explicit export failed");
		PollAgain();
	}
	const std::u8string utf8Path{path.begin(), path.end()};
	std::ofstream output(std::filesystem::path{utf8Path}, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("Cannot open explicit export path");
	std::uint64_t offset = 0;
	while (offset < total)
	{
		const auto chunk =
			Call(control::OpCode::ReadExport,
				 {control::Field::U64(tag::ExportId, exported, true), control::Field::U64(tag::Offset, offset, true),
				  control::Field::U64(tag::Length, control::MaxPayload - 4096, true)});
		const auto* bytes = chunk.Find(tag::Bytes);
		if (!bytes || bytes->value.empty() || Number(chunk, tag::Offset) != offset ||
			bytes->value.size() > total - offset)
			throw std::runtime_error("Invalid export chunk");
		output.write(reinterpret_cast<const char*>(bytes->value.data()),
					 static_cast<std::streamsize>(bytes->value.size()));
		if (!output)
			throw std::runtime_error("Explicit export write failed");
		offset += bytes->value.size();
	}
	output.close();
	if (!output)
		throw std::runtime_error("Explicit export close failed");
	ticket.Release();
	reader.Release();
}
void RigRuntime::ReleaseCapture(std::uint64_t capture)
{
	Debug(control::CommandKind::Release, {control::Field::U64(tag::CaptureId, capture, true)});
	std::erase(m_captures, capture);
}
ScriptGameOperation RigRuntime::GameCommand(std::uint32_t kind, std::string payload)
{
	const auto game = Number(Call(control::OpCode::GameStatus, Session()), tag::GameId);
	if (!game)
		throw std::runtime_error("No cooperating game bridge is registered");
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::GameId, game));
	fields.push_back(control::Field::U32(tag::GameKind, kind));
	fields.push_back(control::Field::U64(tag::CommandId, Next(m_nextCommand)));
	fields.push_back(control::Field::Text(tag::Detail, payload));
	return {Number(Call(control::OpCode::SubmitGame, std::move(fields)), tag::OperationId), game};
}
std::vector<std::byte> RigRuntime::AwaitGame(std::uint64_t game, std::uint64_t operation)
{
	for (;;)
	{
		auto fields = Session();
		fields.push_back(control::Field::U64(tag::GameId, game));
		fields.push_back(control::Field::U64(tag::OperationId, operation));
		const auto response = Call(control::OpCode::GameOperation, std::move(fields));
		const auto phase = Text(response, tag::Phase);
		if (phase == "complete")
			return Document(response);
		if (phase != "queued" && phase != "delivered")
			throw std::runtime_error("Game command " + phase);
		PollAgain();
	}
}
std::vector<std::byte> RigRuntime::GameState()
{
	return Document(Call(control::OpCode::GameStatus, Session()));
}
void RigRuntime::ReleaseUnchecked(control::OpCode opcode, std::uint16_t field, std::uint64_t id) noexcept
{
	try
	{
		m_exchange(opcode, {control::Field::U64(field, id)});
	}
	catch (...)
	{
	}
}
std::string RigRuntime::Cleanup()
{
	std::string error;
	// Cancellation must not prevent releasing source-owned controls or the lease.
	try
	{
		const auto response = m_exchange(control::OpCode::ReleaseControl, Source());
		const auto* field = response.Find(tag::Error);
		if (!field || !field->AsU32())
			throw std::runtime_error("Runtime omitted cleanup result");
		const auto code = static_cast<control::ErrorCode>(*field->AsU32());
		if (code != control::ErrorCode::Ok && code != control::ErrorCode::Closed &&
			code != control::ErrorCode::StaleGeneration && code != control::ErrorCode::NotFound)
			throw std::runtime_error(Text(response, tag::Detail));
	}
	catch (const std::exception& failure)
	{
		error = std::string{"Input release failed: "} + failure.what();
	}
	if (m_lease)
	{
		for (const auto capture : m_captures)
		{
			try
			{
				auto fields = Session();
				fields.push_back(control::Field::U64(tag::Lease, m_lease));
				fields.push_back(control::Field::U64(tag::CommandId, Next(m_nextCommand)));
				fields.push_back(
					control::Field::U32(tag::CommandKind, static_cast<std::uint32_t>(control::CommandKind::Release)));
				fields.push_back(control::Field::U64(tag::CaptureId, capture));
				m_exchange(control::OpCode::Submit, std::move(fields));
			}
			catch (...)
			{
			}
		}
		try
		{
			auto fields = Session();
			fields.push_back(control::Field::U64(tag::Lease, m_lease));
			m_exchange(control::OpCode::Relinquish, std::move(fields));
		}
		catch (...)
		{
		}
	}
	m_captures.clear();
	m_lease = 0;
	return error;
}
} // namespace simxr::automation
