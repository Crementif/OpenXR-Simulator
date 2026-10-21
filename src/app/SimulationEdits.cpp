#include "app/SimulationEdits.h"
#include "Math.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include <algorithm>
#include <cmath>
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
wire::Message Member(const wire::Message& node, std::string_view name)
{
	for (const auto& field : DecodeNode(NodeData(node, wire::InspectionType::Object).value).fields)
	{
		auto child = DecodeNode(field.value);
		if (const auto key = child.Find(1); key && key->AsText() == name)
			return child;
	}
	throw std::runtime_error("Observed device state is not available yet");
}
std::vector<wire::Message> Elements(const wire::Message& node)
{
	std::vector<wire::Message> elements;
	for (const auto& page : DecodeNode(NodeData(node, wire::InspectionType::Array).value).fields)
		for (const auto& field : DecodeNode(page.value).fields)
			elements.push_back(DecodeNode(field.value));
	return elements;
}
float Number(const wire::Message& node)
{
	const auto number = NodeData(node, wire::InspectionType::Float).AsF32();
	if (!number || !std::isfinite(*number))
		throw std::runtime_error("Observed device pose contains an invalid number");
	return *number;
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

PoseSample ObservedDevicePose(const control::Message& state, ControlChannel channel)
{
	NextObservedFrame(state);
	auto configuration = Member(DecodeNode(state.Find(wire::tag::Document)->value), "configuration");
	wire::Message sample;
	if (channel == ControlChannel::Head)
		sample = Member(configuration, "head");
	else if (channel == ControlChannel::LeftPose || channel == ControlChannel::RightPose)
	{
		const auto hands = Elements(Member(configuration, "controllers"));
		const auto hand = channel == ControlChannel::LeftPose ? 0u : 1u;
		if (hands.size() <= hand ||
			NodeData(Member(hands[hand], "connected"), wire::InspectionType::Boolean).AsBool() != true)
			throw std::runtime_error("Connect the selected controller before piloting it");
		sample = Member(hands[hand], "pose");
	}
	else
		throw std::invalid_argument("This input channel does not have a pilotable pose");
	const auto position = Elements(Member(sample, "position"));
	const auto rotation = Elements(Member(sample, "orientation"));
	if (position.size() != 3 || rotation.size() != 4)
		throw std::runtime_error("Observed device pose has invalid dimensions");
	PoseSample pose;
	pose.pose = {{Number(rotation[0]), Number(rotation[1]), Number(rotation[2]), Number(rotation[3])},
				 {Number(position[0]), Number(position[1]), Number(position[2])}};
	const auto normalized = NormalizedPose(pose.pose);
	if (!normalized)
		throw std::runtime_error("Observed device orientation is invalid");
	pose.pose = *normalized;
	pose.flags = NodeData(Member(sample, "flags"), wire::InspectionType::Unsigned).AsU64().value_or(0);
	if ((pose.flags & ValidPoseFlags) != ValidPoseFlags)
		throw std::runtime_error("Restore valid tracking before piloting this device");
	return pose;
}
PoseSample PilotPose(PoseSample pose, XrVector3f movement, float yaw, float pitch, float seconds, float speed)
{
	const auto forward = Rotate(pose.pose.orientation, {0, 0, -1});
	const float currentPitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
	pitch = std::clamp(currentPitch + pitch, -1.55334f, 1.55334f) - currentPitch;
	pose.pose.orientation = Multiply(Multiply(EulerAngles(yaw, 0, 0), pose.pose.orientation), EulerAngles(0, pitch, 0));
	const float length = std::sqrt(movement.x * movement.x + movement.y * movement.y + movement.z * movement.z);
	const float elapsed = std::clamp(seconds, 0.0f, 0.05f);
	const float distance = elapsed * speed / std::max(1.0f, length);
	const auto offset = Rotate(pose.pose.orientation, {movement.x * distance, 0, movement.z * distance});
	pose.pose.position.x += offset.x;
	pose.pose.position.y += offset.y + movement.y * distance;
	pose.pose.position.z += offset.z;
	if (const auto normalized = NormalizedPose(pose.pose))
		pose.pose = *normalized;
	return pose;
}

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
