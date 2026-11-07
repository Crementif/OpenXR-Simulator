#include "runtime/extensions/HandTracking.h"
#include "Pch.h"
#include "math/Math.h"
#include "runtime/validation/StructureChain.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace simxr
{
namespace
{
bool Has(XrSpaceLocationFlags flags, XrSpaceLocationFlags bits)
{
	return (flags & bits) == bits;
}

XrVector3f Add(XrVector3f a, XrVector3f b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

XrVector3f Subtract(XrVector3f a, XrVector3f b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

XrVector3f Scale(XrVector3f v, float s)
{
	return {v.x * s, v.y * s, v.z * s};
}

float Dot(XrVector3f a, XrVector3f b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

XrVector3f Cross(XrVector3f a, XrVector3f b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

XrVector3f Normalize(XrVector3f v)
{
	const float length = std::sqrt(Dot(v, v));
	return length > 0 ? Scale(v, 1 / length) : v;
}

float Radians(float degrees)
{
	return degrees * std::numbers::pi_v<float> / 180;
}

XrQuaternionf FromBasis(XrVector3f x, XrVector3f y, XrVector3f z)
{
	const float trace = x.x + y.y + z.z;
	XrQuaternionf q{};
	if (trace > 0)
	{
		const float s = std::sqrt(trace + 1) * 2;
		q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
	}
	else if (x.x > y.y && x.x > z.z)
	{
		const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
		q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
	}
	else if (y.y > z.z)
	{
		const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
		q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
	}
	else
	{
		const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
		q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
	}
	const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return {q.x / length, q.y / length, q.z / length, q.w / length};
}

// Hand space is (thumb side, finger direction, out of the palm), relative to the middle metacarpal.
struct FingerShape
{
	XrVector3f metacarpal;
	XrVector3f knuckle;
	float spread;
	std::array<float, 3> lengths;
};

constexpr std::array<FingerShape, 4> FingerShapes{{
	{{0.012f, -0.030f, 0}, {0.025f, 0.035f, 0}, 0.08f, {0.040f, 0.024f, 0.022f}},
	{{0.002f, -0.030f, 0}, {0.003f, 0.037f, 0}, 0.00f, {0.044f, 0.028f, 0.024f}},
	{{-0.010f, -0.030f, 0}, {-0.017f, 0.033f, 0}, -0.06f, {0.041f, 0.026f, 0.023f}},
	{{-0.020f, -0.028f, 0}, {-0.035f, 0.026f, 0}, -0.14f, {0.033f, 0.019f, 0.020f}},
}};
constexpr std::array<float, 3> FingerBendDegrees{80, 100, 65};
constexpr std::array<float, 4> FingerRadii{0.010f, 0.009f, 0.008f, 0.007f};
constexpr XrVector3f ThumbMetacarpal{0.018f, -0.028f, 0.008f};
constexpr XrVector3f ThumbKnuckle{0.042f, 0.002f, 0.018f};
constexpr std::array<float, 2> ThumbLengths{0.032f, 0.028f};
constexpr std::array<float, 2> ThumbBendDegrees{45, 70};
constexpr XrVector3f GripInHand{0, 0.015f, 0.02f};
constexpr float RestingIndexCurl = 0.25f;
constexpr float RestingGripCurl = 0.15f;
constexpr float RestingThumbCurl = 0.55f;
} // namespace

HandJointPoses ControllerHandJoints(std::size_t side, const ControllerControls& controls)
{
	// Grip space: -Z runs from the little finger to the thumb and +X points out of the left palm and into the right
	// palm, so the open fingers run along -Y.
	const XrVector3f thumbSide{0, 0, -1};
	const XrVector3f fingers{0, -1, 0};
	const XrVector3f palm{side == 1 ? -1.0f : 1.0f, 0, 0};
	auto direction = [&](XrVector3f v) {
		return Add(Add(Scale(thumbSide, v.x), Scale(fingers, v.y)), Scale(palm, v.z));
	};
	auto joint = [&](XrVector3f position, XrVector3f forward, XrVector3f dorsal, float radius) {
		const auto z = Normalize(Scale(direction(forward), -1));
		const auto hint = direction(dorsal);
		const auto y = Normalize(Subtract(hint, Scale(z, Dot(hint, z))));
		return HandJointPose{{FromBasis(Cross(y, z), y, z), direction(Subtract(position, GripInHand))}, radius};
	};

	const bool thumbResting = controls.thumbTouch || controls.thumbrestTouch || controls.primaryTouch ||
							  controls.secondaryTouch || controls.trackpadTouch || controls.primary ||
							  controls.secondary || controls.thumbClick || controls.trackpadClick;
	const float indexRest = controls.triggerTouch ? RestingIndexCurl : 0;
	const float indexCurl = indexRest + (1 - indexRest) * std::clamp(controls.trigger, 0.0f, 1.0f);
	const float gripCurl = RestingGripCurl + (1 - RestingGripCurl) * std::clamp(controls.grip, 0.0f, 1.0f);
	const float thumbCurl = thumbResting ? RestingThumbCurl : 0;

	const XrVector3f outOfPalm{0, 0, 1};
	const XrVector3f backOfHand{0, 0, -1};
	HandJointPoses joints{};
	joints[XR_HAND_JOINT_PALM_EXT] = joint({0.0025f, 0.0035f, 0}, {0, 1, 0}, backOfHand, 0.025f);
	joints[XR_HAND_JOINT_WRIST_EXT] = joint({0, -0.040f, 0}, {0, 1, 0}, backOfHand, 0.022f);

	const auto thumbOpen = Normalize({0.55f, 0.8f, 0.15f});
	const XrVector3f thumbAcross{-0.6f, 0, 0.8f};
	const auto thumbBend = Normalize(Subtract(thumbAcross, Scale(thumbOpen, Dot(thumbAcross, thumbOpen))));
	joints[XR_HAND_JOINT_THUMB_METACARPAL_EXT] =
		joint(ThumbMetacarpal, Subtract(ThumbKnuckle, ThumbMetacarpal), Scale(thumbBend, -1), 0.012f);
	auto at = ThumbKnuckle;
	float angle = 0;
	for (std::size_t bone = 0; bone < 2; ++bone)
	{
		angle += Radians(ThumbBendDegrees[bone] * thumbCurl);
		const auto forward = Add(Scale(thumbOpen, std::cos(angle)), Scale(thumbBend, std::sin(angle)));
		const auto dorsal = Subtract(Scale(thumbOpen, std::sin(angle)), Scale(thumbBend, std::cos(angle)));
		joints[XR_HAND_JOINT_THUMB_PROXIMAL_EXT + bone] = joint(at, forward, dorsal, 0.010f - 0.001f * bone);
		at = Add(at, Scale(forward, ThumbLengths[bone]));
		if (bone == 1)
			joints[XR_HAND_JOINT_THUMB_TIP_EXT] = joint(at, forward, dorsal, 0.008f);
	}

	for (std::size_t finger = 0; finger < FingerShapes.size(); ++finger)
	{
		const auto& shape = FingerShapes[finger];
		const float curl = finger == 0 ? indexCurl : gripCurl;
		const auto first = XR_HAND_JOINT_INDEX_METACARPAL_EXT + finger * 5;
		joints[first] = joint(shape.metacarpal, Subtract(shape.knuckle, shape.metacarpal), backOfHand, FingerRadii[0]);
		const auto open = Normalize({shape.spread, 1, 0});
		at = shape.knuckle;
		angle = 0;
		XrVector3f forward = open, dorsal = backOfHand;
		for (std::size_t bone = 0; bone < 3; ++bone)
		{
			angle += Radians(FingerBendDegrees[bone] * curl);
			forward = Add(Scale(open, std::cos(angle)), Scale(outOfPalm, std::sin(angle)));
			dorsal = Subtract(Scale(open, std::sin(angle)), Scale(outOfPalm, std::cos(angle)));
			joints[first + 1 + bone] = joint(at, forward, dorsal, FingerRadii[bone + 1]);
			at = Add(at, Scale(forward, shape.lengths[bone]));
		}
		joints[first + 4] = joint(at, forward, dorsal, FingerRadii[3] - 0.001f);
	}
	return joints;
}

HandTracking::HandTracking(XrSession owner, const TrackingSystem& tracking, const InputState& input,
						   const SpaceSystem& spaces, const EnabledExtensions& enabled)
	: m_owner(owner), m_tracking(tracking), m_input(input), m_spaces(spaces), m_enabled(enabled)
{
	if (owner == XR_NULL_HANDLE)
		throw std::invalid_argument("Missing hand tracking owner");
}

XrResult HandTracking::Create(const XrHandTrackerCreateInfoEXT* info, XrHandTrackerEXT* output)
{
	if (!Enabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!info || info->type != XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT || !output)
		return XR_ERROR_VALIDATION_FAILURE;
	const bool dataSources = Enabled(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
	const std::array known{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
	StructureChain chain(info->next);
	if (XR_FAILED(chain.Validate(std::span(known).first(dataSources ? 1 : 0))))
		return XR_ERROR_VALIDATION_FAILURE;
	if ((info->hand != XR_HAND_LEFT_EXT && info->hand != XR_HAND_RIGHT_EXT) ||
		info->handJointSet != XR_HAND_JOINT_SET_DEFAULT_EXT)
		return XR_ERROR_VALIDATION_FAILURE;
	auto tracker = std::make_unique<Tracker>();
	tracker->side = info->hand == XR_HAND_RIGHT_EXT ? 1 : 0;
	if (const auto sources = dataSources ? chain.Find<XrHandTrackingDataSourceInfoEXT>(
												   XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT)
										 : nullptr)
	{
		if (sources->requestedDataSourceCount == 0 || !sources->requestedDataSources)
			return XR_ERROR_VALIDATION_FAILURE;
		bool unobstructed = false, controller = false;
		for (std::uint32_t index = 0; index < sources->requestedDataSourceCount; ++index)
		{
			bool& seen = sources->requestedDataSources[index] == XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT
							 ? unobstructed
							 : controller;
			if ((sources->requestedDataSources[index] != XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT &&
				 sources->requestedDataSources[index] != XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT) ||
				seen)
				return XR_ERROR_VALIDATION_FAILURE;
			seen = true;
		}
		// Only controller-driven hands are simulated, so an unobstructed-only tracker never activates.
		tracker->controllerSource = controller;
	}
	*output = m_trackers.Insert(m_owner, std::move(tracker));
	return XR_SUCCESS;
}

XrResult HandTracking::Destroy(XrHandTrackerEXT tracker)
{
	if (!Enabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return m_trackers.Erase(tracker) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult HandTracking::Locate(XrHandTrackerEXT handle, const XrHandJointsLocateInfoEXT* info,
							  XrHandJointLocationsEXT* locations) const
{
	if (!Enabled(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	const auto tracker = m_trackers.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT || !locations ||
		locations->type != XR_TYPE_HAND_JOINT_LOCATIONS_EXT || locations->jointCount != XR_HAND_JOINT_COUNT_EXT ||
		!locations->jointLocations || XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	const std::array known{XR_TYPE_HAND_JOINT_VELOCITIES_EXT, XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
	StructureChain chain(locations->next);
	if (XR_FAILED(chain.Validate(
			std::span(known).first(Enabled(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME) ? 2 : 1))))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto velocities =
		const_cast<XrHandJointVelocitiesEXT*>(chain.Find<XrHandJointVelocitiesEXT>(XR_TYPE_HAND_JOINT_VELOCITIES_EXT));
	if (velocities && (velocities->jointCount != XR_HAND_JOINT_COUNT_EXT || !velocities->jointVelocities))
		return XR_ERROR_VALIDATION_FAILURE;

	PoseSample base;
	const auto result = m_spaces.WorldPose(info->baseSpace, {Duration{info->time}}, base);
	if (XR_FAILED(result))
		return result;
	const auto snapshot = m_tracking.Sample({Duration{info->time}});
	const auto controls = m_input.Snapshot().controllers[tracker->side];
	std::optional<PoseSample> grip;
	if (snapshot && tracker->controllerSource && controls.connected)
	{
		const auto& controller = snapshot->controllers[tracker->side];
		if (controller.connected && Has(controller.pose.flags, ValidPoseFlags) && Has(base.flags, ValidPoseFlags))
			grip = controller.pose;
	}

	locations->isActive = grip ? XR_TRUE : XR_FALSE;
	const auto joints = grip ? ControllerHandJoints(tracker->side, controls) : HandJointPoses{};
	for (std::size_t index = 0; index < XR_HAND_JOINT_COUNT_EXT; ++index)
	{
		auto& output = locations->jointLocations[index];
		if (!grip)
		{
			output = {0, IdentityPose(), 0};
			continue;
		}
		const auto pose = RelativePose(Compose(grip->pose, joints[index].pose), base.pose);
		XrSpaceLocationFlags flags = 0;
		for (auto bit : {XR_SPACE_LOCATION_ORIENTATION_VALID_BIT, XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT})
			if (Has(grip->flags, bit) && Has(base.flags, bit))
				flags |= bit;
		for (auto bit : {XR_SPACE_LOCATION_POSITION_VALID_BIT, XR_SPACE_LOCATION_POSITION_TRACKED_BIT})
			if (Has(grip->flags, bit) && Has(base.flags, bit | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
				flags |= bit;
		output = {FinitePoseFlags(pose, flags), pose, joints[index].radius};
	}
	if (velocities)
		for (std::size_t index = 0; index < XR_HAND_JOINT_COUNT_EXT; ++index)
			velocities->jointVelocities[index] = {0, {}, {}};
	if (Enabled(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME))
		if (auto state = const_cast<XrHandTrackingDataSourceStateEXT*>(
				chain.Find<XrHandTrackingDataSourceStateEXT>(XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT)))
		{
			state->isActive = locations->isActive;
			state->dataSource = XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
		}
	return XR_SUCCESS;
}

bool HandTracking::Owns(XrHandTrackerEXT tracker) const
{
	return static_cast<bool>(m_trackers.Find(tracker, m_owner));
}

void HandTracking::Clear()
{
	auto retired = m_trackers.RetireAll();
}

bool HandTracking::Enabled(std::string_view extension) const
{
	return m_enabled.Contains(extension) && m_enabled.Contains(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
}
} // namespace simxr
