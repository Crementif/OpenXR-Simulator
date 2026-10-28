#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetActions.h"
#include "headset/OpenXrRuntime.h"
#include "math/Math.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace simxr::headset::test
{
static std::vector<std::string> s_paths{1};
static std::map<XrAction, std::string> s_actions;
static std::map<std::string, std::vector<std::string>> s_bindings;
static bool s_focused = true, s_active = true, s_pressed = true;
static bool s_neutralAxes = false;
static std::string s_profile = "/interaction_profiles/oculus/touch_controller";
static std::uint64_t s_space = 100;
static bool s_trackerEnabled = false, s_attached = false;
static std::vector<XrSpace> s_trackerSpaces;
static std::vector<XrViveTrackerPathsHTCX> s_trackers;
static XrResult XRAPI_CALL EnumerateTrackers(XrInstance, std::uint32_t capacity, std::uint32_t* count,
											 XrViveTrackerPathsHTCX* paths)
{
	*count = static_cast<std::uint32_t>(s_trackers.size());
	if (capacity < s_trackers.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(s_trackers.begin(), s_trackers.end(), paths);
	return XR_SUCCESS;
}
static std::vector<XrPath> s_hapticHands, s_stoppedHands;
static XrHapticVibration s_vibration{};
static XrResult s_hapticResult = XR_SUCCESS;
static XrPath Path(const std::string& name)
{
	const auto found = std::find(s_paths.begin(), s_paths.end(), name);
	if (found != s_paths.end())
		return static_cast<XrPath>(found - s_paths.begin());
	s_paths.push_back(name);
	return s_paths.size() - 1;
}
static bool Bound(const XrActionStateGetInfo& info)
{
	const auto prefix = s_actions.at(info.action) + ":" + s_paths.at(info.subactionPath) + "/";
	const auto& bindings = s_bindings.at(s_profile);
	return s_active && std::any_of(bindings.begin(), bindings.end(),
								   [&](const auto& binding)
								   {
									   return binding.starts_with(prefix);
								   });
}
} // namespace simxr::headset::test
namespace simxr::headset
{
OpenXrRuntime::~OpenXrRuntime() = default;
const std::vector<XrExtensionProperties>& OpenXrRuntime::Extensions() const
{
	static std::vector<XrExtensionProperties> extensions;
	extensions.clear();
	if (test::s_trackerEnabled)
	{
		XrExtensionProperties extension{XR_TYPE_EXTENSION_PROPERTIES};
		std::strcpy(extension.extensionName, XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
		extension.extensionVersion = 3;
		extensions.push_back(extension);
	}
	return extensions;
}
PFN_xrVoidFunction OpenXrRuntime::Procedure(const char*) const
{
	Require(test::s_trackerEnabled, "Optional tracker procedure requested without support");
	return reinterpret_cast<PFN_xrVoidFunction>(test::EnumerateTrackers);
}
XrInstance OpenXrRuntime::Instance() const
{
	return reinterpret_cast<XrInstance>(1);
}
bool OpenXrRuntime::ResourcesPreserved() const
{
	return false;
}
void CheckXr(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(operation);
}
} // namespace simxr::headset
using namespace simxr::headset::test;
extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession, const XrHapticActionInfo* info,
														 const XrHapticBaseHeader* feedback)
	{
		Require(s_actions.at(info->action) == "vibration" && feedback->type == XR_TYPE_HAPTIC_VIBRATION,
				"Use the native vibration action");
		s_hapticHands.push_back(info->subactionPath);
		s_vibration = *reinterpret_cast<const XrHapticVibration*>(feedback);
		return s_hapticResult;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession, const XrHapticActionInfo* info)
	{
		s_stoppedHands.push_back(info->subactionPath);
		return s_focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char* text, XrPath* path)
	{
		*path = Path(text);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance, XrPath path, std::uint32_t capacity, std::uint32_t* count,
												  char* buffer)
	{
		const auto& text = s_paths.at(path);
		*count = static_cast<std::uint32_t>(text.size() + 1);
		if (capacity < *count)
			return XR_ERROR_SIZE_INSUFFICIENT;
		std::copy(text.begin(), text.end(), buffer);
		buffer[text.size()] = '\0';
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* set)
	{
		s_attached = false;
		*set = reinterpret_cast<XrActionSet>(1);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet)
	{
		Require(s_trackerSpaces.empty(), "Tracker spaces outlived their action set");
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet, const XrActionCreateInfo* info, XrAction* action)
	{
		Require(!s_attached, "Action created after action-set attachment");
		*action = reinterpret_cast<XrAction>(s_actions.size() + 10);
		s_actions[*action] = info->actionName;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance,
																	   const XrInteractionProfileSuggestedBinding* info)
	{
		auto& bindings = s_bindings[s_paths.at(info->interactionProfile)];
		for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i)
			bindings.push_back(s_actions.at(info->suggestedBindings[i].action) + ":" +
							   s_paths.at(info->suggestedBindings[i].binding));
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*)
	{
		if (s_trackerEnabled)
			Require(s_trackerSpaces.size() == 17 + s_trackers.size(),
					"Tracker roles were not predeclared before attachment");
		s_attached = true;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space)
	{
		*space = reinterpret_cast<XrSpace>(++s_space);
		if (s_actions.at(info->action).starts_with("tracker_"))
			s_trackerSpaces.push_back(*space);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space)
	{
		*space = reinterpret_cast<XrSpace>(++s_space);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space)
	{
		std::erase(s_trackerSpaces, space);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace, XrTime, XrSpaceLocation* location)
	{
		location->pose = simxr::IdentityPose();
		location->locationFlags =
			std::find(s_trackerSpaces.begin(), s_trackerSpaces.end(), space) != s_trackerSpaces.end()
				? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
				: 0;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo*)
	{
		return s_focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession, XrPath, XrInteractionProfileState* state)
	{
		state->interactionProfile = Path(s_profile);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* info,
														   XrActionStateBoolean* state)
	{
		const auto& name = s_actions.at(info->action);
		const bool left = s_paths.at(info->subactionPath) == "/user/hand/left";
		state->isActive = Bound(*info);
		// Deliberately nonzero inactive values must never leak into controls.
		state->currentState = s_pressed && (name != "primary" || left);
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* info,
														 XrActionStateFloat* state)
	{
		const auto& name = s_actions.at(info->action);
		state->isActive = Bound(*info);
		state->currentState = s_neutralAxes				 ? .02f
							  : name == "trigger"		 ? .25f
							  : name == "grip_force"	 ? .6f
							  : name == "trackpad_force" ? .9f
														 : .75f;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo* info,
															XrActionStateVector2f* state)
	{
		const bool trackpad = s_actions.at(info->action) == "trackpad";
		state->isActive = Bound(*info);
		state->currentState = s_neutralAxes && !trackpad ? XrVector2f{}
							  : trackpad				 ? XrVector2f{-.3f, .8f}
														 : XrVector2f{.4f, -.2f};
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession, const XrActionStateGetInfo*,
														XrActionStatePose* state)
	{
		state->isActive = s_active;
		return XR_SUCCESS;
	}
}
namespace simxr::tests
{
static void Trackers()
{
	s_trackerEnabled = true;
	s_focused = s_active = true;
	s_trackers = {
		{XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr, Path("/device/stable"), Path("/user/vive_tracker_htcx/role/waist")}};
	headset::OpenXrRuntime runtime;
	headset::HeadsetActions actions(runtime);
	actions.Open(reinterpret_cast<XrSession>(1), true);
	auto sample = actions.Sample(200, reinterpret_cast<XrSpace>(1));
	Require(sample.trackers && sample.trackers->size() == 1 &&
				sample.trackers->front().persistentPath == "/device/stable" &&
				sample.trackers->front().location.locationFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
			"Tracker provider was not sampled through native action synchronization");
	s_focused = false;
	sample = actions.Sample(201, reinterpret_cast<XrSpace>(1));
	Require(sample.trackers && sample.trackers->size() == 1 && !sample.trackers->front().location.locationFlags,
			"Focus loss discarded inventory or retained valid tracker poses");
	s_focused = true;
	s_trackers.front().rolePath = Path("/user/vive_tracker_htcx/role/chest");
	sample = actions.Sample(202, reinterpret_cast<XrSpace>(1));
	Require(sample.trackers->front().persistentPath == "/device/stable" &&
				sample.trackers->front().role == TrackerRole::Chest,
			"Role reassignment changed physical identity");
	s_trackers.clear();
	sample = actions.Sample(203, reinterpret_cast<XrSpace>(1));
	Require(sample.trackers && sample.trackers->empty(), "Disconnect did not produce an explicit empty inventory");
}
static void Actions()
{
	headset::OpenXrRuntime runtime;
	headset::HeadsetActions actions(runtime);
	actions.Open(reinterpret_cast<XrSession>(1));
	const auto& touch = s_bindings.at("/interaction_profiles/oculus/touch_controller");
	const auto has = [&](const std::string& binding)
	{
		return std::find(touch.begin(), touch.end(), binding) != touch.end();
	};
	Require(has("primary:/user/hand/left/input/x/click") && has("primary:/user/hand/right/input/a/click") &&
				has("secondary:/user/hand/left/input/y/click") && has("secondary:/user/hand/right/input/b/click"),
			"Correct handed face button bindings");
	Require(has("stick:/user/hand/left/input/thumbstick") && has("squeeze:/user/hand/right/input/squeeze/value") &&
				!has("menu:/user/hand/right/input/system/click"),
			"Analog bindings omit reserved system button");
	auto sample = actions.Sample(100, reinterpret_cast<XrSpace>(1));
	const auto& controls = sample.hands[0].controls;
	Require(controls.primary && controls.secondary && controls.menu && controls.thumbClick && controls.primaryTouch &&
				controls.secondaryTouch && controls.triggerTouch && controls.thumbTouch && controls.thumbrestTouch &&
				controls.grip == .75f && controls.stick.x == .4f && controls.stick.y == -.2f,
			"Read all Touch controls");
	Require(!sample.hands[1].controls.primary && !sample.hands[1].controls.menu && controls.connected &&
				sample.hands[0].grip.locationFlags == 0,
			"Handed inactivity and tracking loss preserve independent control activity");
	Require(sample.hands[0].profile == headset::HeadsetControllerProfile::Touch, "Retain observed physical profile");
	const auto& indexBindings = s_bindings.at("/interaction_profiles/valve/index_controller");
	for (const auto* path :
		 {"trigger_click:/user/hand/left/input/trigger/click", "primary:/user/hand/left/input/a/click",
		  "secondary:/user/hand/left/input/b/click", "grip_force:/user/hand/right/input/squeeze/force",
		  "trackpad_force:/user/hand/left/input/trackpad/force", "trackpad:/user/hand/right/input/trackpad",
		  "trackpad_touch:/user/hand/left/input/trackpad/touch"})
		Require(std::find(indexBindings.begin(), indexBindings.end(), path) != indexBindings.end(),
				"Index-specific component binding");
	Require(std::none_of(indexBindings.begin(), indexBindings.end(),
						 [](const auto& binding)
						 {
							 return binding.find("/system/") != std::string::npos ||
									binding.find("/thumbrest/") != std::string::npos ||
									binding.find("/trackpad/click") != std::string::npos;
						 }),
			"Index does not invent reserved or absent components");
	s_profile = "/interaction_profiles/valve/index_controller";
	sample = actions.Sample(101, reinterpret_cast<XrSpace>(1));
	Require(sample.hands[0].profile == headset::HeadsetControllerProfile::Index &&
				sample.hands[0].controls.triggerClick == true && sample.hands[0].trigger.currentState == .25f &&
				sample.hands[0].controls.gripForce == .6f && sample.hands[0].controls.trackpadForce == .9f &&
				sample.hands[0].controls.trackpad.x == -.3f && sample.hands[0].controls.trackpad.y == .8f &&
				sample.hands[0].controls.trackpadTouch && !sample.hands[0].controls.thumbrestTouch &&
				!sample.hands[0].controls.menu,
			"Index analog force and touch inputs stay independent");
	for (const auto& [profile, expected] : std::array{
			 std::pair{"/interaction_profiles/htc/vive_controller", headset::HeadsetControllerProfile::Vive},
			 std::pair{"/interaction_profiles/microsoft/motion_controller", headset::HeadsetControllerProfile::Motion}})
	{
		s_profile = profile;
		const auto& bindings = s_bindings.at(s_profile);
		const bool vive = expected == headset::HeadsetControllerProfile::Vive;
		for (const auto* hand : {"/user/hand/left", "/user/hand/right"})
		{
			const std::string prefix(hand);
			for (const auto& [action, component] :
				 std::array{std::pair{"squeeze_click", "/input/squeeze/click"},
							std::pair{"trackpad_click", "/input/trackpad/click"},
							std::pair{"trackpad_touch", "/input/trackpad/touch"},
							std::pair{"menu", "/input/menu/click"}, std::pair{"vibration", "/output/haptic"}})
				Require(std::count(bindings.begin(), bindings.end(), std::string(action) + ":" + prefix + component) ==
							1,
						"Bind both hands' digital squeeze, trackpad, menu and haptics");
		}
		Require(bindings.size() == (vive ? 20 : 22) &&
					std::none_of(bindings.begin(), bindings.end(),
								 [](const auto& binding)
								 {
									 return binding.find("/system/") != std::string::npos ||
											binding.find("/force") != std::string::npos ||
											binding.find("/squeeze/value") != std::string::npos;
								 }),
				"Vive and motion bindings contain only supported components");
		sample = actions.Sample(102, reinterpret_cast<XrSpace>(1));
		for (const auto& hand : sample.hands)
		{
			Require(hand.profile == expected && hand.controls.physicalProfile == profile && hand.controls.connected &&
						hand.controls.grip == 1 && hand.controls.menu && hand.controls.trackpadClick &&
						hand.controls.trackpadTouch && hand.controls.trackpad.x == -.3f &&
						hand.controls.trackpad.y == .8f && hand.controls.gripForce == 0 &&
						hand.controls.trackpadForce == 0 && !hand.controls.primary,
					"Digital squeeze and trackpad observations preserve independent components");
			Require(vive ? hand.controls.triggerClick == true && hand.controls.stick.x == 0 && !hand.controls.thumbClick
						 : !hand.controls.triggerClick && hand.controls.stick.x == .4f && hand.controls.thumbClick,
					"Vive trigger click and motion thumbstick do not invent each other's controls");
		}
		s_pressed = false;
		sample = actions.Sample(103, reinterpret_cast<XrSpace>(1));
		for (const auto& hand : sample.hands)
			Require(hand.controls.connected && hand.controls.grip == 0 && !hand.controls.trackpadClick &&
						!hand.controls.trackpadTouch && !hand.controls.menu && !hand.controls.thumbClick &&
						(vive ? hand.controls.triggerClick == false : !hand.controls.triggerClick.has_value()),
					"Active digital release clears squeeze, clicks and touches without disconnecting");
		s_pressed = true;
	}
	s_active = false;
	sample = actions.Sample(101, reinterpret_cast<XrSpace>(1));
	Require(!sample.hands[0].controls.primary && sample.hands[0].controls.grip == 0 &&
				sample.hands[0].controls.stick.x == 0 && !sample.hands[0].controls.connected &&
				!sample.hands[0].controls.triggerClick.has_value() && sample.hands[0].controls.gripForce == 0 &&
				sample.hands[0].controls.trackpadForce == 0 && sample.hands[0].controls.trackpad.x == 0,
			"Inactive actions clear stale values");
	Require(has("vibration:/user/hand/left/output/haptic") && has("vibration:/user/hand/right/output/haptic"),
			"Bind vibration independently for both Touch hands");
	const auto& simple = s_bindings.at("/interaction_profiles/khr/simple_controller");
	Require(std::count(simple.begin(), simple.end(), "vibration:/user/hand/left/output/haptic") == 1,
			"Simple controllers expose vibration too");
	Require(actions.ApplyHaptic(0, .75f, 120, 5'000'000'000) && s_hapticHands.back() == Path("/user/hand/left") &&
				s_vibration.duration == 2'000'000'000 && s_vibration.amplitude == .75f && s_vibration.frequency == 120,
			"Bound duration while preserving vibration parameters");
	Require(actions.ApplyHaptic(1, .25f, 0, 1000) && s_hapticHands.back() == Path("/user/hand/right") &&
				s_vibration.duration == 1000,
			"Right hand uses its own output and remaining duration");
	bool rejected = false;
	try
	{
		actions.ApplyHaptic(2, .5f, 0, 1000);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Reject an invalid haptic hand");
	for (unsigned fault = 0; fault < 4; ++fault)
	{
		rejected = false;
		try
		{
			actions.ApplyHaptic(0, fault == 0 ? 1.5f : .5f,
								fault == 1	 ? -1.f
								: fault == 2 ? std::numeric_limits<float>::quiet_NaN()
											 : 0.f,
								fault == 3 ? 0 : 1000);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Reject invalid vibration parameters before native submission");
	}
	s_hapticResult = XR_ERROR_RUNTIME_FAILURE;
	rejected = false;
	try
	{
		actions.ApplyHaptic(0, .5f, 0, 1000);
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	Require(rejected, "Surface physical runtime output failure");
	s_hapticResult = XR_SESSION_NOT_FOCUSED;
	Require(!actions.ApplyHaptic(1, .5f, 0, 1000), "Unfocused result does not claim haptic delivery");
	s_hapticResult = XR_SUCCESS;
	s_focused = false;
	sample = actions.Sample(102, reinterpret_cast<XrSpace>(1));
	Require(s_stoppedHands.size() == 2 && s_stoppedHands.front() == Path("/user/hand/left") &&
				s_stoppedHands.back() == Path("/user/hand/right"),
			"Focus loss stops outstanding physical feedback");
	Require(!sample.hands[0].controls.connected && !sample.hands[0].controls.triggerTouch,
			"Focus loss clears controls");
	s_focused = true;
	s_active = true;
	sample = actions.Sample(103, reinterpret_cast<XrSpace>(1));
	Require(sample.focused && !sample.controlsReady && !sample.hands[0].controls.menu &&
				sample.hands[0].controls.grip == 0 && sample.hands[0].trigger.currentState == 0,
			"Focus return cannot replay held buttons or axes");
	s_pressed = false;
	sample = actions.Sample(104, reinterpret_cast<XrSpace>(1));
	Require(!sample.controlsReady && sample.hands[0].controls.stick.x == 0,
			"Released buttons do not bypass a held analog axis");
	s_neutralAxes = true;
	sample = actions.Sample(105, reinterpret_cast<XrSpace>(1));
	Require(!sample.controlsReady && sample.hands[0].trigger.currentState == 0,
			"Analog noise and untouched trackpad coordinates permit a clean release boundary");
	s_pressed = true;
	s_neutralAxes = false;
	sample = actions.Sample(106, reinterpret_cast<XrSpace>(1));
	Require(sample.controlsReady && sample.hands[0].controls.menu && sample.hands[0].controls.grip == 1,
			"A fresh press after neutral recovery reaches the application");
	Require(actions.ApplyHaptic(0, 0, 0, 1000) && s_stoppedHands.size() == 3,
			"Zero amplitude explicitly stops the selected output");
}
} // namespace simxr::tests
int main()
{
	try
	{
		simxr::tests::Actions();
		simxr::tests::Trackers();
		std::cout << "Native controller action checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
