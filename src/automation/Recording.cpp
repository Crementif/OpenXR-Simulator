#include "automation/Recording.h"
#include "control/ControlSchema.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"

#include <charconv>
#include <span>
#include <stdexcept>
#include <utility>

namespace simxr::automation
{
namespace
{
namespace wire = control;
class Node
{
  public:
	explicit Node(std::span<const std::byte> bytes)
	{
		auto decoded = wire::DecodeFields(bytes);
		if (decoded.error != wire::DecodeError::None)
			throw std::runtime_error("Malformed recording inspection fields");
		m_fields.fields = std::move(decoded.fields);
	}
	Node Member(std::string_view name) const
	{
		const auto members = Fields(Data(wire::InspectionType::Object).value);
		for (const auto& field : members)
		{
			Node child(field.value);
			const auto label = child.m_fields.Find(1);
			if (!label || !label->AsText())
				throw std::runtime_error("Unnamed recording field");
			if (*label->AsText() == name)
				return child;
		}
		throw std::runtime_error("Missing recording field: " + std::string(name));
	}
	std::vector<Node> Elements() const
	{
		std::vector<Node> result;
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
std::vector<SimulationCommand> Commands(const Node& array)
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
		if (wire::EncodeSimulationCommand(command).value != field.value)
			throw std::runtime_error("Recorded command cannot be exported without losing fields");
		commands.push_back(std::move(command));
	}
	return commands;
}
std::vector<std::byte> CommandIdentity(std::span<const SimulationCommand> commands)
{
	std::vector<wire::Field> fields;
	for (const auto& command : commands)
		fields.push_back(wire::EncodeSimulationCommand(command, static_cast<std::uint16_t>(fields.size() + 1)));
	return wire::EncodeFields(fields);
}
std::uint64_t Number(const wire::Message& response, std::uint16_t name, std::uint64_t fallback)
{
	const auto field = response.Find(name);
	if (!field)
		return fallback;
	if (!field->AsU64())
		throw std::runtime_error("Invalid recording page identity");
	return *field->AsU64();
}
} // namespace
RecordingPage DecodeRecordingPage(const control::Message& response)
{
	const auto document = response.Find(control::tag::Document);
	if (!document)
		throw std::runtime_error("Runtime omitted recording metadata");
	const Node root(document->value);
	RecordingPage result;
	auto& recording = result.recording;
	recording.version = root.Member("version").Unsigned();
	recording.seed = root.Member("seed").Unsigned();
	recording.total = root.Member("total").Unsigned();
	recording.originFrame = FrameId{root.Member("origin_frame").Unsigned()};
	recording.originTime = root.Member("origin_time").Signed();
	recording.active = root.Member("active").Boolean();
	recording.overflow = root.Member("overflow").Boolean();
	recording.initial = Commands(root.Member("initial_commands"));
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
		transaction.commands = Commands(entry.Member("commands"));
		recording.transactions.push_back(std::move(transaction));
	}
	result.offset = Number(response, control::tag::Offset, 0);
	if (recording.version != 1 || recording.total > 1024 || result.offset > recording.total ||
		recording.transactions.size() > recording.total - result.offset ||
		Number(response, control::tag::Length, recording.transactions.size()) != recording.transactions.size())
		throw std::runtime_error("Unsupported or invalid recording page");
	result.end = result.offset + recording.transactions.size() == recording.total;
	if (const auto end = response.Find(control::tag::End); end && end->AsBool() != result.end)
		throw std::runtime_error("Recording page end flag differs from its transaction count");
	return result;
}
void RecordingCollection::Append(RecordingPage page)
{
	if (page.recording.version != 1 || page.recording.total > 1024 || m_complete || page.recording.active ||
		page.recording.overflow || page.offset != Offset() || (!page.end && page.recording.transactions.empty()))
		throw std::runtime_error("Recording must be stopped, complete and collected in order");
	auto identity = CommandIdentity(page.recording.initial);
	if (m_recording)
	{
		const auto& previous = *m_recording;
		const auto& incoming = page.recording;
		if (previous.version != incoming.version || previous.seed != incoming.seed ||
			previous.total != incoming.total || previous.originFrame != incoming.originFrame ||
			previous.originTime != incoming.originTime || previous.initialScenarios != incoming.initialScenarios ||
			identity != m_initialIdentity)
			throw std::runtime_error("Recording changed while its pages were collected");
	}
	auto next = m_recording ? *m_recording : page.recording;
	if (m_recording)
		next.transactions.insert(next.transactions.end(), page.recording.transactions.begin(),
								 page.recording.transactions.end());
	std::size_t bytes = 0;
	for (const auto& transaction : page.recording.transactions)
		bytes += CommandIdentity(transaction.commands).size();
	if (bytes > 64 * 1024 * 1024 - m_bytes || next.transactions.size() > next.total ||
		page.end != (next.transactions.size() == next.total))
		throw std::runtime_error("Recording collection exceeds its bound or has inconsistent pagination");
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
