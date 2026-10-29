#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "simulation/BodyObservation.h"

#include <initializer_list>

namespace simxr::control
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
	ReferenceSpaces = 13,
	EyeViews = 14,
	Trackers = 15
};

struct CommandPayload
{
	SimulationCommandKind kind;
	std::vector<Field> fields;
};

template<class Value>
static Value RequireValue(std::optional<Value> value)
{
	if (!value)
		throw std::invalid_argument("Invalid command field value");
	return *value;
}

static const Field& RequireField(const Message& payload, std::uint16_t name)
{
	const auto* field = payload.Find(name);
	if (!field)
		throw std::invalid_argument("Missing command field");
	return *field;
}

static Message DecodePayload(const Field& field, std::initializer_list<std::uint16_t> known)
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

static std::vector<Field> DecodeOrderedFields(const Field& field)
{
	auto decoded = DecodeFields(field.value);
	if (decoded.error != DecodeError::None)
		throw std::invalid_argument("Invalid ordered command fields");
	for (std::size_t index = 0; index < decoded.fields.size(); ++index)
		if (decoded.fields[index].tag != index + 1)
			throw std::invalid_argument("Invalid command field index");
	return std::move(decoded.fields);
}

static std::uint64_t ReadU64(const Message& payload, std::uint16_t name)
{
	return RequireValue(RequireField(payload, name).AsU64());
}

static std::uint32_t ReadU32(const Message& payload, std::uint16_t name)
{
	return RequireValue(RequireField(payload, name).AsU32());
}

static bool ReadBoolean(const Message& payload, std::uint16_t name)
{
	return RequireValue(RequireField(payload, name).AsBool());
}

static float ReadFloat(const Field& field)
{
	const auto value = RequireValue(field.AsF32());
	if (!std::isfinite(value))
		throw std::invalid_argument("Commands require finite values");
	return value;
}

static Field EncodeNested(std::uint16_t name, const std::vector<Field>& fields)
{
	return Field::Bytes(name, EncodeFields(fields), true);
}

static Field EncodeFloats(std::uint16_t name, std::initializer_list<float> values)
{
	std::vector<Field> fields;
	fields.reserve(values.size());
	for (const auto value : values)
		fields.push_back(Field::F32(static_cast<std::uint16_t>(fields.size() + 1), value, true));
	return EncodeNested(name, fields);
}

template<std::size_t Count>
static std::array<float, Count> DecodeFloats(const Field& field)
{
	const auto fields = DecodeOrderedFields(field);
	if (fields.size() != Count)
		throw std::invalid_argument("Invalid command vector length");
	std::array<float, Count> values;
	for (std::size_t index = 0; index < Count; ++index)
		values[index] = ReadFloat(fields[index]);
	return values;
}

static Field EncodePose(const XrPosef& pose, std::uint16_t fieldTag = tag::Pose)
{
	return EncodeFloats(fieldTag, {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w,
								   pose.position.x, pose.position.y, pose.position.z});
}

static XrPosef DecodePose(const Field& field)
{
	const auto values = DecodeFloats<7>(field);
	return {{values[0], values[1], values[2], values[3]}, {values[4], values[5], values[6]}};
}

static std::vector<Field> EncodePoseSample(const PoseSample& sample)
{
	return {EncodePose(sample.pose), Field::U64(tag::Usage, sample.flags, true)};
}

static PoseSample DecodePoseSample(const Message& payload)
{
	PoseSample sample;
	sample.pose = DecodePose(RequireField(payload, tag::Pose));
	sample.flags = ReadU64(payload, tag::Usage);
	return sample;
}

static Field EncodeControllerControls(const ControllerControls& controls)
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
	if (controls.triggerClick)
		fields.push_back(Field::Bool(22, *controls.triggerClick, true));
	return EncodeNested(tag::Controls, fields);
}

static ControllerControls DecodeControllerControls(const Field& field)
{
	const auto fields = DecodeOrderedFields(field);
	if (fields.size() != 21 && fields.size() != 22)
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
	if (fields.size() == 22)
		controls.triggerClick = RequireValue(fields[21].AsBool());
	return controls;
}

static std::vector<Field> EncodeBodyControls(const BodyControls& body)
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

static BodyControls DecodeBodyControls(const Message& payload)
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

static std::vector<Field> DecodeBodyJoints(const Field& field)
{
	auto decoded = DecodeFields(field.value);
	if (decoded.error != DecodeError::None || decoded.fields.size() > BodyJointCount)
		throw std::invalid_argument("Invalid body joint collection");
	std::uint16_t previous = 0;
	for (const auto& entry : decoded.fields)
	{
		if (entry.tag <= previous || entry.tag > BodyJointCount)
			throw std::invalid_argument("Invalid body joint identity or order");
		previous = entry.tag;
	}
	return std::move(decoded.fields);
}

static Field EncodeBodyObservation(const BodyObservation& value)
{
	const auto body = NormalizeBodyObservation(value);
	std::vector<Field> fields{Field::U32(1, static_cast<std::uint32_t>(body.jointSet), true),
							  Field::I64(2, body.requestedTime.value.count(), true),
							  Field::Bool(4, body.located, true)};
	if (body.returnedTime)
		fields.push_back(Field::I64(3, body.returnedTime->value.count(), true));
	if (body.active)
		fields.push_back(Field::Bool(5, *body.active, true));
	if (body.allJointPosesTracked)
		fields.push_back(Field::Bool(6, *body.allJointPosesTracked, true));
	if (body.confidence)
		fields.push_back(Field::F32(7, *body.confidence, true));
	if (body.skeletonRevision)
		fields.push_back(Field::U32(8, *body.skeletonRevision, true));
	std::vector<Field> joints;
	for (std::size_t index = 0; index < BodyJointCount; ++index)
		if (const auto& joint = body.joints[index])
			joints.push_back(
				EncodeNested(static_cast<std::uint16_t>(index + 1), EncodePoseSample({joint->pose, joint->flags})));
	fields.push_back(EncodeNested(9, joints));
	if (body.skeleton)
	{
		joints.clear();
		for (std::size_t index = 0; index < BodyJointCount; ++index)
			if (const auto& joint = (*body.skeleton)[index])
			{
				std::vector<Field> rest{EncodePose(joint->bindPose)};
				if (joint->parent)
					rest.push_back(Field::U32(tag::Channel, static_cast<std::uint32_t>(*joint->parent), true));
				joints.push_back(EncodeNested(static_cast<std::uint16_t>(index + 1), rest));
			}
		fields.push_back(EncodeNested(10, joints));
	}
	return EncodeNested(tag::BodyObservation, fields);
}

static BodyObservation DecodeBodyObservation(const Field& field)
{
	const auto fields = DecodePayload(field, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
	BodyObservation result;
	result.jointSet = static_cast<BodyJointSet>(ReadU32(fields, 1));
	result.requestedTime = {Duration{RequireValue(RequireField(fields, 2).AsI64())}};
	result.located = ReadBoolean(fields, 4);
	if (const auto* time = fields.Find(3))
		result.returnedTime = RuntimeTime{Duration{RequireValue(time->AsI64())}};
	if (fields.Find(5))
		result.active = ReadBoolean(fields, 5);
	if (fields.Find(6))
		result.allJointPosesTracked = ReadBoolean(fields, 6);
	if (const auto* confidence = fields.Find(7))
		result.confidence = ReadFloat(*confidence);
	if (fields.Find(8))
		result.skeletonRevision = ReadU32(fields, 8);
	for (const auto& entry : DecodeBodyJoints(RequireField(fields, 9)))
	{
		const auto joint = DecodePoseSample(DecodePayload(entry, {tag::Pose, tag::Usage}));
		result.joints[entry.tag - 1] = ObservedBodyJoint{joint.pose, joint.flags};
	}
	if (const auto* skeleton = fields.Find(10))
	{
		result.skeleton.emplace();
		for (const auto& entry : DecodeBodyJoints(*skeleton))
		{
			const auto joint = DecodePayload(entry, {tag::Pose, tag::Channel});
			SkeletonJoint rest{static_cast<BodyJoint>(entry.tag - 1), {}, DecodePose(RequireField(joint, tag::Pose))};
			if (joint.Find(tag::Channel))
				rest.parent = static_cast<BodyJoint>(ReadU32(joint, tag::Channel));
			(*result.skeleton)[entry.tag - 1] = rest;
		}
	}
	return NormalizeBodyObservation(std::move(result));
}

static CommandPayload EncodeCommand(const TakeControl& command)
{
	return {SimulationCommandKind::TakeControl,
			{Field::U32(tag::Channel, static_cast<std::uint32_t>(command.channel), true),
			 Field::Bool(tag::TakeControl, command.replace, true)}};
}

static CommandPayload EncodeCommand(const ReleaseControl& command)
{
	return {SimulationCommandKind::ReleaseControl,
			{Field::U32(tag::Channel, static_cast<std::uint32_t>(command.channel), true)}};
}

static CommandPayload EncodeCommand(const SetHeadPose& command)
{
	return {SimulationCommandKind::HeadPose, EncodePoseSample(command.value)};
}

static CommandPayload EncodeCommand(const SetControllerPose& command)
{
	auto fields = EncodePoseSample(command.value.pose);
	fields.push_back(Field::U32(tag::Layer, command.hand, true));
	fields.push_back(Field::Bool(tag::Enabled, command.value.connected, true));
	if (command.value.aim)
		fields.push_back(EncodeNested(tag::AimPose, EncodePoseSample(*command.value.aim)));
	return {SimulationCommandKind::ControllerPose, std::move(fields)};
}

static CommandPayload EncodeCommand(const SetControllerControls& command)
{
	std::vector<Field> fields{Field::U32(tag::Layer, command.hand, true), EncodeControllerControls(command.value)};
	if (!command.value.physicalProfile.empty())
		fields.push_back(Field::Text(tag::Detail, command.value.physicalProfile, true));
	return {SimulationCommandKind::ControllerControls, std::move(fields)};
}

static CommandPayload EncodeCommand(const SetBodyControls& command)
{
	if (command.observationOrigin && (!command.observation || command.observationOrigin->value.count() <= 0))
		throw std::invalid_argument("Body replay timing requires an observation and positive anchor");
	auto fields = EncodeBodyControls(command.value);
	if (command.observation)
		fields.push_back(EncodeBodyObservation(*command.observation));
	if (command.observationOrigin)
		fields.push_back(Field::I64(tag::BodyObservationOrigin, command.observationOrigin->value.count(), true));
	return {SimulationCommandKind::BodyControls, std::move(fields)};
}

static CommandPayload EncodeCommand(const SetGaze& command)
{
	return {SimulationCommandKind::Gaze,
			{EncodeFloats(tag::Values, {command.value.yaw, command.value.pitch}),
			 Field::Bool(tag::Enabled, command.value.available, true),
			 Field::Bool(tag::Running, command.value.tracked, true)}};
}

static CommandPayload EncodeCommand(const SetTracker& command)
{
	const auto& tracker = command.value;
	std::vector<Field> fields{
		Field::U32(tag::Layer, command.index, true), Field::U64(tag::Source, tracker.identity.value, true),
		Field::U32(tag::Kind, static_cast<std::uint32_t>(tracker.role), true),
		Field::Bool(tag::Enabled, tracker.connected, true), Field::Bool(tag::Running, tracker.tracked, true)};
	if (!tracker.sourcePath.View().empty())
		fields.push_back(Field::Text(tag::Detail, tracker.sourcePath.View(), true));
	if (tracker.poseFlags != TrackedPoseFlags)
		fields.push_back(Field::U64(tag::Usage, tracker.poseFlags, true));
	if (tracker.anchor)
		fields.push_back(Field::U32(tag::Channel, static_cast<std::uint32_t>(*tracker.anchor), true));
	if (tracker.jointOffset)
		fields.push_back(EncodePose(*tracker.jointOffset, tag::JointOffset));
	if (tracker.poseOverride)
		fields.push_back(EncodePose(*tracker.poseOverride));
	return {SimulationCommandKind::Tracker, std::move(fields)};
}

static CommandPayload EncodeCommand(const SetTrackers& command)
{
	if (command.value.size() > MaxTrackerDevices)
		throw std::invalid_argument("Tracker inventory exceeds capacity");
	std::vector<Field> entries;
	for (std::size_t index = 0; index < command.value.size(); ++index)
		entries.push_back(
			EncodeNested(static_cast<std::uint16_t>(index + 1),
						 EncodeCommand(SetTracker{static_cast<std::uint32_t>(index), command.value[index]}).fields));
	return {SimulationCommandKind::Trackers, std::move(entries)};
}

static CommandPayload EncodeCommand(const SetEyeViews& command)
{
	std::vector<Field> fields{Field::Bool(1, command.value.has_value(), true)};
	if (command.value)
	{
		fields.push_back(Field::U64(2, command.value->spaceRevision, true));
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			auto pose = EncodePoseSample(command.value->poses[eye]);
			const auto& fov = command.value->fovs[eye];
			pose.push_back(EncodeFloats(tag::Values, {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown}));
			fields.push_back(EncodeNested(static_cast<std::uint16_t>(eye + 3), pose));
		}
	}
	return {SimulationCommandKind::EyeViews, std::move(fields)};
}

static CommandPayload EncodeCommand(const SetViewSettings& command)
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

static CommandPayload EncodeCommand(const SetBodyProportions& command)
{
	return {SimulationCommandKind::BodyProportions,
			{EncodeFloats(tag::Values, {command.value.height, command.value.shoulderWidth, command.value.hipWidth})}};
}

static CommandPayload EncodeCommand(const SetControllerProfile& command)
{
	return {SimulationCommandKind::ControllerProfile,
			{Field::U32(tag::Layer, command.hand, true), Field::Text(tag::Detail, command.path, true)}};
}

static CommandPayload EncodeCommand(const SetScenario& command)
{
	return {SimulationCommandKind::Scenario,
			{Field::U32(tag::Scenario, static_cast<std::uint32_t>(command.scenario), true),
			 Field::Bool(tag::Enabled, command.enabled, true)}};
}

static CommandPayload EncodeCommand(const SetReferenceSpaces& command)
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

Field EncodeSimulationCommand(const SimulationCommand& command, std::uint16_t index)
{
	const auto payload = std::visit([](const auto& value) { return EncodeCommand(value); }, command);
	return EncodeNested(index, {Field::U32(tag::Kind, static_cast<std::uint32_t>(payload.kind), true),
								EncodeNested(tag::Values, payload.fields)});
}

static TrackerDevice DecodeTracker(const Message& fields)
{
	TrackerDevice tracker;
	tracker.identity = {ReadU64(fields, tag::Source)};
	tracker.role = static_cast<TrackerRole>(ReadU32(fields, tag::Kind));
	tracker.connected = ReadBoolean(fields, tag::Enabled);
	tracker.tracked = ReadBoolean(fields, tag::Running);
	if (const auto source = fields.Find(tag::Detail))
		tracker.sourcePath = TrackerSourcePath{RequireValue(source->AsText())};
	if (fields.Find(tag::Usage))
		tracker.poseFlags = ReadU64(fields, tag::Usage);
	if (fields.Find(tag::Channel))
		tracker.anchor = static_cast<BodyJoint>(ReadU32(fields, tag::Channel));
	if (const auto* pose = fields.Find(tag::Pose))
		tracker.poseOverride = DecodePose(*pose);
	if (const auto* offset = fields.Find(tag::JointOffset))
		tracker.jointOffset = DecodePose(*offset);
	return tracker;
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
		const auto fields = DecodePayload(payload, {tag::Pose, tag::Usage, tag::Layer, tag::Enabled, tag::AimPose});
		ControllerPose value{DecodePoseSample(fields), ReadBoolean(fields, tag::Enabled)};
		if (const auto* aim = fields.Find(tag::AimPose))
			value.aim = DecodePoseSample(DecodePayload(*aim, {tag::Pose, tag::Usage}));
		return SetControllerPose{ReadU32(fields, tag::Layer), value};
	}
	case SimulationCommandKind::ControllerControls:
	{
		const auto fields = DecodePayload(payload, {tag::Layer, tag::Controls, tag::Detail});
		auto controls = DecodeControllerControls(RequireField(fields, tag::Controls));
		if (const auto profile = fields.Find(tag::Detail))
			controls.physicalProfile = RequireValue(profile->AsText());
		return SetControllerControls{ReadU32(fields, tag::Layer), std::move(controls)};
	}
	case SimulationCommandKind::BodyControls:
	{
		const auto fields =
			DecodePayload(payload, {tag::Enabled, tag::Values, tag::Kind, tag::Controls, tag::Proportions, tag::Pose,
									tag::BodyObservation, tag::BodyObservationOrigin});
		SetBodyControls result{DecodeBodyControls(fields)};
		if (const auto* body = fields.Find(tag::BodyObservation))
			result.observation = DecodeBodyObservation(*body);
		if (const auto* origin = fields.Find(tag::BodyObservationOrigin))
		{
			result.observationOrigin = RuntimeTime{Duration{RequireValue(origin->AsI64())}};
			if (!result.observation || result.observationOrigin->value.count() <= 0)
				throw std::invalid_argument("Body replay timing requires an observation and positive anchor");
		}
		return result;
	}
	case SimulationCommandKind::Gaze:
	{
		const auto fields = DecodePayload(payload, {tag::Values, tag::Enabled, tag::Running});
		const auto values = DecodeFloats<2>(RequireField(fields, tag::Values));
		return SetGaze{{values[0], values[1], ReadBoolean(fields, tag::Enabled), ReadBoolean(fields, tag::Running)}};
	}
	case SimulationCommandKind::Tracker:
	{
		const auto fields =
			DecodePayload(payload, {tag::Layer, tag::Source, tag::Kind, tag::Enabled, tag::Running, tag::Channel,
									tag::Pose, tag::JointOffset, tag::Usage, tag::Detail});
		auto tracker = DecodeTracker(fields);
		return SetTracker{ReadU32(fields, tag::Layer), std::move(tracker)};
	}
	case SimulationCommandKind::Trackers:
	{
		const auto entries = DecodeOrderedFields(payload);
		if (entries.size() > MaxTrackerDevices)
			throw std::invalid_argument("Tracker inventory exceeds capacity");
		SetTrackers result;
		result.value.reserve(entries.size());
		for (std::size_t index = 0; index < entries.size(); ++index)
		{
			const auto fields =
				DecodePayload(entries[index], {tag::Layer, tag::Source, tag::Kind, tag::Enabled, tag::Running,
											   tag::Channel, tag::Pose, tag::JointOffset, tag::Usage, tag::Detail});
			if (ReadU32(fields, tag::Layer) != index)
				throw std::invalid_argument("Invalid tracker collection index");
			result.value.push_back(DecodeTracker(fields));
		}
		return result;
	}
	case SimulationCommandKind::EyeViews:
	{
		const auto fields = DecodePayload(payload, {1, 2, 3, 4});
		if (!ReadBoolean(fields, 1))
		{
			if (fields.fields.size() != 1)
				throw std::invalid_argument("Cleared eye views contain observations");
			return SetEyeViews{};
		}
		StereoEyeViews views;
		views.spaceRevision = ReadU64(fields, 2);
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			const auto pose = DecodePayload(RequireField(fields, static_cast<std::uint16_t>(eye + 3)),
											{tag::Pose, tag::Usage, tag::Values});
			views.poses[eye] = DecodePoseSample(pose);
			const auto fov = DecodeFloats<4>(RequireField(pose, tag::Values));
			views.fovs[eye] = {fov[0], fov[1], fov[2], fov[3]};
		}
		return SetEyeViews{views};
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
	if (const auto* expected = request.Find(tag::ExpectedInputs))
	{
		if (request.minor < 17 || expected->value.size() > 4096)
			throw std::invalid_argument("Expected inputs require protocol minor 17 and a bounded payload");
		const auto fields = DecodeOrderedFields(*expected);
		if (fields.empty() || fields.size() > 3)
			throw std::invalid_argument("Expected inputs require one to three entries");
		for (const auto& field : fields)
			transaction.expected.push_back(DecodeSimulationCommand(field));
	}
	return transaction;
}
} // namespace simxr::control
