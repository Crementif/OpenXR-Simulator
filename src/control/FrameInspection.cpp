#include "control/FrameInspection.h"
#include "Pch.h"
#include "control/SimulationControl.h"
#include "graphics/FrameOutput.h"
#include "simulation/BodyModel.h"

#include <stdexcept>
#include <variant>

namespace simxr::control
{
template <class Range, class Function> static InspectionValue InspectArray(const Range& range, Function function)
{
	if (range.size() > MaxFields * MaxFields)
		throw std::length_error("Inspection array capacity exceeded");
	std::vector<InspectionValue> values;
	values.reserve(range.size());
	for (const auto& value : range)
		values.push_back(function(value));
	return InspectionValue::Array(std::move(values));
}
static InspectionValue InspectVector(XrVector2f value)
{
	return InspectionValue::Object({{"x", InspectionValue::Float(value.x)}, {"y", InspectionValue::Float(value.y)}});
}
static InspectionValue InspectVector(XrVector3f value)
{
	return InspectionValue::Object({{"x", InspectionValue::Float(value.x)},
									{"y", InspectionValue::Float(value.y)},
									{"z", InspectionValue::Float(value.z)}});
}
static InspectionValue InspectPose(const XrPosef& value)
{
	return InspectionValue::Object(
		{{"orientation", InspectionValue::Object({{"x", InspectionValue::Float(value.orientation.x)},
												  {"y", InspectionValue::Float(value.orientation.y)},
												  {"z", InspectionValue::Float(value.orientation.z)},
												  {"w", InspectionValue::Float(value.orientation.w)}})},
		 {"position", InspectVector(value.position)}});
}
static InspectionValue InspectPoseSample(const PoseSample& value)
{
	return InspectionValue::Object(
		{{"pose", InspectPose(value.pose)},
		 {"flags", InspectionValue::Unsigned(value.flags)},
		 {"sample_time", InspectionValue::Signed(value.sampleTime.value.count())},
		 {"eye_gaze_sample_time", value.eyeGazeSampleTime
									  ? InspectionValue::Signed(value.eyeGazeSampleTime->value.count())
									  : InspectionValue::Null()}});
}
static InspectionValue InspectFov(const XrFovf& value)
{
	return InspectionValue::Object({{"left", InspectionValue::Float(value.angleLeft)},
									{"right", InspectionValue::Float(value.angleRight)},
									{"up", InspectionValue::Float(value.angleUp)},
									{"down", InspectionValue::Float(value.angleDown)}});
}
static InspectionValue InspectDevice(InputDeviceId value)
{
	const char* kind;
	switch (value.kind)
	{
	case InputDeviceKind::Controller:
		kind = "controller";
		break;
	case InputDeviceKind::EyeGaze:
		kind = "eye_gaze";
		break;
	case InputDeviceKind::Tracker:
		kind = "tracker";
		break;
	default:
		throw std::invalid_argument("Unknown device kind");
	}
	return InspectionValue::Object(
		{{"kind", InspectionValue::Text(kind)}, {"id", InspectionValue::Unsigned(value.value)}});
}
static InspectionValue InspectControls(const ControllerControls& value)
{
	return InspectionValue::Object({{"trigger", InspectionValue::Float(value.trigger)},
									{"grip", InspectionValue::Float(value.grip)},
									{"grip_force", InspectionValue::Float(value.gripForce)},
									{"stick", InspectVector(value.stick)},
									{"trackpad", InspectVector(value.trackpad)},
									{"trackpad_force", InspectionValue::Float(value.trackpadForce)},
									{"primary", InspectionValue::Boolean(value.primary)},
									{"secondary", InspectionValue::Boolean(value.secondary)},
									{"menu", InspectionValue::Boolean(value.menu)},
									{"back", InspectionValue::Boolean(value.back)},
									{"thumb_click", InspectionValue::Boolean(value.thumbClick)},
									{"trackpad_click", InspectionValue::Boolean(value.trackpadClick)},
									{"primary_touch", InspectionValue::Boolean(value.primaryTouch)},
									{"secondary_touch", InspectionValue::Boolean(value.secondaryTouch)},
									{"trigger_touch", InspectionValue::Boolean(value.triggerTouch)},
									{"thumb_touch", InspectionValue::Boolean(value.thumbTouch)},
									{"trackpad_touch", InspectionValue::Boolean(value.trackpadTouch)},
									{"thumbrest_touch", InspectionValue::Boolean(value.thumbrestTouch)},
									{"connected", InspectionValue::Boolean(value.connected)}});
}
static InspectionValue InspectInput(const InputSnapshot& value)
{
	return InspectionValue::Object(
		{{"revision", InspectionValue::Unsigned(value.revision)},
		 {"time", InspectionValue::Signed(value.time.value.count())},
		 {"controllers", InspectArray(value.controllers, InspectControls)},
		 {"preferred_profiles", InspectArray(value.preferredProfiles,
											 [](const auto& name)
											 {
												 return InspectionValue::Text(name);
											 })},
		 {"devices", InspectArray(value.devices,
								  [](const auto& device)
								  {
									  return InspectionValue::Object(
										  {{"identity", InspectDevice(device.identity)},
										   {"user_paths", InspectArray(device.userPaths,
																	   [](const auto& path)
																	   {
																		   return InspectionValue::Text(path);
																	   })},
										   {"preferred_profile", InspectionValue::Text(device.preferredProfile)},
										   {"connected", InspectionValue::Boolean(device.connected)},
										   {"pose_available", InspectionValue::Boolean(device.poseAvailable)},
										   {"controls", InspectControls(device.controls)}});
								  })}});
}
static const char* JointName(BodyJoint joint)
{
	switch (joint)
	{
	case BodyJoint::Chest:
		return "Chest";
	case BodyJoint::Head:
		return "Head";
	case BodyJoint::Hips:
		return "Hips";
	case BodyJoint::LeftArmLower:
		return "LeftArmLower";
	case BodyJoint::LeftArmUpper:
		return "LeftArmUpper";
	case BodyJoint::LeftFootAnkle:
		return "LeftFootAnkle";
	case BodyJoint::LeftFootAnkleTwist:
		return "LeftFootAnkleTwist";
	case BodyJoint::LeftFootBall:
		return "LeftFootBall";
	case BodyJoint::LeftFootSubtalar:
		return "LeftFootSubtalar";
	case BodyJoint::LeftFootTransverse:
		return "LeftFootTransverse";
	case BodyJoint::LeftHandIndexDistal:
		return "LeftHandIndexDistal";
	case BodyJoint::LeftHandIndexIntermediate:
		return "LeftHandIndexIntermediate";
	case BodyJoint::LeftHandIndexMetacarpal:
		return "LeftHandIndexMetacarpal";
	case BodyJoint::LeftHandIndexProximal:
		return "LeftHandIndexProximal";
	case BodyJoint::LeftHandIndexTip:
		return "LeftHandIndexTip";
	case BodyJoint::LeftHandLittleDistal:
		return "LeftHandLittleDistal";
	case BodyJoint::LeftHandLittleIntermediate:
		return "LeftHandLittleIntermediate";
	case BodyJoint::LeftHandLittleMetacarpal:
		return "LeftHandLittleMetacarpal";
	case BodyJoint::LeftHandLittleProximal:
		return "LeftHandLittleProximal";
	case BodyJoint::LeftHandLittleTip:
		return "LeftHandLittleTip";
	case BodyJoint::LeftHandMiddleDistal:
		return "LeftHandMiddleDistal";
	case BodyJoint::LeftHandMiddleIntermediate:
		return "LeftHandMiddleIntermediate";
	case BodyJoint::LeftHandMiddleMetacarpal:
		return "LeftHandMiddleMetacarpal";
	case BodyJoint::LeftHandMiddleProximal:
		return "LeftHandMiddleProximal";
	case BodyJoint::LeftHandMiddleTip:
		return "LeftHandMiddleTip";
	case BodyJoint::LeftHandPalm:
		return "LeftHandPalm";
	case BodyJoint::LeftHandRingDistal:
		return "LeftHandRingDistal";
	case BodyJoint::LeftHandRingIntermediate:
		return "LeftHandRingIntermediate";
	case BodyJoint::LeftHandRingMetacarpal:
		return "LeftHandRingMetacarpal";
	case BodyJoint::LeftHandRingProximal:
		return "LeftHandRingProximal";
	case BodyJoint::LeftHandRingTip:
		return "LeftHandRingTip";
	case BodyJoint::LeftHandThumbDistal:
		return "LeftHandThumbDistal";
	case BodyJoint::LeftHandThumbMetacarpal:
		return "LeftHandThumbMetacarpal";
	case BodyJoint::LeftHandThumbProximal:
		return "LeftHandThumbProximal";
	case BodyJoint::LeftHandThumbTip:
		return "LeftHandThumbTip";
	case BodyJoint::LeftHandWrist:
		return "LeftHandWrist";
	case BodyJoint::LeftHandWristTwist:
		return "LeftHandWristTwist";
	case BodyJoint::LeftLowerLeg:
		return "LeftLowerLeg";
	case BodyJoint::LeftScapula:
		return "LeftScapula";
	case BodyJoint::LeftShoulder:
		return "LeftShoulder";
	case BodyJoint::LeftUpperLeg:
		return "LeftUpperLeg";
	case BodyJoint::Neck:
		return "Neck";
	case BodyJoint::RightArmLower:
		return "RightArmLower";
	case BodyJoint::RightArmUpper:
		return "RightArmUpper";
	case BodyJoint::RightFootAnkle:
		return "RightFootAnkle";
	case BodyJoint::RightFootAnkleTwist:
		return "RightFootAnkleTwist";
	case BodyJoint::RightFootBall:
		return "RightFootBall";
	case BodyJoint::RightFootSubtalar:
		return "RightFootSubtalar";
	case BodyJoint::RightFootTransverse:
		return "RightFootTransverse";
	case BodyJoint::RightHandIndexDistal:
		return "RightHandIndexDistal";
	case BodyJoint::RightHandIndexIntermediate:
		return "RightHandIndexIntermediate";
	case BodyJoint::RightHandIndexMetacarpal:
		return "RightHandIndexMetacarpal";
	case BodyJoint::RightHandIndexProximal:
		return "RightHandIndexProximal";
	case BodyJoint::RightHandIndexTip:
		return "RightHandIndexTip";
	case BodyJoint::RightHandLittleDistal:
		return "RightHandLittleDistal";
	case BodyJoint::RightHandLittleIntermediate:
		return "RightHandLittleIntermediate";
	case BodyJoint::RightHandLittleMetacarpal:
		return "RightHandLittleMetacarpal";
	case BodyJoint::RightHandLittleProximal:
		return "RightHandLittleProximal";
	case BodyJoint::RightHandLittleTip:
		return "RightHandLittleTip";
	case BodyJoint::RightHandMiddleDistal:
		return "RightHandMiddleDistal";
	case BodyJoint::RightHandMiddleIntermediate:
		return "RightHandMiddleIntermediate";
	case BodyJoint::RightHandMiddleMetacarpal:
		return "RightHandMiddleMetacarpal";
	case BodyJoint::RightHandMiddleProximal:
		return "RightHandMiddleProximal";
	case BodyJoint::RightHandMiddleTip:
		return "RightHandMiddleTip";
	case BodyJoint::RightHandPalm:
		return "RightHandPalm";
	case BodyJoint::RightHandRingDistal:
		return "RightHandRingDistal";
	case BodyJoint::RightHandRingIntermediate:
		return "RightHandRingIntermediate";
	case BodyJoint::RightHandRingMetacarpal:
		return "RightHandRingMetacarpal";
	case BodyJoint::RightHandRingProximal:
		return "RightHandRingProximal";
	case BodyJoint::RightHandRingTip:
		return "RightHandRingTip";
	case BodyJoint::RightHandThumbDistal:
		return "RightHandThumbDistal";
	case BodyJoint::RightHandThumbMetacarpal:
		return "RightHandThumbMetacarpal";
	case BodyJoint::RightHandThumbProximal:
		return "RightHandThumbProximal";
	case BodyJoint::RightHandThumbTip:
		return "RightHandThumbTip";
	case BodyJoint::RightHandWrist:
		return "RightHandWrist";
	case BodyJoint::RightHandWristTwist:
		return "RightHandWristTwist";
	case BodyJoint::RightLowerLeg:
		return "RightLowerLeg";
	case BodyJoint::RightScapula:
		return "RightScapula";
	case BodyJoint::RightShoulder:
		return "RightShoulder";
	case BodyJoint::RightUpperLeg:
		return "RightUpperLeg";
	case BodyJoint::Root:
		return "Root";
	case BodyJoint::SpineLower:
		return "SpineLower";
	case BodyJoint::SpineMiddle:
		return "SpineMiddle";
	case BodyJoint::SpineUpper:
		return "SpineUpper";
	default:
		throw std::invalid_argument("Unknown body joint");
	}
}
static InspectionValue InspectProportions(const BodyProportions& value)
{
	return InspectionValue::Object({{"height", InspectionValue::Float(value.height)},
									{"shoulder_width", InspectionValue::Float(value.shoulderWidth)},
									{"hip_width", InspectionValue::Float(value.hipWidth)}});
}
static InspectionValue InspectSkeleton(const TrackingSnapshot& value)
{
	return InspectionValue::Object(
		{{"schema", InspectionValue::Unsigned(1)},
		 {"model_revision", InspectionValue::Unsigned(value.body.skeletonRevision)},
		 {"proportions", InspectProportions(value.proportions)},
		 {"joints", InspectArray(value.body.skeleton,
								 [](const auto& joint)
								 {
									 return InspectionValue::Object(
										 {{"joint", InspectionValue::Text(JointName(joint.joint))},
										  {"parent", joint.parent ? InspectionValue::Text(JointName(*joint.parent))
																  : InspectionValue::Null()},
										  {"bind_pose", InspectPose(joint.bindPose)}});
								 })}});
}
static InspectionValue InspectTracker(const TrackerDevice& device)
{
	return InspectionValue::Object(
		{{"id", InspectionValue::Unsigned(device.identity.value)},
		 {"role", InspectionValue::Text(BodyModel::TrackerRolePath(device.role))},
		 {"connected", InspectionValue::Boolean(device.connected)},
		 {"tracked", InspectionValue::Boolean(device.tracked)},
		 {"anchor", device.anchor ? InspectionValue::Text(JointName(*device.anchor)) : InspectionValue::Null()},
		 {"joint_offset", device.jointOffset ? InspectPose(*device.jointOffset) : InspectionValue::Null()},
		 {"pose_override", device.poseOverride ? InspectPose(*device.poseOverride) : InspectionValue::Null()}});
}
static InspectionValue InspectBodyInput(const BodyControls& value)
{
	const char* preset;
	switch (value.preset)
	{
	case BodyPreset::Standing:
		preset = "standing";
		break;
	case BodyPreset::TPose:
		preset = "t_pose";
		break;
	case BodyPreset::Seated:
		preset = "seated";
		break;
	default:
		throw std::invalid_argument("Unknown body preset");
	}
	const std::array<const char*, 5> names{"Hips", "Chest", "Head", "LeftFoot", "RightFoot"};
	std::vector<InspectionValue> controls, joints;
	for (std::size_t index = 0; index < value.controls.size(); ++index)
		controls.push_back(InspectionValue::Object(
			{{"control", InspectionValue::Text(names.at(index))},
			 {"tracked", InspectionValue::Boolean(value.controls[index].tracked)},
			 {"offset",
			  value.controls[index].offset ? InspectVector(*value.controls[index].offset) : InspectionValue::Null()}}));
	for (std::size_t index = 0; index < value.joints.size(); ++index)
		if (const auto& joint = value.joints[index])
			joints.push_back(
				InspectionValue::Object({{"joint", InspectionValue::Text(JointName(static_cast<BodyJoint>(index)))},
										 {"tracked", InspectionValue::Boolean(joint->tracked)},
										 {"pose", joint->pose ? InspectPose(*joint->pose) : InspectionValue::Null()}}));
	return InspectionValue::Object(
		{{"active", InspectionValue::Boolean(value.active)},
		 {"confidence", InspectionValue::Float(value.confidence)},
		 {"preset", InspectionValue::Text(preset)},
		 {"root_pose", value.rootPose ? InspectPose(*value.rootPose) : InspectionValue::Null()},
		 {"controls", InspectionValue::Array(std::move(controls))},
		 {"joint_overrides", InspectionValue::Array(std::move(joints))}});
}
static InspectionValue InspectTrackedController(const ControllerPose& controller)
{
	return InspectionValue::Object({{"connected", InspectionValue::Boolean(controller.connected)},
									{"sample", InspectPoseSample(controller.pose)}});
}

static InspectionValue InspectTrackingInput(const TrackingState& input)
{
	const auto gaze = InspectionValue::Object({{"yaw", InspectionValue::Float(input.gaze.yaw)},
											   {"pitch", InspectionValue::Float(input.gaze.pitch)},
											   {"available", InspectionValue::Boolean(input.gaze.available)},
											   {"tracked", InspectionValue::Boolean(input.gaze.tracked)}});
	return InspectionValue::Object({{"head", InspectPoseSample(input.head)},
									{"controllers", InspectArray(input.controllers, InspectTrackedController)},
									{"gaze", gaze},
									{"body", InspectBodyInput(input.body)},
									{"focused", InspectionValue::Boolean(input.focused)},
									{"trackers", InspectArray(input.trackers, InspectTracker)}});
}

static InspectionValue InspectTrackedBody(const BodySample& body, std::uint64_t skeleton)
{
	std::vector<InspectionValue> joints;
	joints.reserve(body.joints.size());
	for (std::size_t index = 0; index < body.joints.size(); ++index)
		joints.push_back(
			InspectionValue::Object({{"joint", InspectionValue::Text(JointName(static_cast<BodyJoint>(index)))},
									 {"sample", InspectPoseSample(body.joints[index])}}));
	return InspectionValue::Object({{"active", InspectionValue::Boolean(body.active)},
									{"confidence", InspectionValue::Float(body.confidence)},
									{"sample_time", InspectionValue::Signed(body.sampleTime.value.count())},
									{"skeleton", InspectionValue::Revision(skeleton)},
									{"joints", InspectionValue::Array(std::move(joints))}});
}

static InspectionValue InspectTrackedDevice(const TrackerSample& tracker)
{
	return InspectionValue::Object(
		{{"device", InspectTracker(tracker.device)}, {"sample", InspectPoseSample(tracker.pose)}});
}

static InspectionValue InspectTracking(const TrackingSnapshot& tracking, std::uint64_t skeleton)
{
	return InspectionValue::Object({{"time", InspectionValue::Signed(tracking.time.value.count())},
									{"head", InspectPoseSample(tracking.head)},
									{"controllers", InspectArray(tracking.controllers, InspectTrackedController)},
									{"gaze", InspectPoseSample(tracking.gaze)},
									{"body", InspectTrackedBody(tracking.body, skeleton)},
									{"trackers", InspectArray(tracking.trackers, InspectTrackedDevice)},
									{"input", InspectTrackingInput(tracking.input)},
									{"proportions", InspectProportions(tracking.proportions)}});
}
static InspectionValue InspectViews(const ViewSettings& value, ViewLayout layout)
{
	return InspectionValue::Object(
		{{"schema", InspectionValue::Unsigned(1)},
		 {"view_layout", InspectionValue::Text(layout == ViewLayout::StereoWithInset ? "stereo_with_inset" : "stereo")},
		 {"active_view_count", InspectionValue::Unsigned(layout == ViewLayout::StereoWithInset ? 4 : 2)},
		 {"ipd", InspectionValue::Float(value.ipd)},
		 {"outer_width", InspectionValue::Unsigned(value.outerWidth)},
		 {"outer_height", InspectionValue::Unsigned(value.outerHeight)},
		 {"inset_width", InspectionValue::Unsigned(value.insetWidth)},
		 {"inset_height", InspectionValue::Unsigned(value.insetHeight)},
		 {"inset_half_angle", InspectionValue::Float(value.insetHalfAngle)},
		 {"outer_fov", InspectArray(value.outerFov, InspectFov)}});
}
class ActionDictionaryEncoder
{
  public:
	explicit ActionDictionaryEncoder(const ActionInspectionDictionary* dictionary) : m_dictionary(dictionary) {}
	bool Available() const
	{
		return m_dictionary != nullptr;
	}
	std::uint64_t SetIdentity(XrActionSet handle) const
	{
		if (m_dictionary)
			for (std::size_t index = 0; index < m_dictionary->sets.size(); ++index)
				if (m_dictionary->sets[index].handle == handle)
					return index + 1;
		throw std::invalid_argument("Missing historical action set definition");
	}
	std::uint64_t ActionIdentity(XrAction handle) const
	{
		if (m_dictionary)
			for (std::size_t index = 0; index < m_dictionary->actions.size(); ++index)
				if (m_dictionary->actions[index].handle == handle)
					return index + 1;
		throw std::invalid_argument("Missing historical action definition");
	}
	std::string_view PathName(XrPath path) const
	{
		if (!path)
			return {};
		const auto name = m_dictionary ? m_dictionary->PathName(path) : std::nullopt;
		if (!name)
			throw std::invalid_argument("Missing historical path name");
		return *name;
	}
	InspectionValue InspectPath(XrPath path) const
	{
		if (!path)
			return InspectionValue::Null();
		const auto name = m_dictionary ? m_dictionary->PathName(path) : std::nullopt;
		if (!name)
			throw std::invalid_argument("Missing historical path name");
		return InspectionValue::Text(*name);
	}
	InspectionValue InspectBinding(const ActionBinding& binding) const
	{
		return InspectionValue::Object({{"action", InspectionValue::Unsigned(ActionIdentity(binding.action))},
										{"profile", InspectPath(binding.profile)},
										{"source", InspectPath(binding.source)},
										{"user_path", InspectPath(binding.userPath)},
										{"component", InspectionValue::Text(binding.component)},
										{"source_type", InspectionValue::Signed(binding.sourceType)}});
	}
	InspectionValue InspectDictionary() const
	{
		if (!m_dictionary)
			throw std::invalid_argument("Missing historical action dictionary");
		return InspectionValue::Object({{"schema", InspectionValue::Unsigned(1)},
										{"identity_scope", InspectionValue::Text("revision")},
										{"sets", InspectArray(m_dictionary->sets,
															  [&](const auto& set)
															  {
																  return InspectSetDefinition(set);
															  })},
										{"actions", InspectArray(m_dictionary->actions,
																 [&](const auto& action)
																 {
																	 return InspectActionDefinition(action);
																 })},
										{"bindings", InspectArray(m_dictionary->bindings,
																  [&](const auto& binding)
																  {
																	  return InspectBinding(binding);
																  })}});
	}

  private:
	InspectionValue InspectSetDefinition(const ActionSetDefinition& set) const
	{
		return InspectionValue::Object({{"id", InspectionValue::Unsigned(SetIdentity(set.handle))},
										{"name", InspectionValue::Text(set.name)},
										{"localized_name", InspectionValue::Text(set.localizedName)},
										{"priority", InspectionValue::Unsigned(set.priority)},
										{"attached", InspectionValue::Boolean(set.attached)},
										{"live", InspectionValue::Boolean(set.live)}});
	}
	InspectionValue InspectActionDefinition(const ActionDefinition& action) const
	{
		return InspectionValue::Object({{"id", InspectionValue::Unsigned(ActionIdentity(action.handle))},
										{"set", InspectionValue::Unsigned(SetIdentity(action.set))},
										{"type", InspectionValue::Signed(action.type)},
										{"name", InspectionValue::Text(action.name)},
										{"localized_name", InspectionValue::Text(action.localizedName)},
										{"subaction_paths", InspectArray(action.subactionPaths,
																		 [&](auto path)
																		 {
																			 return InspectPath(path);
																		 })},
										{"live", InspectionValue::Boolean(action.live)}});
	}

  private:
	const ActionInspectionDictionary* m_dictionary;
};
static const char* InactiveReasonName(InactiveReason value)
{
	switch (value)
	{
	case InactiveReason::None:
		return "none";
	case InactiveReason::NotSynchronized:
		return "not_synchronized";
	case InactiveReason::InactiveSet:
		return "inactive_set";
	case InactiveReason::NotFocused:
		return "not_focused";
	case InactiveReason::Unbound:
		return "unbound";
	case InactiveReason::Disconnected:
		return "disconnected";
	case InactiveReason::HigherPriority:
		return "higher_priority";
	case InactiveReason::Unavailable:
		return "unavailable";
	}
	throw std::invalid_argument("Unknown inactive reason");
}
static InspectionValue InspectActionValue(XrPath path, const ActionStateValue& state,
										  const ActionDictionaryEncoder& dictionary)
{
	return InspectionValue::Object(
		{{"subaction_path", dictionary.InspectPath(path)},
		 {"active", InspectionValue::Boolean(state.active)},
		 {"boolean", InspectionValue::Boolean(state.boolean)},
		 {"scalar", InspectionValue::Float(state.scalar)},
		 {"vector", InspectVector(state.vector)},
		 {"changed", InspectionValue::Boolean(state.changed)},
		 {"change_time", InspectionValue::Signed(state.changeTime.value.count())},
		 {"inactive_reason", InspectionValue::Text(InactiveReasonName(state.inactiveReason))}});
}

static InspectionValue InspectActionSource(const ResolvedActionSource& source,
										   const ActionDictionaryEncoder& dictionary)
{
	return InspectionValue::Object({{"device", InspectDevice(source.device)},
									{"binding", dictionary.InspectBinding(source.binding)},
									{"user_paths", InspectArray(source.userPaths,
																[&](auto path)
																{
																	return dictionary.InspectPath(path);
																})},
									{"active", InspectionValue::Boolean(source.active)}});
}

static InspectionValue InspectActionSnapshot(const ActionSnapshot& action, const ActionDictionaryEncoder& dictionary)
{
	std::vector<std::pair<XrPath, ActionStateValue>> values(action.values.begin(), action.values.end());
	std::sort(values.begin(), values.end(),
			  [&](const auto& left, const auto& right)
			  {
				  return dictionary.PathName(left.first) < dictionary.PathName(right.first);
			  });
	return InspectionValue::Object(
		{{"id", InspectionValue::Unsigned(dictionary.ActionIdentity(action.definition.handle))},
		 {"values", InspectArray(values,
								 [&](const auto& value)
								 {
									 return InspectActionValue(value.first, value.second, dictionary);
								 })},
		 {"sources", InspectArray(action.sources,
								  [&](const auto& source)
								  {
									  return InspectActionSource(source, dictionary);
								  })},
		 {"bindings", InspectArray(action.bindings,
								   [&](const auto& binding)
								   {
									   return dictionary.InspectBinding(binding);
								   })}});
}

static InspectionValue InspectActions(const ActionSyncSnapshot& actions, const ActionDictionaryEncoder& dictionary,
									  std::optional<std::uint64_t> revision)
{
	if (!revision)
		return InspectionValue::Object({{"available", InspectionValue::Boolean(false)},
										{"reason", InspectionValue::Text("no_attached_dictionary")},
										{"serial", InspectionValue::Unsigned(actions.serial)}});
	std::vector<std::pair<XrPath, XrPath>> profiles(actions.profiles.begin(), actions.profiles.end());
	std::sort(profiles.begin(), profiles.end(),
			  [&](const auto& left, const auto& right)
			  {
				  return dictionary.PathName(left.first) < dictionary.PathName(right.first);
			  });
	return InspectionValue::Object(
		{{"available", InspectionValue::Boolean(true)},
		 {"dictionary", InspectionValue::Revision(*revision)},
		 {"serial", InspectionValue::Unsigned(actions.serial)},
		 {"time", InspectionValue::Signed(actions.time.value.count())},
		 {"input_revision", InspectionValue::Unsigned(actions.inputRevision)},
		 {"focused", InspectionValue::Boolean(actions.focused)},
		 {"profiles", InspectArray(profiles,
								   [&](const auto& profile)
								   {
									   return InspectionValue::Object(
										   {{"user_path", dictionary.InspectPath(profile.first)},
											{"profile", dictionary.InspectPath(profile.second)}});
								   })},
		 {"active_sets", InspectArray(actions.activeSets,
									  [&](const auto& set)
									  {
										  return InspectionValue::Object(
											  {{"set",
												InspectionValue::Unsigned(dictionary.SetIdentity(set.actionSet))},
											   {"subaction_path", dictionary.InspectPath(set.subactionPath)}});
									  })},
		 {"actions", InspectArray(actions.actions,
								  [&](const auto& action)
								  {
									  return InspectActionSnapshot(action, dictionary);
								  })}});
}
static const char* HapticStatusName(HapticStatus value)
{
	switch (value)
	{
	case HapticStatus::Active:
		return "active";
	case HapticStatus::Expired:
		return "expired";
	case HapticStatus::Stopped:
		return "stopped";
	case HapticStatus::Replaced:
		return "replaced";
	case HapticStatus::FocusLost:
		return "focus_lost";
	}
	throw std::invalid_argument("Unknown haptic status");
}
static InspectionValue InspectHaptics(const std::vector<HapticOutput>& value, const ActionDictionaryEncoder& dictionary)
{
	return InspectArray(
		value,
		[&](const auto& haptic)
		{
			return InspectionValue::Object(
				{{"sequence", InspectionValue::Unsigned(haptic.sequence)},
				 {"device", InspectDevice(haptic.device)},
				 {"action", InspectionValue::Unsigned(dictionary.ActionIdentity(haptic.action))},
				 {"subaction_path", dictionary.InspectPath(haptic.subactionPath)},
				 {"amplitude", InspectionValue::Float(haptic.amplitude)},
				 {"frequency", InspectionValue::Float(haptic.frequency)},
				 {"requested_duration", InspectionValue::Signed(haptic.requestedDuration)},
				 {"start", InspectionValue::Signed(haptic.start.value.count())},
				 {"end", haptic.end ? InspectionValue::Signed(haptic.end->value.count()) : InspectionValue::Null()},
				 {"status", InspectionValue::Text(HapticStatusName(haptic.status))}});
		});
}
class FrameLayerEncoder
{
  public:
	InspectionValue Projection(const ProjectionLayerDescription& layer)
	{
		return Layer(layer);
	}
	InspectionValue InspectLayers(const FrameLayers& value)
	{
		return InspectionValue::Object({{"identity_scope", InspectionValue::Text("frame")},
										{"display_time", InspectionValue::Signed(value.displayTime.value.count())},
										{"blend_mode", InspectionValue::Signed(value.blendMode)},
										{"layers", InspectArray(value.layers,
																[&](const auto& layer)
																{
																	return std::visit(
																		[&](const auto& item)
																		{
																			return Layer(item);
																		},
																		layer);
																})}});
	}

  private:
	template <class Handle> static InspectionValue Identity(std::vector<Handle>& handles, Handle handle)
	{
		if (handle == XR_NULL_HANDLE)
			return InspectionValue::Null();
		auto position = std::find(handles.begin(), handles.end(), handle);
		if (position == handles.end())
		{
			handles.push_back(handle);
			return InspectionValue::Unsigned(handles.size());
		}
		return InspectionValue::Unsigned(static_cast<std::uint64_t>(position - handles.begin()) + 1);
	}
	InspectionValue Image(const LayerImageDescription& value)
	{
		return InspectionValue::Object(
			{{"source", Identity(m_sources, value.source)},
			 {"rectangle",
			  InspectionValue::Object({{"x", InspectionValue::Signed(value.rectangle.offset.x)},
									   {"y", InspectionValue::Signed(value.rectangle.offset.y)},
									   {"width", InspectionValue::Signed(value.rectangle.extent.width)},
									   {"height", InspectionValue::Signed(value.rectangle.extent.height)}})},
			 {"array_slice", InspectionValue::Unsigned(value.arraySlice)}});
	}
	InspectionValue Space(const LayerSpaceDescription& value)
	{
		const auto referenceType = value.referenceType == XR_REFERENCE_SPACE_TYPE_MAX_ENUM
									   ? InspectionValue::Null()
									   : InspectionValue::Signed(value.referenceType);
		return InspectionValue::Object({{"source", Identity(m_spaces, value.source)},
										{"flags", InspectionValue::Unsigned(value.flags)},
										{"world_pose", InspectPoseSample(value.worldPose)},
										{"reference_type", referenceType},
										{"pose_offset", InspectPose(value.poseOffset)}});
	}
	InspectionValue InspectDepth(const LayerDepthDescription& depth)
	{
		return InspectionValue::Object({{"image", Image(depth.image)},
										{"min_depth", InspectionValue::Float(depth.minDepth)},
										{"max_depth", InspectionValue::Float(depth.maxDepth)},
										{"near_z", InspectionValue::Float(depth.nearZ)},
										{"far_z", InspectionValue::Float(depth.farZ)}});
	}
	InspectionValue InspectProjectionView(const ProjectionViewDescription& view)
	{
		// Depth receives its frame-local source identity before color.
		auto depth = view.depth ? InspectDepth(*view.depth) : InspectionValue::Null();
		return InspectionValue::Object({{"pose_in_space", InspectPose(view.poseInSpace)},
										{"world_pose", InspectPoseSample(view.worldPose)},
										{"fov", InspectFov(view.fov)},
										{"color", Image(view.color)},
										{"depth", std::move(depth)}});
	}
	InspectionValue Layer(const ProjectionLayerDescription& layer)
	{
		return InspectionValue::Object({{"kind", InspectionValue::Text("projection")},
										{"space", Space(layer.space)},
										{"views", InspectArray(layer.views,
															   [&](const auto& view)
															   {
																   return InspectProjectionView(view);
															   })}});
	}
	InspectionValue Layer(const QuadLayerDescription& value)
	{
		return InspectionValue::Object(
			{{"kind", InspectionValue::Text("quad")},
			 {"space", Space(value.space)},
			 {"eyes", InspectionValue::Signed(value.eyes)},
			 {"pose_in_space", InspectPose(value.poseInSpace)},
			 {"world_pose", InspectPoseSample(value.worldPose)},
			 {"size", InspectionValue::Object({{"width", InspectionValue::Float(value.size.width)},
											   {"height", InspectionValue::Float(value.size.height)}})},
			 {"color", Image(value.color)}});
	}

  private:
	std::vector<XrSwapchain> m_sources;
	std::vector<XrSpace> m_spaces;
};
static const char* FrameStageName(FrameStage value)
{
	switch (value)
	{
	case FrameStage::Issued:
		return "issued";
	case FrameStage::Begun:
		return "begun";
	case FrameStage::Submitted:
		return "submitted";
	case FrameStage::Discarded:
		return "discarded";
	case FrameStage::Cancelled:
		return "cancelled";
	}
	throw std::invalid_argument("Unknown frame stage");
}
static const char* OutputDispositionName(FrameOutputDisposition value)
{
	switch (value)
	{
	case FrameOutputDisposition::Unavailable:
		return "unavailable";
	case FrameOutputDisposition::Submitted:
		return "submitted";
	case FrameOutputDisposition::Clear:
		return "clear";
	case FrameOutputDisposition::Dropped:
		return "dropped";
	}
	throw std::invalid_argument("Unknown output disposition");
}
static const char* DropReasonName(FrameDropReason value)
{
	switch (value)
	{
	case FrameDropReason::None:
		return "none";
	case FrameDropReason::NativeCapacity:
		return "native_capacity";
	case FrameDropReason::PresentationCapacity:
		return "presentation_capacity";
	}
	throw std::invalid_argument("Unknown drop reason");
}
static InspectionValue InspectPrediction(const FramePrediction& prediction)
{
	return InspectionValue::Object({{"frame_id", InspectionValue::Unsigned(prediction.id.value)},
									{"tick", InspectionValue::Unsigned(prediction.displayTick.value)},
									{"display_time", InspectionValue::Signed(prediction.displayTime.value.count())},
									{"period", InspectionValue::Signed(prediction.displayPeriod.count())}});
}

static InspectionValue InspectSubmissionSummary(const FrameSubmissionState& submission)
{
	return InspectionValue::Object(
		{{"display_time", InspectionValue::Signed(submission.displayTime.value.count())},
		 {"layer_count", InspectionValue::Unsigned(submission.layerCount)},
		 {"blend_mode", InspectionValue::Signed(submission.blendMode)},
		 {"output_disposition", InspectionValue::Text(OutputDispositionName(submission.outputDisposition))},
		 {"drop_reason", InspectionValue::Text(DropReasonName(submission.dropReason))}});
}

static InspectionValue InspectViewQuery(const ViewQuerySnapshot& query)
{
	return InspectionValue::Object({{"correlation", InspectionValue::Text("last_successful_query_at_submission_time")},
									{"display_time", InspectionValue::Signed(query.displayTime.value.count())},
									{"sample_time", InspectionValue::Signed(query.sampleTime.value.count())},
									{"foveated_requested", InspectionValue::Boolean(query.foveatedRequested)},
									{"gaze_active", InspectionValue::Boolean(query.gazeActive)},
									{"fovs", InspectArray(query.fieldsOfView, InspectFov)}});
}

static InspectionValue InspectAvailability(const FrameSubmissionState& submission)
{
	const auto diagnosticAvailability = submission.diagnostics ? "recorded_at_submission" : "not_recorded";
	return InspectionValue::Object(
		{{"applied_foveation", InspectionValue::Text(submission.viewQuery ? "query_recorded" : "not_recorded")},
		 {"applied_commands", InspectionValue::Text(submission.commands ? "recorded" : "not_recorded")},
		 {"tracking", InspectionValue::Text(submission.tracking ? "recorded" : "not_recorded")},
		 {"action_dictionary", InspectionValue::Text(submission.actions.dictionary ? "recorded" : "not_recorded")},
		 {"fb_foveation", InspectionValue::Text("unsupported")},
		 {"validation_scope", InspectionValue::Text("runtime_submitted_layers")},
		 {"native_driver_validation", InspectionValue::Text("not_recorded")},
		 {"validation_events", InspectionValue::Text(diagnosticAvailability)},
		 {"frame_trace", InspectionValue::Text(diagnosticAvailability)}});
}
InspectionValue InspectDiagnostics(const FrameDiagnostics& diagnostics)
{
	static constexpr std::array s_kinds{"wait_started",		"wait_returned",	"issued",			"begun",
										"submitted",		"discarded",		"cancelled",		"native_submitted",
										"gpu_complete",		"gpu_failed",		"display_selected", "display_repeated",
										"display_cleared",	"capture_ready",	"capture_failed",	"preview_dropped",
										"commands_applied", "command_rejected", "api_error",		"validation",
										"display_empty",	"image_retired"};
	static constexpr std::array s_categories{"timing", "performance", "api_error", "validation", "command"};
	return InspectionValue::Object(
		{{"capacity", InspectionValue::Unsigned(diagnostics.capacity)},
		 {"dropped", InspectionValue::Unsigned(diagnostics.dropped)},
		 {"oldest", InspectionValue::Unsigned(diagnostics.oldest)},
		 {"next", InspectionValue::Unsigned(diagnostics.next)},
		 {"truncated", InspectionValue::Boolean(diagnostics.truncated)},
		 {"events",
		  InspectArray(
			  diagnostics.events,
			  [&](const auto& event)
			  {
				  return InspectionValue::Object(
					  {{"sequence", InspectionValue::Unsigned(event.sequence)},
					   {"generation", InspectionValue::Unsigned(event.frame.generation.value)},
					   {"frame_id", InspectionValue::Unsigned(event.frame.frame.value)},
					   {"time", InspectionValue::Signed(event.time.value.count())},
					   {"host_time", InspectionValue::Signed(event.hostTime.count())},
					   {"kind", InspectionValue::Text(s_kinds.at(static_cast<std::size_t>(event.kind)))},
					   {"category", InspectionValue::Text(s_categories.at(static_cast<std::size_t>(event.category)))},
					   {"detail", InspectionValue::Text(std::string_view{event.detail.data(), event.detailLength})},
					   {"detail_truncated", InspectionValue::Boolean(event.detailTruncated)},
					   {"result", InspectionValue::Signed(event.result)},
					   {"related", InspectionValue::Unsigned(event.related)}});
			  })}});
}
static InspectionValue InspectHistoricalImage(const HistoricalImage& image)
{
	const auto& frame = *image.metadata;
	const char* phase = "unavailable";
	if (image.output)
	{
		switch (image.output->PollCompletion())
		{
		case CompletionStatus::Complete:
			phase = "ready";
			break;
		case CompletionStatus::Pending:
			phase = "pending";
			break;
		case CompletionStatus::Failed:
			phase = "failed";
			break;
		}
	}
	else if ((image.view == ImageHistoryView::Quads && !image.quadVisible) ||
			 (frame.submission && !frame.submission->layerCount))
		phase = "clear";
	return InspectionValue::Object(
		{{"frame_id", InspectionValue::Unsigned(frame.key.frame.value)},
		 {"phase", InspectionValue::Text(phase)},
		 {"quad_rectangles", image.view == ImageHistoryView::Quads
								 ? InspectArray(image.quadBounds,
												[](const auto& rectangle)
												{
													return InspectionValue::Object(
														{{"x", InspectionValue::Signed(rectangle.offset.x)},
														 {"y", InspectionValue::Signed(rectangle.offset.y)},
														 {"width", InspectionValue::Signed(rectangle.extent.width)},
														 {"height", InspectionValue::Signed(rectangle.extent.height)}});
												})
								 : InspectionValue::Null()}});
}
InspectionValue InspectImageHistory(const ImageHistorySnapshot& history)
{
	return InspectionValue::Object(
		{{"generation", InspectionValue::Unsigned(history.generation.value)},
		 {"target", InspectionValue::Text(history.view == ImageHistoryView::Quads ? "quad_layers" : "composed_eyes")},
		 {"capacity", InspectionValue::Unsigned(FrameImageHistory::Capacity)},
		 {"evicted", InspectionValue::Unsigned(history.evicted)},
		 {"unavailable", InspectionValue::Unsigned(history.unavailable)},
		 {"entries", InspectArray(history.entries, InspectHistoricalImage)}});
}
InspectionValue InspectImageWindow(const ImageWindowSnapshot& window)
{
	const char* phase = "collecting";
	if (window.interrupted)
		phase = "interrupted";
	else if (window.postCount == FrameImageWindow::PostCount)
		phase = "complete";
	return InspectionValue::Object(
		{{"generation", InspectionValue::Unsigned(window.generation.value)},
		 {"target", InspectionValue::Text(window.view == ImageHistoryView::Quads ? "quad_layers" : "composed_eyes")},
		 {"trigger_frame", InspectionValue::Unsigned(window.trigger.value)},
		 {"capacity", InspectionValue::Unsigned(FrameImageWindow::Capacity)},
		 {"pre_count", InspectionValue::Unsigned(window.preCount)},
		 {"post_count", InspectionValue::Unsigned(window.postCount)},
		 {"post_requested", InspectionValue::Unsigned(FrameImageWindow::PostCount)},
		 {"phase", InspectionValue::Text(phase)},
		 {"entries", InspectArray(window.entries, InspectHistoricalImage)}});
}

InspectionValue InspectProjectionHistory(const FrameHistorySnapshot& history, std::size_t count)
{
	if (!count || count > 64)
		throw std::invalid_argument("Projection history count must be in [1,64]");
	std::vector<InspectionValue> entries;
	entries.reserve(count);
	std::size_t available = 0;
	for (auto frame = history.frames.rbegin(); frame != history.frames.rend(); ++frame)
	{
		if (!(*frame)->submission || !(*frame)->submission->layers)
			continue;
		const auto& submission = *(*frame)->submission;
		const auto& layers = submission.layers->layers;
		FrameLayerEncoder encoder;
		for (std::size_t index = layers.size(); index > 0; --index)
		{
			const auto* projection = std::get_if<ProjectionLayerDescription>(&layers[index - 1]);
			if (!projection)
				continue;
			++available;
			if (entries.size() == count)
				continue;
			entries.push_back(InspectionValue::Object(
				{{"frame_id", InspectionValue::Unsigned((*frame)->key.frame.value)},
				 {"display_time", InspectionValue::Signed(submission.displayTime.value.count())},
				 {"layer", InspectionValue::Unsigned(index - 1)},
				 {"ipd", InspectionValue::Float(submission.viewSettings.ipd)},
				 {"projection", encoder.Projection(*projection)}}));
		}
	}
	std::reverse(entries.begin(), entries.end());
	return InspectionValue::Object({{"generation", InspectionValue::Unsigned(history.generation.value)},
									{"capacity", InspectionValue::Unsigned(history.capacity)},
									{"evicted_frames", InspectionValue::Unsigned(history.evicted)},
									{"omitted_entries", InspectionValue::Unsigned(available - entries.size())},
									{"entries", InspectionValue::Array(std::move(entries))}});
}

std::shared_ptr<const InspectionRecord> InspectFrame(InspectionStore& store, ControlSessionId session,
													 const FrameRecord& frame)
{
	if (auto existing = store.Find(session, frame.key))
		return existing;
	if (!frame.submission)
		throw std::invalid_argument("A submitted CPU frame is required");
	const auto& state = *frame.submission;
	const ActionDictionaryEncoder dictionary{state.actions.dictionary.get()};
	std::vector<FeatureDocument> features;
	features.push_back({"view_settings_v1", InspectViews(state.viewSettings, state.viewLayout).Encode()});
	std::optional<std::size_t> actions, skeleton, commands;
	if (dictionary.Available())
	{
		actions = features.size();
		features.push_back({"action_dictionary_v1", dictionary.InspectDictionary().Encode()});
	}
	if (state.tracking)
	{
		skeleton = features.size();
		features.push_back({"body_skeleton_v1", InspectSkeleton(*state.tracking).Encode()});
	}
	if (state.commands)
	{
		commands = features.size();
		CommandSchedulerSnapshot commandSnapshot;
		commandSnapshot.outcomes = state.commands->outcomes;
		commandSnapshot.applied = state.commands;
		commandSnapshot.seed = state.commands->seed;
		features.push_back({"applied_commands_v1", SimulationState(commandSnapshot, true).Encode()});
	}
	return store.Retain(
		session, frame.key, std::move(features),
		[&](std::span<const std::uint64_t> revisions)
		{
			FrameLayerEncoder layers;
			return InspectionValue::Object(
					   {{"schema", InspectionValue::Unsigned(1)},
						{"session", InspectionValue::Unsigned(session.value)},
						{"generation", InspectionValue::Unsigned(frame.key.generation.value)},
						{"frame_id", InspectionValue::Unsigned(frame.key.frame.value)},
						{"stage", InspectionValue::Text(FrameStageName(frame.stage))},
						{"prediction", InspectPrediction(frame.prediction)},
						{"submission", InspectSubmissionSummary(state)},
						{"input", InspectInput(state.input)},
						{"actions", InspectActions(state.actions, dictionary,
												   actions ? std::optional{revisions[*actions]} : std::nullopt)},
						{"tracking", state.tracking ? InspectTracking(*state.tracking, revisions[*skeleton])
													: InspectionValue::Null()},
						{"view_settings", InspectionValue::Revision(revisions[0])},
						{"view_query", state.viewQuery ? InspectViewQuery(*state.viewQuery) : InspectionValue::Null()},
						{"applied_commands",
						 commands ? InspectionValue::Revision(revisions[*commands]) : InspectionValue::Null()},
						{"frame_trace",
						 state.diagnostics ? InspectDiagnostics(*state.diagnostics) : InspectionValue::Null()},
						{"haptics", InspectHaptics(state.haptics, dictionary)},
						{"layers", state.layers ? layers.InspectLayers(*state.layers) : InspectionValue::Null()},
						{"availability", InspectAvailability(state)}})
				.Encode();
		});
}
} // namespace simxr::control
