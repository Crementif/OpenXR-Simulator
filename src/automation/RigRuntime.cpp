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
wire::Message Nested(const wire::Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	if (!field)
		throw std::runtime_error("Runtime omitted expected nested fields");
	auto decoded = wire::DecodeFields(field->value);
	if (decoded.error != wire::DecodeError::None)
		throw std::runtime_error("Runtime returned invalid nested fields");
	wire::Message result;
	result.fields = std::move(decoded.fields);
	return result;
}
std::uint64_t Next(std::uint64_t& counter)
{
	if (counter == std::numeric_limits<std::uint64_t>::max())
		throw std::length_error("Rig command identifiers exhausted");
	return counter++;
}
control::Field EncodeTarget(ScriptCaptureTarget target)
{
	const std::vector fields{
		control::Field::U32(tag::CaptureKind, static_cast<std::uint32_t>(target.kind), true),
		control::Field::U32(tag::Layer, target.layer), control::Field::U32(tag::View, target.view)};
	return control::Field::Bytes(tag::Target, control::EncodeFields(fields));
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
	if (m_lease || m_inputRecording || !m_captures.empty() || !m_inspections.empty())
		throw std::logic_error("Release rig runtime resources before configuring another run");
	m_session = session;
	m_generation = generation;
	m_lease = 0;
	m_nextRenew = {};
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
	const auto now = std::chrono::steady_clock::now();
	const bool sessionOperation = opcode == control::OpCode::Status || opcode == control::OpCode::Submit ||
		opcode == control::OpCode::Operation || opcode == control::OpCode::SimulationStatus;
	if (m_lease && sessionOperation && now >= m_nextRenew)
	{
		auto renewal = Session();
		renewal.push_back(control::Field::U64(tag::Lease, m_lease, true));
		Exchange(control::OpCode::Renew, std::move(renewal));
		m_nextRenew = now + std::chrono::seconds{1};
	}
	return Exchange(opcode, std::move(fields));
}
control::Message RigRuntime::Exchange(control::OpCode opcode, Fields fields)
{
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
ScriptStep RigRuntime::StepApplication(ScriptCaptureTarget target)
{
	return CaptureStep(control::CommandKind::Step,
		{EncodeTarget(target), control::Field::U32(tag::Lookahead, 1)});
}
std::vector<std::byte> RigRuntime::StartInputRecording()
{
	const auto response = Call(control::OpCode::StartRecording, Source());
	m_inputRecording = true;
	return Document(response);
}
std::vector<std::byte> RigRuntime::StopInputRecording()
{
	const auto response = Call(control::OpCode::StopRecording, Source());
	m_inputRecording = false;
	return Document(response);
}
std::vector<std::byte> RigRuntime::ReadInputRecording(std::uint64_t offset, std::uint64_t count)
{
	if (offset > 1024 || count > 16)
		throw std::invalid_argument("Input recording pages require offset <= 1024 and count <= 16");
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::Offset, offset));
	fields.push_back(control::Field::U64(tag::Length, count));
	return Document(Call(control::OpCode::ReadRecording, std::move(fields)));
}
void RigRuntime::StartImageRecording(ScriptCaptureTarget target, std::uint32_t capacity, std::uint64_t frames)
{
	if (capacity < 1 || capacity > 4)
		throw std::invalid_argument("Image recording capacity must be in [1, 4]");
	Debug(control::CommandKind::StartImageRecording,
		{EncodeTarget(target), control::Field::U32(tag::Capacity, capacity), control::Field::U64(tag::Length, frames)});
}
void RigRuntime::StopImageRecording()
{
	Debug(control::CommandKind::StopImageRecording);
}
ScriptImageRecording RigRuntime::ImageRecording()
{
	const auto status = Call(control::OpCode::Status, Session());
	const auto record = Nested(status, tag::ImageRecording);
	ScriptImageRecording result;
	result.available =
		(Number(status, tag::Capabilities) & static_cast<std::uint64_t>(control::Capability::ImageRecording)) != 0;
	result.phase = Text(record, tag::Phase);
	result.capacity = Number(record, tag::Capacity);
	result.captured = Number(record, tag::Length);
	result.requested = Number(record, tag::TotalFrames);
	if (record.Find(tag::Frame))
		result.missedFrame = Number(Nested(record, tag::Frame), tag::FrameId);
	for (const auto& capture : Nested(record, tag::Captures).fields)
	{
		if (!capture.AsU64())
			throw std::runtime_error("Runtime returned an invalid recording capture identity");
		result.captures.push_back(*capture.AsU64());
	}
	return result;
}
ScriptCapture RigRuntime::Capture(std::uint64_t capture)
{
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::CaptureId, capture, true));
	const auto response = Call(control::OpCode::Capture, std::move(fields));
	ScriptCapture result;
	result.identity = Number(response, tag::CaptureId);
	result.phase = Text(response, tag::Phase);
	if (response.Find(tag::Frame))
		result.frame = Number(Nested(response, tag::Frame), tag::FrameId);
	const auto* available = response.Find(tag::ImageAvailable);
	if (!available || !available->AsBool())
		throw std::runtime_error("Runtime omitted capture image availability");
	result.imageAvailable = *available->AsBool();
	return result;
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
std::vector<std::byte> RigRuntime::ProjectionHistory(std::uint32_t count)
{
	if (!count || count > 64)
		throw std::invalid_argument("Projection history count must be in [1,64]");
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::Length, count));
	return Document(Call(control::OpCode::ReadProjectionHistory, std::move(fields)));
}
ScriptInspection RigRuntime::Inspect(std::uint64_t capture)
{
	// Reserve ownership bookkeeping before acquiring a remote reader.
	if (m_inspections.size() == m_inspections.capacity())
		m_inspections.reserve(m_inspections.size() + 1);
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::CaptureId, capture, true));
	const auto response = Call(control::OpCode::InspectFrame, std::move(fields));
	const auto identity = Number(response, tag::InspectionId);
	if (std::find(m_inspections.begin(), m_inspections.end(), identity) == m_inspections.end())
		m_inspections.push_back(identity);
	return {Document(response), identity};
}
std::vector<std::byte> RigRuntime::Revision(std::uint64_t revision)
{
	return Document(Call(control::OpCode::FeatureRevision, {control::Field::U64(tag::RevisionId, revision, true)}));
}
void RigRuntime::ReleaseInspection(std::uint64_t inspection)
{
	Call(control::OpCode::ReleaseInspection, {control::Field::U64(tag::InspectionId, inspection, true)});
	std::erase(m_inspections, inspection);
}
std::vector<std::byte> RigRuntime::State()
{
	return Document(Call(control::OpCode::SimulationStatus, Source()));
}
void RigRuntime::ReleaseInputs()
{
	Call(control::OpCode::ReleaseControl, Source());
}
void RigRuntime::Export(std::uint64_t capture, const std::string& path,
						std::optional<ScriptCaptureTarget> target, std::optional<std::uint32_t> arraySlice)
{
	auto fields = Session();
	fields.push_back(control::Field::U64(tag::CaptureId, capture, true));
	if (target)
		fields.push_back(EncodeTarget(*target));
	const auto image = Number(Call(control::OpCode::RetainImage, std::move(fields)), tag::ImageReaderId);
	RemoteResource reader{*this, control::OpCode::ReleaseImage, tag::ImageReaderId, image};
	Fields exportFields{control::Field::U64(tag::ImageReaderId, image, true)};
	if (arraySlice)
		exportFields.push_back(control::Field::U32(tag::Layer, *arraySlice));
	const auto start = Call(control::OpCode::StartExport, std::move(exportFields));
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
void RigRuntime::CleanupCall(control::OpCode opcode, Fields fields)
{
	const auto response = m_exchange(opcode, std::move(fields));
	const auto* field = response.Find(tag::Error);
	if (!field || !field->AsU32())
		throw std::runtime_error("Runtime omitted cleanup result");
	const auto code = static_cast<control::ErrorCode>(*field->AsU32());
	if (code != control::ErrorCode::Ok && code != control::ErrorCode::Closed &&
		code != control::ErrorCode::StaleGeneration && code != control::ErrorCode::NotFound)
		throw std::runtime_error(Text(response, tag::Detail));
}
std::string RigRuntime::Cleanup()
{
	std::string error;
	for (const auto inspection : m_inspections)
		ReleaseUnchecked(control::OpCode::ReleaseInspection, tag::InspectionId, inspection);
	m_inspections.clear();
	if (m_inputRecording)
	{
		try
		{
			CleanupCall(control::OpCode::StopRecording, Source());
		}
		catch (const std::exception& failure)
		{
			error = std::string{"Input recording stop failed: "} + failure.what();
		}
		m_inputRecording = false;
	}
	// Cancellation must not prevent releasing source-owned controls or the lease.
	try
	{
		CleanupCall(control::OpCode::ReleaseControl, Source());
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
