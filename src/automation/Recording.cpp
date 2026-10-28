#include "automation/Recording.h"
#include "Pch.h"

#include "control/ControlSchema.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace simxr::automation
{
namespace wire = control;
class RecordingNode
{
  public:
	explicit RecordingNode(std::span<const std::byte> bytes)
	{
		auto decoded = wire::DecodeFields(bytes);
		if (decoded.error != wire::DecodeError::None)
			throw std::runtime_error("Malformed recording inspection fields");
		m_fields.fields = std::move(decoded.fields);
	}
	std::optional<RecordingNode> Find(std::string_view name) const
	{
		const auto members = Fields(Data(wire::InspectionType::Object).value);
		for (const auto& field : members)
		{
			RecordingNode child(field.value);
			const auto label = child.m_fields.Find(1);
			if (!label || !label->AsText())
				throw std::runtime_error("Unnamed recording field");
			if (*label->AsText() == name)
				return child;
		}
		return {};
	}
	RecordingNode Member(std::string_view name) const
	{
		const auto child = Find(name);
		if (!child)
			throw std::runtime_error("Missing recording field: " + std::string(name));
		return *child;
	}
	bool IsNull() const
	{
		const auto kind = m_fields.Find(2);
		return kind && kind->AsU32() == static_cast<std::uint32_t>(wire::InspectionType::Null);
	}
	std::vector<RecordingNode> Elements() const
	{
		std::vector<RecordingNode> result;
		for (const auto& page : Fields(Data(wire::InspectionType::Array).value))
			for (const auto& child : Fields(page.value))
			{
				if (result.size() == 1024)
					throw std::runtime_error("Recording array exceeds its bound");
				result.emplace_back(child.value);
			}
		return result;
	}
	std::uint64_t Unsigned() const
	{
		const auto value = Data(wire::InspectionType::Unsigned).AsU64();
		if (!value)
			throw std::runtime_error("Invalid recording integer");
		return *value;
	}
	std::int64_t Signed() const
	{
		const auto value = Data(wire::InspectionType::Signed).AsI64();
		if (!value)
			throw std::runtime_error("Invalid recording time");
		return *value;
	}
	bool Boolean() const
	{
		const auto value = Data(wire::InspectionType::Boolean).AsBool();
		if (!value)
			throw std::runtime_error("Invalid recording boolean");
		return *value;
	}
	std::string Text() const
	{
		const auto value = Data(wire::InspectionType::Text).AsText();
		if (!value)
			throw std::runtime_error("Invalid recording text");
		return std::string(*value);
	}

  private:
	static std::vector<wire::Field> Fields(std::span<const std::byte> bytes)
	{
		auto decoded = wire::DecodeFields(bytes);
		if (decoded.error != wire::DecodeError::None)
			throw std::runtime_error("Malformed recording collection");
		return std::move(decoded.fields);
	}
	const wire::Field& Data(wire::InspectionType expected) const
	{
		const auto kind = m_fields.Find(2), data = m_fields.Find(3);
		if (!kind || kind->AsU32() != static_cast<std::uint32_t>(expected) || !data)
			throw std::runtime_error("Unexpected recording value type");
		return *data;
	}
	wire::Message m_fields;
};
static std::vector<SimulationCommand> Commands(const RecordingNode& array, std::uint64_t version)
{
	std::vector<SimulationCommand> commands;
	for (const auto& node : array.Elements())
	{
		if (commands.size() == 64 || node.Member("encoding").Text() != "simxr-command-v1-hex")
			throw std::runtime_error("Unsupported recording command encoding or count");
		const auto text = node.Member("data").Text();
		if (text.empty() || text.size() % 2 || text.size() > 128 * 1024)
			throw std::runtime_error("Invalid bounded recording command");
		wire::Field field;
		field.tag = 1;
		field.value.reserve(text.size() / 2);
		for (std::size_t index = 0; index < text.size(); index += 2)
		{
			unsigned byte = 0;
			const auto parsed = std::from_chars(text.data() + index, text.data() + index + 2, byte, 16);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index + 2)
				throw std::runtime_error("Invalid hexadecimal recording command");
			field.value.push_back(static_cast<std::byte>(byte));
		}
		auto command = wire::DecodeSimulationCommand(field);
		if (const auto* body = std::get_if<SetBodyControls>(&command); body && body->observation && version < 4)
			throw std::runtime_error("Body observations require recording schema 4");
		if (version < 3 && std::holds_alternative<SetTrackers>(command))
			throw std::runtime_error("Tracker collections require recording schema 3");
		if (wire::EncodeSimulationCommand(command).value != field.value)
			throw std::runtime_error("Recorded command cannot be exported without losing fields");
		commands.push_back(std::move(command));
	}
	return commands;
}
static std::vector<std::byte> CommandIdentity(std::span<const SimulationCommand> commands)
{
	std::vector<wire::Field> fields;
	for (const auto& command : commands)
		fields.push_back(wire::EncodeSimulationCommand(command, static_cast<std::uint16_t>(fields.size() + 1)));
	return wire::EncodeFields(fields);
}
static std::uint64_t Number(const wire::Message& response, std::uint16_t name, std::uint64_t fallback)
{
	const auto field = response.Find(name);
	if (!field)
		return fallback;
	if (!field->AsU64())
		throw std::runtime_error("Invalid recording page identity");
	return *field->AsU64();
}

RecordedGamePage DecodeRecordedGamePage(const control::Message& response)
{
	const auto document = response.Find(wire::tag::Document);
	if (!document || response.opcode != static_cast<std::uint16_t>(wire::OpCode::ReadGameObservations) ||
		!response.Find(wire::tag::Error) || response.Find(wire::tag::Error)->AsU32() != 0)
		throw std::runtime_error("Missing SDK observation page");
	const RecordingNode root(document->value);
	if (root.Member("schema").Unsigned() != 1)
		throw std::runtime_error("Unknown SDK observation schema");
	RecordedGamePage result;
	auto& page = result.journal;
	page.floor = root.Member("floor").Unsigned();
	page.offset = root.Member("offset").Unsigned();
	page.next = root.Member("next").Unsigned();
	page.total = root.Member("total").Unsigned();
	page.gap = root.Member("gap").Boolean();
	page.failed = root.Member("failed").Boolean();
	if (page.floor > page.offset || page.offset > page.next || page.next > page.total || page.next - page.offset > 16)
		throw std::runtime_error("Invalid SDK observation cursors");
	const auto state = root.Member("state");
	page.state.game = state.Member("game_id").Unsigned();
	page.state.revision = state.Member("revision").Unsigned();
	const auto capabilities = state.Member("capabilities").Unsigned();
	if (capabilities > 7)
		throw std::runtime_error("Invalid SDK capabilities");
	page.state.capabilities = static_cast<std::uint32_t>(capabilities);
	page.state.telemetry = state.Member("telemetry").Text();
	if (page.state.telemetry.size() > SIMXR_GAME_PAYLOAD_CAPACITY)
		throw std::runtime_error("SDK state exceeds payload bound");
	for (const auto& entry : root.Member("observations").Elements())
	{
		GameObservation observation;
		observation.sequence = entry.Member("sequence").Unsigned();
		observation.game = entry.Member("game_id").Unsigned();
		observation.revision = entry.Member("revision").Unsigned();
		const auto caps = entry.Member("capabilities").Unsigned();
		if (page.observations.size() >= page.next - page.offset ||
			observation.sequence != page.offset + page.observations.size() || !observation.game || caps > 7)
			throw std::runtime_error("Invalid SDK observation identity");
		observation.capabilities = static_cast<std::uint32_t>(caps);
		const auto kind = entry.Member("kind").Text();
		if (kind == "registered")
			observation.kind = GameObservationKind::Registered;
		else if (kind == "telemetry")
			observation.kind = GameObservationKind::Telemetry;
		else if (kind == "unregistered")
			observation.kind = GameObservationKind::Unregistered;
		else if (kind == "closed")
			observation.kind = GameObservationKind::Closed;
		else
			throw std::runtime_error("Unknown SDK observation kind");
		observation.payload = entry.Member("payload").Text();
		if (observation.payload.size() > SIMXR_GAME_PAYLOAD_CAPACITY)
			throw std::runtime_error("SDK observation exceeds payload bound");
		const auto time = entry.Member("observed_time");
		if (!time.IsNull())
		{
			const auto domain = time.Member("domain").Unsigned();
			NativeClockSample sample{};
			sample.domain = static_cast<NativeClockDomain>(domain);
			sample.nanoseconds = time.Member("nanoseconds").Signed();
			sample.resolutionNanoseconds = time.Member("resolution").Signed();
			if ((domain != 1 && domain != 2) || sample.nanoseconds <= 0 || sample.resolutionNanoseconds <= 0)
				throw std::runtime_error("Invalid SDK observation timestamp");
			observation.time = sample;
		}
		page.observations.push_back(std::move(observation));
	}
	if (page.observations.size() != page.next - page.offset)
		throw std::runtime_error("Incomplete SDK observation page");
	result.session = Number(response, wire::tag::Session, 0);
	result.generation = Number(response, wire::tag::Generation, 0);
	const bool hasClock = response.Find(wire::tag::ClockDomain) || response.Find(wire::tag::ClockRevision) ||
						  response.Find(wire::tag::NativeTime) || response.Find(wire::tag::RuntimeTimeBefore) ||
						  response.Find(wire::tag::RuntimeTimeAfter) || response.Find(wire::tag::ClockResolution);
	if (hasClock)
	{
		const auto signedValue = [&](std::uint16_t tag)
		{
			const auto field = response.Find(tag);
			if (!field || !field->AsI64() || *field->AsI64() <= 0)
				throw std::runtime_error("Invalid SDK clock mapping");
			return *field->AsI64();
		};
		const auto domainField = response.Find(wire::tag::ClockDomain);
		const auto domain = domainField ? domainField->AsU32().value_or(0) : 0;
		FrameNativeClockMapping clock;
		clock.generation = {result.generation};
		clock.revision = Number(response, wire::tag::ClockRevision, 0);
		clock.domain = static_cast<NativeClockDomain>(domain);
		clock.nativeNanoseconds = signedValue(wire::tag::NativeTime);
		clock.runtimeBefore = {Duration{signedValue(wire::tag::RuntimeTimeBefore)}};
		clock.runtimeAfter = {Duration{signedValue(wire::tag::RuntimeTimeAfter)}};
		clock.resolutionNanoseconds = signedValue(wire::tag::ClockResolution);
		if (!result.session || !result.generation || !clock.revision || (domain != 1 && domain != 2) ||
			clock.runtimeBefore > clock.runtimeAfter)
			throw std::runtime_error("Invalid SDK clock correlation identity");
		result.clock = clock;
	}
	return result;
}
RecordingPage DecodeRecordingPage(const control::Message& response)
{
	const auto document = response.Find(control::tag::Document);
	if (!document)
		throw std::runtime_error("Runtime omitted recording metadata");
	const RecordingNode root(document->value);
	RecordingPage result;
	auto& recording = result.recording;
	recording.version = root.Member("version").Unsigned();
	recording.seed = root.Member("seed").Unsigned();
	recording.total = root.Member("total").Unsigned();
	recording.originFrame = FrameId{root.Member("origin_frame").Unsigned()};
	recording.originTime = root.Member("origin_time").Signed();
	if (const auto time = root.Find("origin_runtime_time"); time && !time->IsNull())
	{
		recording.originRuntimeTime = RuntimeTime{Duration{time->Signed()}};
		if (recording.originRuntimeTime->value.count() <= 0)
			throw std::runtime_error("Invalid recording runtime origin");
	}
	recording.active = root.Member("active").Boolean();
	recording.overflow = root.Member("overflow").Boolean();
	recording.initial = Commands(root.Member("initial_commands"), recording.version);
	for (const auto& scenario : root.Member("initial_scenarios").Elements())
	{
		const auto index = scenario.Unsigned();
		if (index >= recording.initialScenarios.size() || recording.initialScenarios[index])
			throw std::runtime_error("Unknown or duplicate recording origin scenario");
		recording.initialScenarios[index] = true;
	}
	for (const auto& entry : root.Member("transactions").Elements())
	{
		RecordedTransaction transaction;
		transaction.id = entry.Member("transaction_id").Unsigned();
		transaction.phase = entry.Member("phase").Text();
		transaction.frame = FrameId{entry.Member("applied_frame").Unsigned()};
		if (const auto time = entry.Find("applied_runtime_time"); time && !time->IsNull())
		{
			transaction.runtimeTime = RuntimeTime{Duration{time->Signed()}};
			if (transaction.runtimeTime->value.count() <= 0)
				throw std::runtime_error("Invalid recording runtime boundary");
		}
		transaction.commands = Commands(entry.Member("commands"), recording.version);
		if (recording.version >= 2)
			transaction.afterFrame = entry.Member("after_frame").Boolean();
		if (const auto released = entry.Find("release_state"); released && !released->IsNull())
		{
			if (released->Member("schema").Unsigned() != 1 ||
				released->Member("frame").Unsigned() != transaction.frame.value ||
				(transaction.phase != "applied" && transaction.phase != "released") ||
				!std::ranges::any_of(transaction.commands,
									 [](const auto& command)
									 {
										 return std::holds_alternative<ReleaseControl>(command);
									 }))
				throw std::runtime_error("Invalid recorded release snapshot identity or phase");
			RecordedReleaseState state;
			state.runtimeTime = RuntimeTime{Duration{released->Member("runtime_time").Signed()}};
			if (state.runtimeTime.value.count() <= 0 || transaction.runtimeTime != state.runtimeTime)
				throw std::runtime_error("Recorded release snapshot changed its runtime anchor");
			state.initial = Commands(released->Member("initial_commands"), 4);
			for (const auto& scenario : released->Member("scenarios").Elements())
			{
				const auto index = scenario.Unsigned();
				if (index >= state.scenarios.size() || state.scenarios[index])
					throw std::runtime_error("Invalid recorded release scenarios");
				state.scenarios[index] = true;
			}
			transaction.releaseState = std::move(state);
		}
		if (const auto live = entry.Find("live_input"); live && !live->IsNull())
		{
			LiveInputCorrelation correlation;
			correlation.connection = live->Member("connection").Unsigned();
			correlation.sequence = live->Member("sequence").Unsigned();
			if (const auto revision = live->Find("clock_revision"))
				correlation.clockRevision = revision->Unsigned();
			correlation.observedTime = {Duration{live->Member("observed_time").Signed()}};
			correlation.targetTime = {Duration{live->Member("target_time").Signed()}};
			const auto controls = live->Member("controls_observed_time").Elements();
			if (recording.version < 2 || transaction.id || transaction.afterFrame || !correlation.connection ||
				!correlation.sequence || correlation.observedTime.value.count() <= 0 ||
				correlation.targetTime < correlation.observedTime || controls.size() != 2 ||
				entry.Member("source").Member("kind").Unsigned() !=
					static_cast<std::uint32_t>(CommandSourceKind::Device))
				throw std::runtime_error("Invalid recorded live-input correlation");
			for (std::size_t hand = 0; hand < 2; ++hand)
			{
				correlation.controlsObservedTime[hand] = {Duration{controls[hand].Signed()}};
				if (correlation.controlsObservedTime[hand].value.count() <= 0 ||
					correlation.controlsObservedTime[hand] > correlation.observedTime)
					throw std::runtime_error("Invalid recorded controller observation time");
			}
			if (const auto trackers = live->Find("trackers_observed_time"); trackers && !trackers->IsNull())
			{
				correlation.trackersObservedTime = RuntimeTime{Duration{trackers->Signed()}};
				if (correlation.trackersObservedTime->value.count() <= 0 ||
					*correlation.trackersObservedTime > correlation.observedTime)
					throw std::runtime_error("Invalid recorded tracker observation time");
			}
			transaction.liveInput = correlation;
		}
		recording.transactions.push_back(std::move(transaction));
	}
	if (const auto identity = root.Find("recording_id"))
		result.identity = identity->Unsigned();
	if (const auto drain = root.Find("drain"))
		result.drain = drain->Boolean();
	if (const auto floor = root.Find("floor"))
		result.floor = floor->Unsigned();
	result.offset = Number(response, control::tag::Offset, result.floor);
	if (result.floor > recording.total || (result.drain && !result.identity) || (!result.drain && result.floor) ||
		result.offset < result.floor)
		throw std::runtime_error("Invalid recording drain identity or window");
	if ((recording.version < 1 || recording.version > 4) || (!result.drain && recording.total > 1024) ||
		result.offset > recording.total || recording.transactions.size() > recording.total - result.offset ||
		Number(response, control::tag::Length, recording.transactions.size()) != recording.transactions.size())
		throw std::runtime_error("Unsupported or invalid recording page");
	result.end = result.offset + recording.transactions.size() == recording.total;
	if (const auto end = response.Find(control::tag::End); end && end->AsBool() != result.end)
		throw std::runtime_error("Recording page end flag differs from its transaction count");
	return result;
}
void RecordingCollection::Append(RecordingPage page)
{
	if (page.drain)
		throw std::invalid_argument("Draining pages require a streaming archive consumer");
	if ((page.recording.version < 1 || page.recording.version > 4) || page.recording.total > 1024 || m_complete ||
		page.recording.active || page.recording.overflow || page.offset != Offset() ||
		(!page.end && page.recording.transactions.empty()))
		throw std::runtime_error("Recording must be stopped, complete and collected in order");
	auto identity = CommandIdentity(page.recording.initial);
	if (m_recording)
	{
		const auto& previous = *m_recording;
		const auto& incoming = page.recording;
		if (previous.version != incoming.version || previous.seed != incoming.seed ||
			previous.total != incoming.total || previous.originFrame != incoming.originFrame ||
			previous.originTime != incoming.originTime || previous.originRuntimeTime != incoming.originRuntimeTime ||
			previous.initialScenarios != incoming.initialScenarios || identity != m_initialIdentity)
			throw std::runtime_error("Recording changed while its pages were collected");
	}
	std::size_t bytes = 0;
	const auto charge = [&](std::size_t size)
	{
		if (size > 64 * 1024 * 1024 - m_bytes - bytes)
			throw std::runtime_error("Recording collection exceeds its byte bound");
		bytes += size;
	};
	if (!m_recording)
		charge(identity.size());
	for (const auto& transaction : page.recording.transactions)
	{
		charge(transaction.phase.size());
		charge(CommandIdentity(transaction.commands).size());
		if (transaction.releaseState)
			charge(CommandIdentity(transaction.releaseState->initial).size());
	}
	auto next = m_recording ? *m_recording : page.recording;
	if (m_recording)
		next.transactions.insert(next.transactions.end(), page.recording.transactions.begin(),
								 page.recording.transactions.end());
	if (next.transactions.size() > next.total || page.end != (next.transactions.size() == next.total))
		throw std::runtime_error("Recording collection has inconsistent pagination");
	auto prepared = std::make_optional(std::move(next));
	m_recording.swap(prepared);
	m_initialIdentity.swap(identity);
	m_bytes += bytes;
	m_complete = page.end;
}
std::uint64_t RecordingCollection::Offset() const noexcept
{
	return m_recording ? m_recording->transactions.size() : 0;
}
bool RecordingCollection::Complete() const noexcept
{
	return m_complete;
}
const InputRecording& RecordingCollection::Recording() const
{
	if (!m_complete)
		throw std::logic_error("Recording collection is incomplete");
	return *m_recording;
}
} // namespace simxr::automation
