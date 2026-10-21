#include "app/SimulationEdits.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

#include <limits>
#include <stdexcept>

namespace simxr::app
{
namespace
{
namespace wire = control;

wire::Message DecodeNode(std::span<const std::byte> bytes)
{
	auto decoded = wire::DecodeFields(bytes);
	if (decoded.error != wire::DecodeError::None)
		throw std::runtime_error("Runtime returned malformed simulation state");
	wire::Message result;
	result.fields = std::move(decoded.fields);
	return result;
}
const wire::Field& NodeData(const wire::Message& node, wire::InspectionType type)
{
	const auto kind = node.Find(2);
	const auto data = node.Find(3);
	if (!kind || kind->AsU32() != static_cast<std::uint32_t>(type) || !data)
		throw std::runtime_error("Runtime returned an invalid simulation state field");
	return *data;
}
FrameId NextObservedFrame(const wire::Message& state)
{
	const auto document = state.Find(wire::tag::Document);
	if (!document)
		throw std::runtime_error("Wait for observed simulation state before scheduling an edit");
	const auto root = DecodeNode(document->value);
	const auto members = DecodeNode(NodeData(root, wire::InspectionType::Object).value);
	std::optional<bool> closed;
	std::optional<std::uint64_t> frame;
	for (const auto& member : members.fields)
	{
		const auto node = DecodeNode(member.value);
		const auto name = node.Find(1);
		if (!name || !name->AsText())
			throw std::runtime_error("Runtime returned an unnamed simulation state field");
		if (name->AsText() == "closed")
		{
			if (closed)
				throw std::runtime_error("Runtime repeated the simulation state flag");
			closed = NodeData(node, wire::InspectionType::Boolean).AsBool();
			if (!closed)
				throw std::runtime_error("Runtime returned an invalid simulation state flag");
		}
		else if (name->AsText() == "frame_id")
		{
			if (frame)
				throw std::runtime_error("Runtime repeated the simulation frame identity");
			frame = NodeData(node, wire::InspectionType::Unsigned).AsU64();
			if (!frame)
				throw std::runtime_error("Runtime returned an invalid simulation frame identity");
		}
	}
	if (!closed || *closed)
		throw std::runtime_error("Simulation state is unavailable or closed");
	// The scheduler has no applied snapshot before its first publication.
	const auto previous = frame.value_or(0);
	if (previous == std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("Simulation frame identity is exhausted");
	return FrameId{previous + 1};
}
} // namespace

control::ControlPacket EncodeSimulationEdit(const SimulationEdit& edit, std::uint64_t session, std::uint64_t generation,
											std::uint64_t transaction, const control::Message& observedState)
{
	if (!session || !generation)
		throw std::invalid_argument("Simulation edits require a current session generation");
	control::ControlPacket packet;
	auto& message = packet.message;
	message.fields = {
		control::Field::U64(control::tag::Session, session, true),
		control::Field::U64(control::tag::Generation, generation, true),
		control::Field::U32(control::tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Manual))};
	if (std::holds_alternative<ReleaseManualInputs>(edit))
	{
		message.opcode = static_cast<std::uint16_t>(control::OpCode::ReleaseControl);
		return packet;
	}
	const auto& manual = std::get<ManualTransaction>(edit);
	if (!transaction || manual.commands.empty() || manual.commands.size() > 64 ||
		(manual.frame && !manual.frame->value))
		throw std::invalid_argument("Simulation transaction requires a nonzero identity, target and commands");
	const auto frame = manual.frame ? *manual.frame : NextObservedFrame(observedState);
	message.opcode = static_cast<std::uint16_t>(control::OpCode::SubmitSimulation);
	message.fields.push_back(control::Field::U64(control::tag::TransactionId, transaction, true));
	message.fields.push_back(control::Field::U64(control::tag::ApplyFrame, frame.value, true));
	message.fields.push_back(
		control::Field::U32(control::tag::LatePolicy, static_cast<std::uint32_t>(manual.latePolicy)));
	std::vector<control::Field> commands;
	commands.reserve(manual.commands.size());
	for (const auto& command : manual.commands)
		commands.push_back(control::EncodeSimulationCommand(command, static_cast<std::uint16_t>(commands.size() + 1)));
	message.fields.push_back(control::Field::Bytes(control::tag::Commands, control::EncodeFields(commands), true));
	return packet;
}
} // namespace simxr::app
