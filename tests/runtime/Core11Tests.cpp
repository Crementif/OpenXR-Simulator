#include "../TestSupport.h"
#include "Pch.h"
#include "math/Math.h"
#include "runtime/DispatchTable.h"
#include "runtime/Runtime.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"

namespace simxr::tests
{
static XrPath Path(ActionRegistry& registry, const char* name)
{
	XrPath path = XR_NULL_PATH;
	Require(registry.StringToPath(name, path) == XR_SUCCESS, "Intern core profile path");
	return path;
}

static void Profiles(XrVersion version)
{
	Instance instance(reinterpret_cast<XrInstance>(991), {}, version);
	auto& registry = instance.Actions();
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "input");
	std::strcpy(setInfo.localizedActionSetName, "Input");
	XrActionSet set{};
	Require(registry.CreateSet(setInfo, set) == XR_SUCCESS, "Create core profile set");
	XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
	std::strcpy(actionInfo.actionName, "pose");
	std::strcpy(actionInfo.localizedActionName, "Pose");
	actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
	XrAction action{};
	Require(registry.CreateAction(set, actionInfo, action) == XR_SUCCESS, "Create core profile pose");
	for (const auto profile :
		 {"/interaction_profiles/khr/simple_controller", "/interaction_profiles/meta/touch_pro_controller",
		  "/interaction_profiles/meta/touch_plus_controller", "/interaction_profiles/htc/vive_cosmos_controller",
		  "/interaction_profiles/bytedance/pico4_controller"})
	{
		const XrActionSuggestedBinding binding{action, Path(registry, "/user/hand/left/input/grip_surface/pose")};
		const XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														Path(registry, profile), 1, &binding};
		Require(registry.SuggestBindings(info) ==
					(version >= XR_MAKE_VERSION(1, 1, 0) ? XR_SUCCESS : XR_ERROR_PATH_UNSUPPORTED),
				"Core profile availability did not follow the instance version");
	}
	ActionSyncState sync(registry);
	InputSnapshot input;
	const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
	Require(sync.Attach(attach, input) == XR_SUCCESS, "Attach core profiles");
	const XrActiveActionSet active{set, XR_NULL_PATH};
	const XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
	Require(sync.Sync(info, input, true) == XR_SUCCESS, "Sync core profiles");
	XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, XR_NULL_PATH};
	XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
	Require(sync.GetPose(get, pose) == XR_SUCCESS && pose.isActive == (version >= XR_MAKE_VERSION(1, 1, 0)),
			"Grip surface pose did not become active");
}

static void Spaces()
{
	TrackingSystem tracking;
	TrackingState input;
	tracking.Publish({Duration{1000000}}, input);
	SpaceSystem spaces(reinterpret_cast<XrSession>(992), tracking, {}, {}, nullptr, false, true);
	SpaceSystem legacy(reinterpret_cast<XrSession>(993), tracking);
	XrSpace floor{}, local{}, view{};
	Require(legacy.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, IdentityPose(), floor) ==
				XR_ERROR_REFERENCE_SPACE_UNSUPPORTED,
			"Core 1.0 exposed local floor without an extension");
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, IdentityPose(), floor) == XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) == XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), view) == XR_SUCCESS,
			"Create batch query spaces");
	ReferenceSpaceState origins;
	origins.local.position = {3, 1.2f, 4};
	origins.stage.position.y = 0.2f;
	Require(spaces.StartGeneration({Duration{1}}, origins) == XR_SUCCESS, "Set elevated local origin");
	std::array handles{floor, view};
	std::array<XrSpaceLocationData, 2> data{};
	std::array<XrSpaceVelocityData, 2> speeds{};
	XrSpaceVelocities velocities{XR_TYPE_SPACE_VELOCITIES, nullptr, 2, speeds.data()};
	XrSpaceLocations locations{XR_TYPE_SPACE_LOCATIONS, &velocities, 2, data.data()};
	XrSpacesLocateInfo info{XR_TYPE_SPACES_LOCATE_INFO, nullptr, local, 1000000, 2, handles.data()};
	Require(LocateSpaces(spaces, &info, &locations) == XR_SUCCESS, "Locate batch with velocities");
	Require(std::abs(data[0].pose.position.y + 1) < 0.0001f && data[0].pose.position.x == 0 &&
				data[0].pose.position.z == 0,
			"Local floor did not use local horizontal origin and floor height");
	XrSpaceVelocity speed{XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation single{XR_TYPE_SPACE_LOCATION, &speed};
	Require(LocateSpace(spaces, view, local, info.time, &single) == XR_SUCCESS &&
				data[1].locationFlags == single.locationFlags && data[1].pose.position.x == single.pose.position.x &&
				speeds[1].velocityFlags == speed.velocityFlags,
			"Batch query disagreed with single-space query");
	data[0].pose.position.y = 123;
	handles[1] = XR_NULL_HANDLE;
	Require(LocateSpaces(spaces, &info, &locations) == XR_ERROR_HANDLE_INVALID && data[0].pose.position.y == 123,
			"Invalid batch partially wrote output");
	handles[1] = view;
	velocities.velocityCount = 1;
	Require(LocateSpaces(spaces, &info, &locations) == XR_ERROR_VALIDATION_FAILURE,
			"Mismatched velocity count accepted");
	velocities.velocityCount = 2;
	info.time = 0;
	Require(LocateSpaces(spaces, &info, &locations) == XR_ERROR_TIME_INVALID, "Invalid batch time accepted");
}

static void XRAPI_PTR Function() {}

static void RuntimeVersion()
{
	Runtime runtime{ExtensionRegistry{SimulationExtensions()}};
	XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(info.applicationInfo.applicationName, "Core API compatibility");
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 99);
	Runtime::InstanceRef instance;
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS, "Create core 1.1 instance");
	Runtime::SessionRef session;
	Require(runtime.CreateSession(instance, session) == XR_SUCCESS, "Create core 1.1 session");
	XrSpace floor{};
	Require(session->Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, IdentityPose(), floor) ==
				XR_SUCCESS,
			"Instance version did not reach session spaces");
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	Require(session->Ready(timing.epoch) == XR_SUCCESS, "Ready core quad session");
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr,
								   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET};
	Require(session->Begin(begin, timing) == XR_SUCCESS && session->ActiveViewCount() == 4,
			"Core quad configuration required an extension");
	const std::array entries{DispatchEntry{"xrLocateSpaces", Function, false, true}};
	const ExtensionRegistry extensions{{}};
	const DispatchTable table(entries, extensions);
	PFN_xrVoidFunction function{};
	EnabledExtensions legacy;
	Require(table.Resolve("xrLocateSpaces", &legacy, function) == XR_ERROR_FUNCTION_UNSUPPORTED && !function &&
				table.Resolve("xrLocateSpaces", &instance->Extensions(), function) == XR_SUCCESS && function,
			"Core command discovery did not follow the instance version");
}
} // namespace simxr::tests

int main()
{
	simxr::tests::Profiles(XR_MAKE_VERSION(1, 0, 0));
	simxr::tests::Profiles(XR_MAKE_VERSION(1, 1, 0));
	simxr::tests::Spaces();
	simxr::tests::RuntimeVersion();
}
