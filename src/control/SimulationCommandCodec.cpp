#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace simxr::control
{
namespace
{
enum class SimulationCommandKind : std::uint32_t
{
	TakeControl = 1,
	ReleaseControl = 2,
	HeadPose = 3,
	ControllerPose = 4,
	ControllerControls = 5,
	BodyControls = 6,
	Gaze = 7,
	Tracker = 8,
	ViewSettings = 9,
	BodyProportions = 10,
	ControllerProfile = 11,
	Scenario = 12,
	ReferenceSpaces = 13
};
struct CommandPayload
{
	SimulationCommandKind kind;
	std::vector<Field> fields;
};
template <class Value> Value RequireValue(std::optional<Value> value)
{
	if (!value)
		throw std::invalid_argument("Invalid command field value");
	return *value;
}
const Field& RequireField(const Message& payload, std::uint16_t name)
{
	const auto* field = payload.Find(name);
	if (!field)
		throw std::invalid_argument("Missing command field");
	return *field;
}
Message DecodePayload(const Field& field, std::initializer_list<std::uint16_t> known)
{
	auto decoded = DecodeFields(field.value);
	if (decoded.error != DecodeError::None)
		throw std::invalid_argument("Invalid command fields");
	Message payload;
	payload.fields = std::move(decoded.fields);
	if (payload.ValidateFields(std::span{known.begin(), known.size()}))
		throw std::invalid_argument("Unknown required command field");
	return payload;
}
std::vector<Field> DecodeOrderedFields(const Field& field)
{
	auto decoded = DecodeFields(field.value);
	if (decoded.error != DecodeError::None)
		throw std::invalid_argument("Invalid ordered command fields");
	for (std::size_t index = 0; index < decoded.fields.size(); ++index)
		if (decoded.fields[index].tag != index + 1)
			throw std::invalid_argument("Invalid command field index");
	return std::move(decoded.fields);
}
std::uint64_t ReadU64(const Message& payload, std::uint16_t name)
{
	return RequireValue(RequireField(payload, name).AsU64());
}
std::uint32_t ReadU32(const Message& payload, std::uint16_t name)
{
	return RequireValue(RequireField(payload, name).AsU32());
}
bool ReadBoolean(const Message& payload, std::uint16_t name)
{
	return RequireValue(RequireField(payload, name).AsBool());
}
float ReadFloat(const Field& field)
{
	const auto value = RequireValue(field.AsF32());
	if (!std::isfinite(value))
		throw std::invalid_argument("Commands require finite values");
	return value;
}
Field EncodeNested(std::uint16_t name, const std::vector<Field>& fields)
{
	return Field::Bytes(name, EncodeFields(fields), true);
}
Field EncodeFloats(std::uint16_t name, std::initializer_list<float> values)
{
	std::vector<Field> fields;
	fields.reserve(values.size());
	for (const auto value : values)
		fields.push_back(Field::F32(static_cast<std::uint16_t>(fields.size() + 1), value, true));
	return EncodeNested(name, fields);
}
template <std::size_t Count> std::array<float, Count> DecodeFloats(const Field& field)
{
	const auto fields = DecodeOrderedFields(field);
	if (fields.size() != Count)
		throw std::invalid_argument("Invalid command vector length");
	std::array<float, Count> values;
	for (std::size_t index = 0; index < Count; ++index)
		values[index] = ReadFloat(fields[index]);
	return values;
}
Field EncodePose(const XrPosef& pose)
{
	return EncodeFloats(tag::Pose, {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w,
									pose.position.x, pose.position.y, pose.position.z});
}
XrPosef DecodePose(const Field& field)
{
	const auto values = DecodeFloats<7>(field);
	return {{values[0], values[1], values[2], values[3]}, {values[4], values[5], values[6]}};
}
std::vector<Field> EncodePoseSample(const PoseSample& sample)
{
	return {EncodePose(sample.pose), Field::U64(tag::Usage, sample.flags, true)};
}
PoseSample DecodePoseSample(const Message& payload)
{
	PoseSample sample;
	sample.pose = DecodePose(RequireField(payload, tag::Pose));
	sample.flags = ReadU64(payload, tag::Usage);
	return sample;
}
Field EncodeControllerControls(const ControllerControls& controls)
{
	std::vector<Field> fields;
	const std::array values{controls.trigger, controls.grip,	   controls.gripForce,	controls.stick.x,
							controls.stick.y, controls.trackpad.x, controls.trackpad.y, controls.trackpadForce};
	for (const auto value : values)
		fields.push_back(Field::F32(static_cast<std::uint16_t>(fields.size() + 1), value, true));
	const std::array buttons{controls.primary,		controls.secondary,		 controls.menu,
							 controls.back,			controls.thumbClick,	 controls.trackpadClick,
							 controls.primaryTouch, controls.secondaryTouch, controls.triggerTouch,
							 controls.thumbTouch,	controls.trackpadTouch,	 controls.thumbrestTouch,
							 controls.connected};
	for (const auto value : buttons)
		fields.push_back(Field::Bool(static_cast<std::uint16_t>(fields.size() + 1), value, true));
	return EncodeNested(tag::Controls, fields);
}
ControllerControls DecodeControllerControls(const Field& field)
{
	const auto fields = DecodeOrderedFields(field);
	if (fields.size() != 21)
		throw std::invalid_argument("Invalid controller controls");
	ControllerControls controls;
	const std::array values{&controls.trigger, &controls.grip,		 &controls.gripForce,  &controls.stick.x,
							&controls.stick.y, &controls.trackpad.x, &controls.trackpad.y, &controls.trackpadForce};
	for (std::size_t index = 0; index < values.size(); ++index)
		*values[index] = ReadFloat(fields[index]);
	const std::array buttons{&controls.primary,		 &controls.secondary,	   &controls.menu,
							 &controls.back,		 &controls.thumbClick,	   &controls.trackpadClick,
							 &controls.primaryTouch, &controls.secondaryTouch, &controls.triggerTouch,
							 &controls.thumbTouch,	 &controls.trackpadTouch,  &controls.thumbrestTouch,
							 &controls.connected};
	for (std::size_t index = 0; index < buttons.size(); ++index)
		*buttons[index] = RequireValue(fields[index + values.size()].AsBool());
	return controls;
}
std::vector<Field> EncodeBodyControls(const BodyControls& body)
{
	std::vector<Field> controls;
	for (const auto& control : body.controls)
	{
		std::vector<Field> fields{Field::Bool(tag::Enabled, control.tracked, true)};
		if (control.offset)
			fields.push_back(EncodeFloats(tag::Position, {control.offset->x, control.offset->y, control.offset->z}));
		controls.push_back(EncodeNested(static_cast<std::uint16_t>(controls.size() + 1), fields));
	}
	std::vector<Field> joints;
	for (std::size_t index = 0; index < body.joints.size(); ++index)
	{
		const auto& joint = body.joints[index];
		if (!joint)
			continue;
		std::vector<Field> fields{Field::Bool(tag::Enabled, joint->tracked, true)};
		if (joint->pose)
			fields.push_back(EncodePose(*joint->pose));
		joints.push_back(EncodeNested(static_cast<std::uint16_t>(index + 1), fields));
	}
	std::vector<Field> result{Field::Bool(tag::Enabled, body.active, true),
							  Field::F32(tag::Values, body.confidence, true),
							  Field::U32(tag::Kind, static_cast<std::uint32_t>(body.preset), true),
							  EncodeNested(tag::Controls, controls), EncodeNested(tag::Proportions, joints)};
	if (body.rootPose)
		result.push_back(EncodePose(*body.rootPose));
	return result;
}
BodyControls DecodeBodyControls(const Message& payload)
{
	BodyControls body;
	body.active = ReadBoolean(payload, tag::Enabled);
	if (const auto* root = payload.Find(tag::Pose))
		body.rootPose = DecodePose(*root);
	body.confidence = ReadFloat(RequireField(payload, tag::Values));
	body.preset = static_cast<BodyPreset>(ReadU32(payload, tag::Kind));
	const auto controls = DecodeOrderedFields(RequireField(payload, tag::Controls));
	if (controls.size() != body.controls.size())
		throw std::invalid_argument("Invalid body controls");
	for (std::size_t index = 0; index < controls.size(); ++index)
	{
		const auto fields = DecodePayload(controls[index], {tag::Enabled, tag::Position});
		auto& control = body.controls[index];
		control.tracked = ReadBoolean(fields, tag::Enabled);
		if (const auto* position = fields.Find(tag::Position))
		{
			const auto values = DecodeFloats<3>(*position);
			control.offset = XrVector3f{values[0], values[1], values[2]};
		}
	}
	auto joints = DecodeFields(RequireField(payload, tag::Proportions).value);
	if (joints.error != DecodeError::None)
		throw std::invalid_argument("Invalid body joints");
	for (const auto& field : joints.fields)
	{
		if (field.tag == 0 || field.tag > body.joints.size())
			throw std::invalid_argument("Invalid body joint index");
		const auto fields = DecodePayload(field, {tag::Enabled, tag::Pose});
		BodyJointOverride joint;
		joint.tracked = ReadBoolean(fields, tag::Enabled);
		if (const auto* pose = fields.Find(tag::Pose))
			joint.pose = DecodePose(*pose);
		body.joints[field.tag - 1] = joint;
	}
	return body;
}
CommandPayload EncodeCommand(const TakeControl& command)
{
	return {SimulationCommandKind::TakeControl,
			{Field::U32(tag::Channel, static_cast<std::uint32_t>(command.channel), true),
			 Field::Bool(tag::TakeControl, command.replace, true)}};
}
CommandPayload EncodeCommand(const ReleaseControl& command)
{
	return {SimulationCommandKind::ReleaseControl,
			{Field::U32(tag::Channel, static_cast<std::uint32_t>(command.channel), true)}};
}
CommandPayload EncodeCommand(const SetHeadPose& command)
{
	return {SimulationCommandKind::HeadPose, EncodePoseSample(command.value)};
}
CommandPayload EncodeCommand(const SetControllerPose& command)
{
	auto fields = EncodePoseSample(command.value.pose);
	fields.push_back(Field::U32(tag::Layer, command.hand, true));
	fields.push_back(Field::Bool(tag::Enabled, command.value.connected, true));
	return {SimulationCommandKind::ControllerPose, std::move(fields)};
}
CommandPayload EncodeCommand(const SetControllerControls& command)
{
	return {SimulationCommandKind::ControllerControls,
			{Field::U32(tag::Layer, command.hand, true), EncodeControllerControls(command.value)}};
}
CommandPayload EncodeCommand(const SetBodyControls& command)
{
	return {SimulationCommandKind::BodyControls, EncodeBodyControls(command.value)};
}
CommandPayload EncodeCommand(const SetGaze& command)
{
	return {SimulationCommandKind::Gaze,
			{EncodeFloats(tag::Values, {command.value.yaw, command.value.pitch}),
			 Field::Bool(tag::Enabled, command.value.available, true),
			 Field::Bool(tag::Running, command.value.tracked, true)}};
}
CommandPayload EncodeCommand(const SetTracker& command)
{
	const auto& tracker = command.value;
	std::vector<Field> fields{
		Field::U32(tag::Layer, command.index, true), Field::U64(tag::Source, tracker.identity.value, true),
		Field::U32(tag::Kind, static_cast<std::uint32_t>(tracker.role), true),
		Field::Bool(tag::Enabled, tracker.connected, true), Field::Bool(tag::Running, tracker.tracked, true)};
	if (tracker.anchor)
		fields.push_back(Field::U32(tag::Channel, static_cast<std::uint32_t>(*tracker.anchor), true));
	if (tracker.poseOverride)
		fields.push_back(EncodePose(*tracker.poseOverride));
	return {SimulationCommandKind::Tracker, std::move(fields)};
}
CommandPayload EncodeCommand(const SetViewSettings& command)
{
	const auto& settings = command.value;
	return {SimulationCommandKind::ViewSettings,
			{EncodeFloats(tag::Values, {settings.ipd, settings.insetHalfAngle, settings.outerFov[0].angleLeft,
										settings.outerFov[0].angleRight, settings.outerFov[0].angleUp,
										settings.outerFov[0].angleDown, settings.outerFov[1].angleLeft,
										settings.outerFov[1].angleRight, settings.outerFov[1].angleUp,
										settings.outerFov[1].angleDown}),
			 Field::U32(tag::Width, settings.outerWidth, true), Field::U32(tag::Height, settings.outerHeight, true),
			 Field::U32(tag::Depth, settings.insetWidth, true),
			 Field::U32(tag::ArrayLayers, settings.insetHeight, true)}};
}
CommandPayload EncodeCommand(const SetBodyProportions& command)
{
	return {SimulationCommandKind::BodyProportions,
			{EncodeFloats(tag::Values, {command.value.height, command.value.shoulderWidth, command.value.hipWidth})}};
}
CommandPayload EncodeCommand(const SetControllerProfile& command)
{
	return {SimulationCommandKind::ControllerProfile,
			{Field::U32(tag::Layer, command.hand, true), Field::Text(tag::Detail, command.path, true)}};
}
CommandPayload EncodeCommand(const SetScenario& command)
{
	return {SimulationCommandKind::Scenario,
			{Field::U32(tag::Scenario, static_cast<std::uint32_t>(command.scenario), true),
			 Field::Bool(tag::Enabled, command.enabled, true)}};
}
CommandPayload EncodeCommand(const SetReferenceSpaces& command)
{
	const auto& local = command.value.local;
	const auto& stage = command.value.stage;
	const auto bounds = command.value.stageBounds.value_or(XrExtent2Df{});
	return {SimulationCommandKind::ReferenceSpaces,
			{EncodeFloats(1, {local.orientation.x, local.orientation.y, local.orientation.z, local.orientation.w,
							  local.position.x, local.position.y, local.position.z}),
			 EncodeFloats(2, {stage.orientation.x, stage.orientation.y, stage.orientation.z, stage.orientation.w,
							  stage.position.x, stage.position.y, stage.position.z}),
			 Field::Bool(3, command.value.stageBounds.has_value(), true),
			 EncodeFloats(4, {bounds.width, bounds.height})}};
}
} // namespace

Field EncodeSimulationCommand(const SimulationCommand& command, std::uint16_t index)
{
	const auto payload = std::visit(
		[](const auto& value)
		{
			return EncodeCommand(value);
		},
		command);
	return EncodeNested(index, {Field::U32(tag::Kind, static_cast<std::uint32_t>(payload.kind), true),
								EncodeNested(tag::Values, payload.fields)});
}
SimulationCommand DecodeSimulationCommand(const Field& field)
{
	const auto command = DecodePayload(field, {tag::Kind, tag::Values});
	const auto kind = static_cast<SimulationCommandKind>(ReadU32(command, tag::Kind));
	const auto& payload = RequireField(command, tag::Values);
	switch (kind)
	{
	case SimulationCommandKind::TakeControl:
	{
		const auto fields = DecodePayload(payload, {tag::Channel, tag::TakeControl});
		return TakeControl{static_cast<ControlChannel>(ReadU32(fields, tag::Channel)),
						   ReadBoolean(fields, tag::TakeControl)};
	}
	case SimulationCommandKind::ReleaseControl:
	{
		const auto fields = DecodePayload(payload, {tag::Channel});
		return ReleaseControl{static_cast<ControlChannel>(ReadU32(fields, tag::Channel))};
	}
	case SimulationCommandKind::HeadPose:
		return SetHeadPose{DecodePoseSample(DecodePayload(payload, {tag::Pose, tag::Usage}))};
	case SimulationCommandKind::ControllerPose:
	{
		const auto fields = DecodePayload(payload, {tag::Pose, tag::Usage, tag::Layer, tag::Enabled});
		return SetControllerPose{ReadU32(fields, tag::Layer),
								 {DecodePoseSample(fields), ReadBoolean(fields, tag::Enabled)}};
	}
	case SimulationCommandKind::ControllerControls:
	{
		const auto fields = DecodePayload(payload, {tag::Layer, tag::Controls});
		return SetControllerControls{ReadU32(fields, tag::Layer),
									 DecodeControllerControls(RequireField(fields, tag::Controls))};
	}
	case SimulationCommandKind::BodyControls:
		return SetBodyControls{DecodeBodyControls(DecodePayload(
			payload, {tag::Enabled, tag::Values, tag::Kind, tag::Controls, tag::Proportions, tag::Pose}))};
	case SimulationCommandKind::Gaze:
	{
		const auto fields = DecodePayload(payload, {tag::Values, tag::Enabled, tag::Running});
		const auto values = DecodeFloats<2>(RequireField(fields, tag::Values));
		return SetGaze{{values[0], values[1], ReadBoolean(fields, tag::Enabled), ReadBoolean(fields, tag::Running)}};
	}
	case SimulationCommandKind::Tracker:
	{
		const auto fields = DecodePayload(
			payload, {tag::Layer, tag::Source, tag::Kind, tag::Enabled, tag::Running, tag::Channel, tag::Pose});
		TrackerDevice tracker;
		tracker.identity = {ReadU64(fields, tag::Source)};
		tracker.role = static_cast<TrackerRole>(ReadU32(fields, tag::Kind));
		tracker.connected = ReadBoolean(fields, tag::Enabled);
		tracker.tracked = ReadBoolean(fields, tag::Running);
		if (fields.Find(tag::Channel))
			tracker.anchor = static_cast<BodyJoint>(ReadU32(fields, tag::Channel));
		if (const auto* pose = fields.Find(tag::Pose))
			tracker.poseOverride = DecodePose(*pose);
		return SetTracker{ReadU32(fields, tag::Layer), std::move(tracker)};
	}
	case SimulationCommandKind::ViewSettings:
	{
		const auto fields =
			DecodePayload(payload, {tag::Values, tag::Width, tag::Height, tag::Depth, tag::ArrayLayers});
		const auto values = DecodeFloats<10>(RequireField(fields, tag::Values));
		ViewSettings settings;
		settings.ipd = values[0];
		settings.insetHalfAngle = values[1];
		settings.outerFov = {
			{{values[2], values[3], values[4], values[5]}, {values[6], values[7], values[8], values[9]}}};
		settings.outerWidth = ReadU32(fields, tag::Width);
		settings.outerHeight = ReadU32(fields, tag::Height);
		settings.insetWidth = ReadU32(fields, tag::Depth);
		settings.insetHeight = ReadU32(fields, tag::ArrayLayers);
		return SetViewSettings{settings};
	}
	case SimulationCommandKind::BodyProportions:
	{
		const auto fields = DecodePayload(payload, {tag::Values});
		const auto values = DecodeFloats<3>(RequireField(fields, tag::Values));
		return SetBodyProportions{{values[0], values[1], values[2]}};
	}
	case SimulationCommandKind::ControllerProfile:
	{
		const auto fields = DecodePayload(payload, {tag::Layer, tag::Detail});
		return SetControllerProfile{ReadU32(fields, tag::Layer),
									std::string{RequireValue(RequireField(fields, tag::Detail).AsText())}};
	}
	case SimulationCommandKind::Scenario:
	{
		const auto fields = DecodePayload(payload, {tag::Scenario, tag::Enabled});
		return SetScenario{static_cast<FailureScenario>(ReadU32(fields, tag::Scenario)),
						   ReadBoolean(fields, tag::Enabled)};
	}
	case SimulationCommandKind::ReferenceSpaces:
	{
		const auto fields = DecodePayload(payload, {1, 2, 3, 4});
		const auto local = DecodeFloats<7>(RequireField(fields, 1));
		const auto stage = DecodeFloats<7>(RequireField(fields, 2));
		const auto bounds = DecodeFloats<2>(RequireField(fields, 4));
		const bool boundsAvailable = ReadBoolean(fields, 3);
		if (!boundsAvailable && (bounds[0] != 0 || bounds[1] != 0))
			throw std::invalid_argument("Unavailable stage bounds must be zero");
		ReferenceSpaceState state;
		state.local = {{local[0], local[1], local[2], local[3]}, {local[4], local[5], local[6]}};
		state.stage = {{stage[0], stage[1], stage[2], stage[3]}, {stage[4], stage[5], stage[6]}};
		state.stageBounds = boundsAvailable ? std::optional{XrExtent2Df{bounds[0], bounds[1]}} : std::nullopt;
		return SetReferenceSpaces{state};
	}
	}
	throw std::invalid_argument("Unsupported simulation command kind");
}
CommandTransaction DecodeSimulationTransaction(const Message& request, std::uint64_t connection)
{
	CommandTransaction transaction;
	transaction.id = ReadU64(request, tag::TransactionId);
	transaction.source = {static_cast<CommandSourceKind>(ReadU32(request, tag::SourceKind)), connection};
	transaction.latePolicy = static_cast<CommandLatePolicy>(ReadU32(request, tag::LatePolicy));
	if (const auto* frame = request.Find(tag::ApplyFrame))
		transaction.target.frame = FrameId{RequireValue(frame->AsU64())};
	if (const auto* time = request.Find(tag::ApplyTime))
		transaction.target.time = SimulationTime{Duration{RequireValue(time->AsI64())}};
	const auto& commandsField = RequireField(request, tag::Commands);
	if (commandsField.value.size() > 64 * 1024)
		throw std::length_error("Simulation command payload exceeds 64 KiB");
	const auto commands = DecodeOrderedFields(commandsField);
	if (commands.empty() || commands.size() > 64)
		throw std::invalid_argument("A transaction requires 1 through 64 commands");
	transaction.commands.reserve(commands.size());
	for (const auto& command : commands)
		transaction.commands.push_back(DecodeSimulationCommand(command));
	return transaction;
}
} // namespace simxr::control
