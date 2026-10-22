#include "app/SimulationEdits.h"
#include "Math.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "simulation/BodyModel.h"
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
std::optional<wire::Message> FindMember(const wire::Message& node, std::string_view name)
{
	for (const auto& field : DecodeNode(NodeData(node, wire::InspectionType::Object).value).fields)
	{
		auto child = DecodeNode(field.value);
		if (const auto key = child.Find(1); key && key->AsText() == name)
			return child;
	}
	return {};
}
wire::Message Member(const wire::Message& node, std::string_view name)
{
	if (auto child = FindMember(node, name))
		return *child;
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
std::uint64_t Unsigned(const wire::Message& node)
{
	const auto value = NodeData(node, wire::InspectionType::Unsigned).AsU64();
	if (!value)
		throw std::runtime_error("Invalid unsigned configuration value");
	return *value;
}
bool Boolean(const wire::Message& node)
{
	const auto value = NodeData(node, wire::InspectionType::Boolean).AsBool();
	if (!value)
		throw std::runtime_error("Invalid boolean configuration value");
	return *value;
}
bool Null(const wire::Message& node)
{
	const auto type = node.Find(2);
	return type && type->AsU32() == static_cast<std::uint32_t>(wire::InspectionType::Null);
}
XrPosef ReadPose(const wire::Message& node)
{
	const auto p = Elements(Member(node, "position")), q = Elements(Member(node, "orientation"));
	if (p.size() != 3 || q.size() != 4)
		throw std::runtime_error("Invalid configuration pose dimensions");
	const auto pose = NormalizedPose(
		{{Number(q[0]), Number(q[1]), Number(q[2]), Number(q[3])}, {Number(p[0]), Number(p[1]), Number(p[2])}});
	if (!pose)
		throw std::runtime_error("Invalid configuration orientation");
	return *pose;
}
PoseSample ReadSample(const wire::Message& node)
{
	return {ReadPose(node), Unsigned(Member(node, "flags"))};
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

std::optional<CommandSource> SimulationEditSource(const control::Message& receipt)
{
	const auto document = receipt.Find(wire::tag::Document);
	if (!document)
		return {};
	const auto source = FindMember(DecodeNode(document->value), "source");
	if (!source)
		return {};
	return CommandSource{static_cast<CommandSourceKind>(Unsigned(Member(*source, "kind"))),
						 Unsigned(Member(*source, "id"))};
}
std::optional<TrackingSnapshot> ReadInspectionTracking(const control::Message& state)
{
	const auto document = state.Find(wire::tag::Document);
	if (!document)
		return {};
	const auto tracking = FindMember(DecodeNode(document->value), "tracking");
	if (!tracking || Null(*tracking))
		return {};
	const auto sample = [](const wire::Message& node)
	{
		const auto pose = Member(node, "pose"), position = Member(pose, "position"),
				   orientation = Member(pose, "orientation");
		const auto normalized = NormalizedPose(
			{{Number(Member(orientation, "x")), Number(Member(orientation, "y")), Number(Member(orientation, "z")),
			  Number(Member(orientation, "w"))},
			 {Number(Member(position, "x")), Number(Member(position, "y")), Number(Member(position, "z"))}});
		if (!normalized)
			throw std::runtime_error("Invalid historical device pose");
		return PoseSample{*normalized, Unsigned(Member(node, "flags"))};
	};
	TrackingSnapshot result;
	result.head = sample(Member(*tracking, "head"));
	result.gaze = sample(Member(*tracking, "gaze"));
	const auto controllers = Elements(Member(*tracking, "controllers")),
			   trackers = Elements(Member(*tracking, "trackers"));
	if (controllers.size() != 2 || trackers.size() != 4)
		throw std::runtime_error("Invalid historical device count");
	for (std::size_t hand = 0; hand < 2; ++hand)
		result.controllers[hand] = {sample(Member(controllers[hand], "sample")),
									Boolean(Member(controllers[hand], "connected"))};
	for (std::size_t slot = 0; slot < 4; ++slot)
	{
		result.trackers[slot].pose = sample(Member(trackers[slot], "sample"));
		result.trackers[slot].device.connected = Boolean(Member(Member(trackers[slot], "device"), "connected"));
	}
	const auto proportions = Member(*tracking, "proportions");
	result.proportions = {Number(Member(proportions, "height")), Number(Member(proportions, "shoulder_width")),
						  Number(Member(proportions, "hip_width"))};
	const auto body = Member(*tracking, "body");
	const auto joints = Elements(Member(body, "joints"));
	if (joints.size() != BodyJointCount)
		throw std::runtime_error("Invalid historical joint count");
	result.body.active = Boolean(Member(body, "active"));
	const BodyModel model(result.proportions);
	std::copy(model.Skeleton().begin(), model.Skeleton().end(), result.body.skeleton.begin());
	for (std::size_t joint = 0; joint < joints.size(); ++joint)
		result.body.joints[joint] = sample(Member(joints[joint], "sample"));
	return result;
}
ObservedSetup ReadObservedSetup(const control::Message& state)
{
	NextObservedFrame(state);
	const auto root = DecodeNode(state.Find(wire::tag::Document)->value);
	if (!Boolean(Member(root, "configuration_available")))
		throw std::runtime_error("The runtime has not published device configuration yet");
	const auto config = Member(root, "configuration");
	ObservedSetup result;
	auto& values = result.values;
	result.frame = Unsigned(Member(root, "frame_id"));
	if (const auto trackers = FindMember(root, "tracker_interaction"))
		result.appTrackers = Boolean(*trackers);
	values.tracking.head = ReadSample(Member(config, "head"));
	values.tracking.focused = Boolean(Member(config, "focused"));
	const auto hands = Elements(Member(config, "controllers"));
	if (hands.size() != 2)
		throw std::runtime_error("Unsupported controller configuration");
	for (std::size_t index = 0; index < hands.size(); ++index)
	{
		values.tracking.controllers[index] = {ReadSample(Member(hands[index], "pose")),
											  Boolean(Member(hands[index], "connected"))};
		values.input.preferredProfiles[index] =
			std::string(NodeData(Member(hands[index], "profile"), wire::InspectionType::Text).AsText().value_or(""));
		const auto controls = Member(hands[index], "controls");
		auto& out = values.input.controllers[index];
		for (const auto& [name, field] : {std::pair{"trigger", &ControllerControls::trigger},
										  {"grip", &ControllerControls::grip},
										  {"grip_force", &ControllerControls::gripForce},
										  {"trackpad_force", &ControllerControls::trackpadForce}})
			out.*field = Number(Member(controls, name));
		for (const auto& [name, field] : {std::pair{"primary", &ControllerControls::primary},
										  {"secondary", &ControllerControls::secondary},
										  {"menu", &ControllerControls::menu},
										  {"back", &ControllerControls::back},
										  {"thumb_click", &ControllerControls::thumbClick},
										  {"trackpad_click", &ControllerControls::trackpadClick},
										  {"primary_touch", &ControllerControls::primaryTouch},
										  {"secondary_touch", &ControllerControls::secondaryTouch},
										  {"trigger_touch", &ControllerControls::triggerTouch},
										  {"thumb_touch", &ControllerControls::thumbTouch},
										  {"trackpad_touch", &ControllerControls::trackpadTouch},
										  {"thumbrest_touch", &ControllerControls::thumbrestTouch},
										  {"connected", &ControllerControls::connected}})
			out.*field = Boolean(Member(controls, name));
		for (const auto& [name, field] :
			 {std::pair{"stick", &ControllerControls::stick}, {"trackpad", &ControllerControls::trackpad}})
		{
			const auto xy = Elements(Member(controls, name));
			if (xy.size() != 2)
				throw std::runtime_error("Invalid controller axis dimensions");
			out.*field = {Number(xy[0]), Number(xy[1])};
		}
	}
	const auto body = Member(config, "body"), gaze = Member(config, "gaze"),
			   proportions = Member(config, "proportions");
	values.tracking.body.active = Boolean(Member(body, "active"));
	const auto bodyRoot = FindMember(body, "root_pose");
	result.bodyRootAvailable = bodyRoot.has_value();
	if (bodyRoot && !Null(*bodyRoot))
		values.tracking.body.rootPose = ReadPose(*bodyRoot);
	values.tracking.body.confidence = Number(Member(body, "confidence"));
	values.tracking.body.preset = static_cast<BodyPreset>(Unsigned(Member(body, "preset")));
	const auto bodyControls = FindMember(body, "controls"), bodyJoints = FindMember(body, "joints");
	result.bodyOverridesAvailable = bodyControls && bodyJoints;
	if (result.bodyOverridesAvailable)
	{
		const auto controls = Elements(*bodyControls);
		if (controls.size() != values.tracking.body.controls.size())
			throw std::runtime_error("Unsupported body controls");
		for (std::size_t index = 0; index < controls.size(); ++index)
		{
			auto& out = values.tracking.body.controls[index];
			out.tracked = Boolean(Member(controls[index], "tracked"));
			const auto offset = Member(controls[index], "offset");
			if (!Null(offset))
			{
				const auto xyz = Elements(offset);
				if (xyz.size() != 3)
					throw std::runtime_error("Invalid body offset dimensions");
				out.offset = XrVector3f{Number(xyz[0]), Number(xyz[1]), Number(xyz[2])};
			}
		}
		for (const auto& joint : Elements(*bodyJoints))
		{
			const auto index = Unsigned(Member(joint, "joint"));
			if (index >= values.tracking.body.joints.size())
				throw std::runtime_error("Unknown body joint");
			BodyJointOverride out;
			out.tracked = Boolean(Member(joint, "tracked"));
			const auto pose = Member(joint, "pose");
			if (!Null(pose))
				out.pose = ReadPose(pose);
			values.tracking.body.joints[index] = out;
		}
	}
	values.tracking.gaze = {Number(Member(gaze, "yaw")), Number(Member(gaze, "pitch")),
							Boolean(Member(gaze, "available")), Boolean(Member(gaze, "tracked"))};
	values.proportions = {Number(Member(proportions, "height")), Number(Member(proportions, "shoulder_width")),
						  Number(Member(proportions, "hip_width"))};
	const auto views = Member(config, "views");
	values.views.ipd = Number(Member(views, "ipd"));
	values.views.insetHalfAngle = Number(Member(views, "inset_half_angle"));
	for (const auto& [name, field] : {std::pair{"outer_width", &ViewSettings::outerWidth},
									  {"outer_height", &ViewSettings::outerHeight},
									  {"inset_width", &ViewSettings::insetWidth},
									  {"inset_height", &ViewSettings::insetHeight}})
	{
		const auto size = Unsigned(Member(views, name));
		if (size > std::numeric_limits<std::uint32_t>::max())
			throw std::runtime_error("Invalid view dimension");
		values.views.*field = static_cast<std::uint32_t>(size);
	}
	const auto fov = Elements(Member(views, "outer_fov"));
	if (fov.size() != 8)
		throw std::runtime_error("Unsupported view configuration");
	for (std::size_t eye = 0; eye < 2; ++eye)
		values.views.outerFov[eye] = {Number(fov[eye * 4]), Number(fov[eye * 4 + 1]), Number(fov[eye * 4 + 2]),
									  Number(fov[eye * 4 + 3])};
	const auto trackers = Elements(Member(config, "trackers"));
	if (trackers.size() != values.tracking.trackers.size())
		throw std::runtime_error("Unsupported tracker configuration");
	for (std::size_t index = 0; index < trackers.size(); ++index)
	{
		const auto& node = trackers[index];
		auto& out = values.tracking.trackers[index];
		out.identity.value = Unsigned(Member(node, "id"));
		out.role = static_cast<TrackerRole>(Unsigned(Member(node, "role")));
		out.connected = Boolean(Member(node, "connected"));
		out.tracked = Boolean(Member(node, "tracked"));
		const auto anchor = FindMember(node, "anchor"), pose = FindMember(node, "pose_override");
		if (!anchor || !pose)
			result.trackerAttachmentsAvailable = false;
		out.anchor.reset();
		out.jointOffset.reset();
		if (const auto offset = FindMember(node, "joint_offset"); offset && !Null(*offset))
			out.jointOffset = ReadPose(*offset);
		out.poseOverride.reset();
		if (anchor && !Null(*anchor))
			out.anchor = static_cast<BodyJoint>(Unsigned(*anchor));
		if (pose && !Null(*pose))
			out.poseOverride = ReadPose(*pose);
	}
	const auto spaces = Member(config, "reference_spaces");
	values.referenceSpaces.local = ReadPose(Member(spaces, "local"));
	values.referenceSpaces.stage = ReadPose(Member(spaces, "stage"));
	const auto bounds = Member(spaces, "stage_bounds");
	values.referenceSpaces.stageBounds.reset();
	if (!Null(bounds))
	{
		const auto dimensions = Elements(bounds);
		if (dimensions.size() != 2)
			throw std::runtime_error("Invalid stage bounds");
		values.referenceSpaces.stageBounds = XrExtent2Df{Number(dimensions[0]), Number(dimensions[1])};
	}
	const auto owners = Elements(Member(root, "owners"));
	if (owners.size() != result.owners.size())
		throw std::runtime_error("Unsupported ownership configuration");
	for (std::size_t index = 0; index < owners.size(); ++index)
		if (!Null(owners[index]))
			result.owners[index] =
				CommandSource{static_cast<CommandSourceKind>(Unsigned(Member(owners[index], "kind"))),
							  Unsigned(Member(owners[index], "id"))};
	for (const auto& scenario : Elements(Member(root, "scenarios")))
	{
		const auto index = Unsigned(scenario);
		if (index >= result.scenarios.size())
			throw std::runtime_error("Unsupported simulation scenario");
		result.scenarios[index] = true;
	}
	if (const auto bound = FindMember(root, "bound_trackers"))
		for (const auto& identity : Elements(*bound))
			for (std::size_t slot = 0; slot < values.tracking.trackers.size(); ++slot)
				if (values.tracking.trackers[slot].identity.value == Unsigned(identity))
					result.boundTrackers[slot] = true;
	return result;
}
void UndoStack::Record(const ObservedSetup& before, const ManualTransaction& edit)
{
	const bool pose = std::any_of(edit.commands.begin(), edit.commands.end(),
								  [](const SimulationCommand& command)
								  {
									  return std::holds_alternative<SetHeadPose>(command) ||
											 std::holds_alternative<SetControllerPose>(command) ||
											 std::holds_alternative<SetBodyControls>(command) ||
											 std::holds_alternative<SetGaze>(command) ||
											 std::holds_alternative<SetTracker>(command) ||
											 std::holds_alternative<SetBodyProportions>(command);
								  });
	if (!pose)
		return;
	// The first edit of a drag opens the group; its continuous updates and final edit join it.
	if (!m_open)
	{
		constexpr std::size_t capacity = 64;
		if (m_entries.size() == capacity)
			m_entries.erase(m_entries.begin());
		m_entries.push_back({before.values.tracking, before.values.proportions});
	}
	m_open = edit.continuous;
}
void UndoStack::Clear()
{
	m_entries.clear();
	m_open = false;
}
std::optional<ManualTransaction> UndoStack::Undo(const ObservedSetup& current, std::optional<CommandSource> source)
{
	m_open = false;
	if (m_entries.empty())
		return {};
	const auto entry = std::move(m_entries.back());
	m_entries.pop_back();
	ManualTransaction result;
	const auto allowed = [&](ControlChannel channel)
	{
		const auto& owner = current.owners[static_cast<std::size_t>(channel)];
		return !owner || owner == source;
	};
	const auto restore = [&](ControlChannel channel, SimulationCommand command)
	{
		if (!allowed(channel))
			return;
		result.commands.emplace_back(TakeControl{channel, false});
		result.commands.push_back(std::move(command));
	};
	restore(ControlChannel::Head, SetHeadPose{entry.tracking.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
		restore(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
				SetControllerPose{hand, entry.tracking.controllers[hand]});
	auto body = entry.tracking.body;
	if (!current.bodyRootAvailable)
		body.rootPose.reset();
	restore(ControlChannel::Body, SetBodyControls{body});
	restore(ControlChannel::Gaze, SetGaze{entry.tracking.gaze});
	if (current.trackerAttachmentsAvailable && allowed(ControlChannel::Trackers))
	{
		result.commands.emplace_back(TakeControl{ControlChannel::Trackers, false});
		for (std::uint32_t slot = 0; slot < entry.tracking.trackers.size(); ++slot)
			result.commands.emplace_back(SetTracker{slot, entry.tracking.trackers[slot]});
	}
	restore(ControlChannel::Proportions, SetBodyProportions{entry.proportions});
	if (result.commands.empty())
		return {};
	return result;
}
ControlChannel ScenarioInputChannel(FailureScenario scenario)
{
	switch (scenario)
	{
	case FailureScenario::HeadUntracked:
	case FailureScenario::HeadUnavailable:
		return ControlChannel::Head;
	case FailureScenario::BodyUntracked:
	case FailureScenario::BodyUnavailable:
		return ControlChannel::Body;
	case FailureScenario::GazeUntracked:
	case FailureScenario::GazeUnavailable:
		return ControlChannel::Gaze;
	case FailureScenario::LeftDisconnected:
		return ControlChannel::LeftPose;
	case FailureScenario::RightDisconnected:
		return ControlChannel::RightPose;
	case FailureScenario::TrackersDisconnected:
		return ControlChannel::Trackers;
	default:
		return ControlChannel::Session;
	}
}

std::string SimulationEditPhase(const control::Message& result)
{
	const auto document = result.Find(wire::tag::Document);
	if (!document)
		return {};
	const auto phase = Member(DecodeNode(document->value), "phase");
	return std::string(NodeData(phase, wire::InspectionType::Text).AsText().value_or("invalid"));
}
std::optional<control::Message> ObservedEditResult(const control::Message& state, const control::Message& receipt)
{
	const auto document = state.Find(wire::tag::Document), received = receipt.Find(wire::tag::Document);
	if (!document || !received)
		return {};
	const auto expected = DecodeNode(received->value);
	const auto identity = [](const wire::Message& node, std::string_view key)
	{
		return NodeData(Member(node, key), wire::InspectionType::Unsigned).AsU64();
	};
	for (const auto& outcome : Elements(Member(DecodeNode(document->value), "transactions")))
		if (identity(outcome, "transaction_id") == identity(expected, "transaction_id") &&
			identity(Member(outcome, "source"), "id") == identity(Member(expected, "source"), "id"))
		{
			wire::Message result;
			result.fields = {wire::Field::Bytes(wire::tag::Document, wire::EncodeFields(outcome.fields))};
			return result;
		}
	return {};
}
PoseSample ObservedDevicePose(const control::Message& state, ControlChannel channel)
{
	NextObservedFrame(state);
	const auto root = DecodeNode(state.Find(wire::tag::Document)->value);
	if (const auto scenarios = FindMember(root, "scenarios"))
		for (const auto& scenario : Elements(*scenarios))
			if (ScenarioInputChannel(static_cast<FailureScenario>(Unsigned(scenario))) == channel)
				throw std::runtime_error("Restore the active failure scenario before piloting this device");
	auto configuration = Member(root, "configuration");
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
