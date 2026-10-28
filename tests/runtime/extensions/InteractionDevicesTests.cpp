#include "runtime/extensions/EyeGazeInteraction.h"
#include "runtime/extensions/TrackerInteraction.h"
#include "simulation/SpaceSystem.h"
#include "math/Math.h"
#include "../../TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

static void CheckTrackerInventory()
{
	using namespace simxr;
	const std::array extensions{XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME};
	EnabledExtensions enabled(extensions);
	ActionRegistry registry(reinterpret_cast<XrInstance>(300), enabled);
	EventQueue events;
	TrackerInteraction trackers(registry, enabled, events);
	const auto initialEvents = events.Observe();
	const auto watermark = initialEvents.empty() ? 0 : initialEvents.back().sequence;
	TrackingSnapshot snapshot;
	snapshot.time = {Duration{10}};
	snapshot.trackers.clear();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
	{
		TrackerSample sample;
		sample.device = {{501 + index}, TrackerRole::Unassigned};
		sample.pose = {IdentityPose(), TrackedPoseFlags, snapshot.time};
		sample.pose.pose.position.x = static_cast<float>(index);
		snapshot.trackers.push_back(sample);
	}
	snapshot.trackers.back().pose.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	std::vector<XrPath> changed;
	Require(trackers.Publish(snapshot, &changed) == XR_SUCCESS, "Expanded tracker inventory publication failed");
	std::uint32_t count = 0;
	Require(trackers.Enumerate(0, &count, nullptr) == XR_SUCCESS && count == MaxTrackerDevices,
			"Expanded inventory was truncated");
	std::vector<XrViveTrackerPathsHTCX> paths(count, {XR_TYPE_VIVE_TRACKER_PATHS_HTCX});
	Require(trackers.Enumerate(4, &count, paths.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == MaxTrackerDevices,
			"Expanded inventory hid the required capacity");
	Require(trackers.Enumerate(count, &count, paths.data()) == XR_SUCCESS, "Expanded inventory enumeration failed");
	for (std::size_t index = 0; index < paths.size(); ++index)
		Require(registry.PathString(paths[index].persistentPath) == BodyModel::TrackerPersistentPath({501 + index}) &&
					paths[index].rolePath == XR_NULL_PATH,
				"Expanded inventory lost persistent identity or invented an unassigned role");
	const auto arrivals = events.Observe(watermark);
	Require(arrivals.size() == MaxTrackerDevices, "Expanded inventory lost connection events");
	for (std::size_t index = 0; index < arrivals.size(); ++index)
	{
		const auto* arrival = std::get_if<TrackerConnectedEvent>(&arrivals[index].event);
		Require(arrival && arrival->persistentPath == paths[index].persistentPath,
				"Connection event was associated with the wrong device");
	}
	InputSnapshot input;
	input.devices.push_back(
		{{InputDeviceKind::EyeGaze, 0}, {"/user/eyes_ext"}, "/interaction_profiles/ext/eye_gaze_interaction"});
	trackers.ContributeInput(snapshot, input);
	Require(input.devices.size() == MaxTrackerDevices + 1 && InputState::Validate(input) == XR_SUCCESS,
			"Expanded tracker actions lost devices or unrelated gaze");
	Require(trackers.Pose(snapshot, {564}).flags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT &&
				trackers.Pose(snapshot, {564}).pose.position.x == 63,
			"Tracker identity lookup lost partial tracking validity");
	auto invalid = snapshot;
	invalid.time = {Duration{100}};
	invalid.trackers.push_back({{{900}, TrackerRole::Unassigned}, {IdentityPose(), TrackedPoseFlags}});
	Require(trackers.Publish(invalid) == XR_ERROR_LIMIT_REACHED && trackers.Paths().size() == MaxTrackerDevices,
			"Over-capacity inventory changed published devices");
	bool rejected = false;
	try
	{
		trackers.ContributeInput(invalid, input);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && input.devices.size() == MaxTrackerDevices + 1,
			"Over-capacity input changed the existing device batch");
	invalid = snapshot;
	invalid.trackers.back().device.identity = invalid.trackers.front().device.identity;
	Require(trackers.Publish(invalid) == XR_ERROR_VALIDATION_FAILURE, "Duplicate expanded identity accepted");
	std::reverse(snapshot.trackers.begin(), snapshot.trackers.end());
	snapshot.time = {Duration{11}};
	Require(trackers.Publish(snapshot, &changed) == XR_SUCCESS && changed.empty() &&
				events.Observe(arrivals.back().sequence).empty(),
			"Reordering devices changed identity or repeated arrivals");
	snapshot.trackers.front().device.role = TrackerRole::Camera;
	snapshot.time = {Duration{12}};
	Require(trackers.Publish(snapshot, &changed) == XR_SUCCESS && !changed.empty(),
			"Role assignment beyond the fourth device was lost");
	const auto assigned = trackers.Paths().back();
	Require(assigned.identity.value == 564 && assigned.persistentPath == paths.back().persistentPath &&
				registry.PathString(assigned.rolePath) == BodyModel::TrackerRolePath(TrackerRole::Camera),
			"Role assignment replaced persistent identity");
	invalid = snapshot;
	invalid.trackers[1].device.role = TrackerRole::Camera;
	Require(trackers.Publish(invalid) == XR_SUCCESS && trackers.Paths().back().rolePath == assigned.rolePath,
			"Shared role rejected independently identified devices");
	const auto sharedPaths = trackers.Paths();
	Require(sharedPaths.size() == MaxTrackerDevices &&
			sharedPaths[62].rolePath == sharedPaths[63].rolePath &&
			sharedPaths[62].persistentPath != sharedPaths[63].persistentPath,
			"Shared-role enumeration hid a role or collapsed persistent paths");
	trackers.ContributeInput(invalid, input);
	const auto cameraPath = BodyModel::TrackerRolePath(TrackerRole::Camera);
	const auto selectedCamera = [&]()
	{
		std::uint64_t selected = 0;
		for (const auto& device : input.devices)
			if (std::ranges::find(device.userPaths, cameraPath) != device.userPaths.end())
			{
				Require(!selected, "Shared role mapped to multiple action devices");
				selected = device.identity.value;
			}
		return selected;
	};
	Require(InputState::Validate(input) == XR_SUCCESS && selectedCamera() == 563,
			"Shared role did not select the lowest connected identity");
	std::reverse(invalid.trackers.begin(), invalid.trackers.end());
	trackers.ContributeInput(invalid, input);
	Require(selectedCamera() == 563, "Enumeration order changed role selection");
	for (auto& tracker : invalid.trackers)
		if (tracker.device.identity.value == 563)
			tracker.device.connected = false;
	trackers.ContributeInput(invalid, input);
	Require(InputState::Validate(input) == XR_SUCCESS && selectedCamera() == 564,
			"Shared role did not transfer when its selected device disconnected");
	snapshot.trackers.erase(snapshot.trackers.begin());
	snapshot.time = {Duration{13}};
	Require(trackers.Publish(snapshot, &changed) == XR_SUCCESS &&
				std::ranges::find(changed, assigned.persistentPath) != changed.end(),
			"Removed device did not invalidate its persistent path");
	trackers.ContributeInput(snapshot, input);
	Require(input.devices.size() == MaxTrackerDevices &&
				std::ranges::none_of(input.devices,
									 [](const InputDevice& device)
									 {
										 return device.identity == InputDeviceId{InputDeviceKind::Tracker, 564};
									 }),
			"Removed tracker retained an action source");
	snapshot.trackers.clear();
	snapshot.time = {Duration{14}};
	Require(trackers.Publish(snapshot) == XR_SUCCESS && trackers.Enumerate(0, &count, nullptr) == XR_SUCCESS &&
				count == 0,
			"Empty inventory kept phantom trackers");
	trackers.ContributeInput(snapshot, input);
	Require(input.devices.size() == 1 && input.devices.front().identity.kind == InputDeviceKind::EyeGaze,
			"Empty tracker inventory removed unrelated input");
}
int main()
{
	CheckTrackerInventory();
	using namespace simxr;
	const std::array extensions{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME};
	EnabledExtensions enabled(extensions);
	ActionRegistry registry(reinterpret_cast<XrInstance>(100), enabled);
	EventQueue events;
	TrackerInteraction trackers(registry, enabled, events);
	EyeGazeInteraction gaze(enabled);
	std::uint32_t initialCount = 0;
	Require(trackers.Enumerate(0, &initialCount, nullptr) == XR_SUCCESS && initialCount == 4, "Tracker inventory unavailable before session/frame");
	TrackingSystem tracking;
	TrackingState state;
	state.gaze.yaw = 0.2f;
	RuntimeTime time{Duration{1000000000}};
	auto first = tracking.Publish(time, state);
	auto path = [&](std::string_view text)
	{
		XrPath result;
		Require(registry.StringToPath(text, result) == XR_SUCCESS, "Interaction path");
		return result;
	};
	const auto eyes = path("/user/eyes_ext");
	const auto waist = path("/user/vive_tracker_htcx/role/waist");
	const auto persistent = path(BodyModel::TrackerPersistentPath({1}));
	const auto eyeProfile = path("/interaction_profiles/ext/eye_gaze_interaction");
	const auto trackerProfile = path("/interaction_profiles/htc/vive_tracker_htcx");
	auto makeSet = [&](const char* name, std::uint32_t priority)
	{
		XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strcpy(info.actionSetName, name);
		std::strcpy(info.localizedActionSetName, name);
		info.priority = priority;
		XrActionSet result;
		Require(registry.CreateSet(info, result) == XR_SUCCESS, "Create interaction set");
		return result;
	};
	const auto low = makeSet("devices", 0), high = makeSet("priority", 10);
	auto makeAction = [&](XrActionSet set, const char* name, XrActionType type, std::span<const XrPath> users)
	{
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(info.actionName, name);
		std::strcpy(info.localizedActionName, name);
		info.actionType = type;
		info.countSubactionPaths = static_cast<std::uint32_t>(users.size());
		info.subactionPaths = users.data();
		XrAction result;
		Require(registry.CreateAction(set, info, result) == XR_SUCCESS, "Create generalized action");
		return result;
	};
	const std::array eyeUsers{eyes};
	const std::array trackerUsers{waist, persistent};
	const std::array persistentUsers{persistent};
	const auto eyeAction = makeAction(low, "gaze", XR_ACTION_TYPE_POSE_INPUT, eyeUsers);
	const auto trackerAction = makeAction(low, "tracker", XR_ACTION_TYPE_POSE_INPUT, trackerUsers);
	const auto trackerButton = makeAction(low, "button", XR_ACTION_TYPE_BOOLEAN_INPUT, trackerUsers);
	const auto trackerOutput = makeAction(low, "output", XR_ACTION_TYPE_VIBRATION_OUTPUT, trackerUsers);
	const auto priorityAction = makeAction(high, "priority_pose", XR_ACTION_TYPE_POSE_INPUT, persistentUsers);
	const XrActionSuggestedBinding eyeBinding{eyeAction, path("/user/eyes_ext/input/gaze_ext/pose")};
	XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, eyeProfile, 1, &eyeBinding};
	Require(registry.SuggestBindings(suggestion) == XR_SUCCESS, "Eye profile binding");
	const auto persistentText = BodyModel::TrackerPersistentPath({1});
	const std::array trackerBindings{
		XrActionSuggestedBinding{trackerAction, path("/user/vive_tracker_htcx/role/waist/input/grip/pose")},
		XrActionSuggestedBinding{trackerAction, path(persistentText + "/input/grip/pose")},
		XrActionSuggestedBinding{priorityAction, path(persistentText + "/input/grip/pose")},
		XrActionSuggestedBinding{trackerButton, path("/user/vive_tracker_htcx/role/waist/input/trigger/value")},
		XrActionSuggestedBinding{trackerOutput, path("/user/vive_tracker_htcx/role/waist/output/haptic")},
		XrActionSuggestedBinding{trackerOutput, path(persistentText + "/output/haptic")}};
	suggestion = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, trackerProfile, static_cast<std::uint32_t>(trackerBindings.size()), trackerBindings.data()};
	Require(registry.SuggestBindings(suggestion) == XR_SUCCESS, "Role and persistent tracker bindings");
	InputState inputs;
	InputSnapshot input;
	input.time = time;
	gaze.ContributeInput(first, input);
	trackers.ContributeInput(first, input);
	for (auto& device : input.devices)
		if (device.identity == InputDeviceId{InputDeviceKind::Tracker, 1})
			device.controls.trigger = 0.8f;
	Require(inputs.Publish(input) == XR_SUCCESS, "Generalized device publication");
	ActionSyncState actions(registry);
	const std::array sets{low, high};
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 2, sets.data()};
	Require(actions.Attach(attach, inputs.Snapshot()) == XR_SUCCESS, "Attach generalized inputs");
	XrPath initialProfile = eyeProfile;
	Require(actions.CurrentProfile(eyes, initialProfile) == XR_SUCCESS && initialProfile == XR_NULL_PATH, "Gaze profile selected before first sync");
	const auto session = reinterpret_cast<XrSession>(101);
	Require(trackers.Publish(first) == XR_SUCCESS, "Initial tracker inventory");
	std::uint32_t count = 0;
	Require(trackers.Enumerate(0, &count, nullptr) == XR_SUCCESS && count == 4, "Tracker count-only enumeration");
	std::array<XrViveTrackerPathsHTCX, 4> paths{};
	for (auto& value : paths)
		value.type = XR_TYPE_VIVE_TRACKER_PATHS_HTCX;
	Require(trackers.Enumerate(3, &count, paths.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == 4, "Tracker enumeration capacity");
	XrBaseOutStructure unknown{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB, nullptr};
	paths[0].next = &unknown;
	Require(trackers.Enumerate(4, &count, paths.data()) == XR_SUCCESS && paths[0].next == &unknown && paths[0].persistentPath == persistent && paths[0].rolePath == waist, "Tracker enumeration paths/chain");
	unknown.next = &unknown;
	Require(trackers.Enumerate(4, &count, paths.data()) == XR_ERROR_VALIDATION_FAILURE, "Tracker output cycle");
	unknown.next = nullptr;
	const XrActiveActionSet activeLow{low, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &activeLow};
	Require(actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS, "Synchronize non-hand devices");
	auto activePose = [&](XrAction action, XrPath user)
	{
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, user};
		XrActionStatePose result{XR_TYPE_ACTION_STATE_POSE};
		Require(actions.GetPose(info, result) == XR_SUCCESS, "Pose activity query");
		return result.isActive != XR_FALSE;
	};
	Require(activePose(eyeAction, eyes) && activePose(trackerAction, waist) && activePose(trackerAction, persistent) && activePose(trackerAction, XR_NULL_PATH), "Per-device/subaction aggregation");
	std::vector<ResolvedActionSource> sources;
	auto resourcePoseActive = [&](XrAction action, XrPath user)
	{
		std::vector<ResolvedActionSource> retained;
		Require(actions.SpaceSources(action, user, retained) == XR_SUCCESS, "Retained pose source query");
		return !retained.empty();
	};
	Require(actions.PoseSources(trackerAction, waist, sources) == XR_SUCCESS && !sources.empty() && sources.front().device == InputDeviceId{InputDeviceKind::Tracker, 1}, "Frozen tracker identity");
	XrPath current;
	Require(actions.CurrentProfile(eyes, current) == XR_SUCCESS && current == eyeProfile, "Eye interaction profile");
	Require(actions.CurrentProfile(persistent, current) == XR_SUCCESS && current == trackerProfile, "Persistent tracker profile");
	Require(actions.CurrentProfile(path("/user/hand/left"), current) == XR_SUCCESS && current == XR_NULL_PATH, "Hand fallback selected incompatible eye/tracker profile");
	XrHapticActionInfo output{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, trackerOutput, waist};
	std::vector<InputDeviceId> targets;
	std::vector<std::size_t> hands;
	Require(actions.OutputTargets(output, targets) == XR_SUCCESS && targets == std::vector<InputDeviceId>{{InputDeviceKind::Tracker, 1}} &&
		actions.OutputDevices(output, hands) == XR_SUCCESS && hands.empty(), "Typed tracker haptic destination/hand compatibility");
	std::string localized;
	Require(registry.LocalizedName(trackerBindings[1].binding, trackerProfile, XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT, localized) == XR_SUCCESS && localized == persistentText, "Persistent source localized name");
	XrActionStateGetInfo getButton{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, trackerButton, waist};
	XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
	Require(actions.GetBoolean(getButton, button) == XR_SUCCESS && button.currentState && !button.changedSinceLastSync, "First generalized button synchronization");
	input.time = {Duration{1000000001}};
	for (auto& device : input.devices)
		if (device.identity == InputDeviceId{InputDeviceKind::Tracker, 1})
			device.controls.trigger = 0.1f;
	Require(inputs.Publish(input) == XR_SUCCESS && actions.GetBoolean(getButton, button) == XR_SUCCESS && button.currentState, "Unsynchronized tracker control changed action query");
	Require(actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && actions.GetBoolean(getButton, button) == XR_SUCCESS && !button.currentState &&
		button.changedSinceLastSync && button.lastChangeTime == input.time.value.count(), "Generalized button change timestamp");
	SpaceSystem spaces(session, tracking,
		[&](XrAction action, XrPath user, RuntimeTime requested)
		{
			std::vector<ResolvedActionSource> resolved;
			if (XR_FAILED(actions.SpaceSources(action, user, resolved)) || resolved.empty()) return PoseSample{};
			auto sample = tracking.Sample(requested);
			if (!sample) return PoseSample{};
			return resolved.front().device.kind == InputDeviceKind::EyeGaze ? gaze.Pose(*sample) : trackers.Pose(*sample, {resolved.front().device.value});
		},
		[&](XrAction action, XrPath user)
		{
			auto definition = registry.Action(action);
			return definition ? registry.ValidateSubaction(*definition, user) : XR_ERROR_HANDLE_INVALID;
		});
	XrSpace gazeSpace{}, base{};
	Require(spaces.CreateActionSpace(eyeAction, eyes, {{0, 0, 0, 1}, {0.1f, 0, 0}}, gazeSpace) == XR_SUCCESS &&
		spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), base) == XR_SUCCESS, "Gaze offset space");
	SpaceLocation location;
	Require(spaces.Locate(gazeSpace, base, time, location) == XR_SUCCESS && location.pose.eyeGazeSampleTime == time, "Gaze sample time lost through offset/base transform");
	state.gaze.yaw = 0.4f;
	auto second = tracking.Publish({Duration{2000000000}}, state);
	Require(spaces.Locate(gazeSpace, base, {Duration{1500000000}}, location) == XR_SUCCESS && location.pose.eyeGazeSampleTime == RuntimeTime{Duration{1500000000}}, "Interpolated gaze sample time");
	Require(spaces.Locate(gazeSpace, base, {Duration{2500000000}}, location) == XR_SUCCESS && location.pose.eyeGazeSampleTime == second.time, "Clamped gaze sample time");
	state.trackers[0].tracked = false;
	auto lost = tracking.Publish({Duration{3000000000}}, state);
	input.time = lost.time;
	trackers.ContributeInput(lost, input);
	gaze.ContributeInput(lost, input);
	Require(inputs.Publish(input) == XR_SUCCESS && actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && activePose(trackerAction, persistent), "Tracking loss disconnected action source");
	Require(trackers.Pose(lost, {1}).flags == ValidPoseFlags, "Tracker inferred tracking flags");
	const std::array priorities{XrActiveActionSet{low, XR_NULL_PATH}, XrActiveActionSet{high, XR_NULL_PATH}};
	sync.countActiveActionSets = 2;
	sync.activeActionSets = priorities.data();
	Require(actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && !activePose(trackerAction, waist) && resourcePoseActive(priorityAction, persistent), "Persistent/role aliases bypassed physical source priority");
	XrSpace retainedTracker{};
	Require(spaces.CreateActionSpace(priorityAction, persistent, IdentityPose(), retainedTracker) == XR_SUCCESS,
		"Create retained tracker action space");
	Require(registry.DestroyAction(priorityAction) == XR_SUCCESS && !registry.Action(priorityAction) && registry.ActionResource(priorityAction),
		"Retire tracker action resource");
	XrSpace invalidNewSpace{};
	Require(spaces.CreateActionSpace(priorityAction, persistent, IdentityPose(), invalidNewSpace) == XR_ERROR_HANDLE_INVALID &&
		actions.PoseSources(priorityAction, persistent, sources) == XR_ERROR_HANDLE_INVALID,
		"Retired pose action accepted through public API");
	Require(actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && !activePose(trackerAction, waist) &&
		actions.SpaceSources(priorityAction, persistent, sources) == XR_SUCCESS && !sources.empty() &&
		spaces.Locate(retainedTracker, base, lost.time, location) == XR_SUCCESS && location.pose.flags == ValidPoseFlags,
		"Retired pose resource stopped locating or processing priority");
	state.trackers[0].role = TrackerRole::Camera;
	state.trackers[1].role = TrackerRole::Waist;
	auto reassigned = tracking.Publish({Duration{4000000000}}, state);
	Require(std::abs(reassigned.trackers[0].pose.pose.position.y - first.trackers[0].pose.pose.position.y) < 0.00001f, "Role assignment moved physical tracker anchor");
	Require(trackers.Publish(reassigned) == XR_SUCCESS, "Tracker role reassignment");
	Require(actions.SpaceSources(priorityAction, persistent, sources) == XR_SUCCESS && sources.front().device.value == 1, "Role reassignment rewrote synchronized identity");
	input.time = reassigned.time;
	trackers.ContributeInput(reassigned, input);
	Require(inputs.Publish(input) == XR_SUCCESS && actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && activePose(trackerAction, waist), "Role binding did not follow reassigned device");
	Require(actions.PoseSources(trackerAction, waist, sources) == XR_SUCCESS && sources.front().device.value == 2, "Reassigned role source identity");
	state.trackers[0].connected = false;
	auto disconnected = tracking.Publish({Duration{5000000000}}, state);
	std::vector<XrPath> changedPaths;
	Require(trackers.Publish(disconnected, &changedPaths) == XR_SUCCESS && trackers.Enumerate(0, &count, nullptr) == XR_SUCCESS && count == 3, "Disconnected tracker enumeration");
	Require(changedPaths.size() == 2 && std::find(changedPaths.begin(), changedPaths.end(), persistent) != changedPaths.end(),
		"Tracker changes did not report owned bound user paths");
	Require(trackers.Publish(disconnected, &changedPaths) == XR_SUCCESS && changedPaths.empty(), "Unchanged inventory repeated user-path change");
	input.time = disconnected.time;
	trackers.ContributeInput(disconnected, input);
	auto frozen = actions.Snapshot();
	Require(actions.CurrentProfile(persistent, current) == XR_SUCCESS && current == trackerProfile,
		"Inventory publication changed profile before sync");
	Require(actions.Snapshot().serial == frozen.serial && actions.Snapshot().time == frozen.time && resourcePoseActive(priorityAction, persistent),
		"Inventory publication changed synchronized action state");
	Require(actions.SpaceSources(priorityAction, persistent, sources) == XR_SUCCESS && !sources.empty() && sources.front().device.value == 1,
		"Inventory publication changed synchronized pose source");
	Require(inputs.Publish(input) == XR_SUCCESS && actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && !resourcePoseActive(priorityAction, persistent), "Disconnected persistent action active");
	Require(actions.CurrentProfile(persistent, current) == XR_SUCCESS && current == XR_NULL_PATH, "Disconnected tracker retained current profile");
	state.trackers[0].role = TrackerRole::Unassigned;
	auto disconnectedRole = tracking.Publish({Duration{5500000000}}, state);
	Require(trackers.Publish(disconnectedRole) == XR_SUCCESS, "Disconnected role assignment");
	state.trackers[0].connected = true;
	auto unassigned = tracking.Publish({Duration{6000000000}}, state);
	Require(trackers.Publish(unassigned) == XR_SUCCESS && trackers.Enumerate(4, &count, paths.data()) == XR_SUCCESS && paths[0].rolePath == XR_NULL_PATH, "Unassigned persistent tracker");
	state.gaze.available = false;
	auto noGaze = tracking.Publish({Duration{7000000000}}, state);
	Require(activePose(eyeAction, eyes), "New gaze sensor sample changed synchronized activity");
	Require(spaces.Locate(gazeSpace, base, noGaze.time, location) == XR_SUCCESS && location.pose.flags == 0 &&
		location.pose.eyeGazeSampleTime.value_or(RuntimeTime{}).value.count() == 0, "Unavailable gaze retained sample time");
	input.time = noGaze.time;
	gaze.ContributeInput(noGaze, input);
	trackers.ContributeInput(noGaze, input);
	Require(inputs.Publish(input) == XR_SUCCESS && actions.Sync(sync, inputs.Snapshot(), true) == XR_SUCCESS && !activePose(eyeAction, eyes) && resourcePoseActive(priorityAction, persistent), "Gaze availability/tracker independence");
	actions.SetFocused(false);
	Require(!resourcePoseActive(priorityAction, persistent), "Unfocused generalized action");
	std::size_t connectedEvents = 0, profileEvents = 0;
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (events.Poll(event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_VIVE_TRACKER_CONNECTED_HTCX)
		{
			auto& connected = reinterpret_cast<const XrEventDataViveTrackerConnectedHTCX&>(event);
			auto pointer = reinterpret_cast<std::uintptr_t>(connected.paths);
			Require(pointer >= reinterpret_cast<std::uintptr_t>(&event) && pointer + sizeof(*connected.paths) <= reinterpret_cast<std::uintptr_t>(&event) + sizeof(event), "Tracker event retained foreign pointer");
			Require(connected.paths->type == XR_TYPE_VIVE_TRACKER_PATHS_HTCX && !connected.paths->next && registry.PathString(connected.paths->persistentPath), "Tracker event payload");
			++connectedEvents;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
			++profileEvents;
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
	Require(connectedEvents == 8 && profileEvents == 0, "Tracker connection/role/profile event delivery");
	TrackingState partialHead;
	partialHead.head.flags = ValidPoseFlags | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	auto partial = TrackingSystem::Evaluate({Duration{8000000000}}, partialHead, BodyModel{});
	Require(partial.gaze.flags == ValidPoseFlags, "Eye tracking flags became uncoupled");
	Require(trackers.Publish(first) == XR_ERROR_TIME_INVALID, "Older inventory changed current tracker state");
	const auto retainedPath = trackers.Paths()[0].persistentPath;
	trackers.BeginSession();
	Require(trackers.Paths()[0].persistentPath == retainedPath && trackers.Publish(first) == XR_SUCCESS,
		"Session restart must preserve tracker paths and accept its restarted deterministic clock");
	Require(trackers.Publish(unassigned) == XR_SUCCESS && trackers.Publish(first) == XR_ERROR_TIME_INVALID,
		"Restarted session must still reject its own stale inventory");
	InputSnapshot malformed;
	malformed.devices.push_back({{InputDeviceKind::Tracker, 1}, {"/user/vive_tracker_htcx/serial//broken"}, "/interaction_profiles/htc/vive_tracker_htcx"});
	Require(inputs.Publish(malformed) == XR_ERROR_PATH_FORMAT_INVALID, "Malformed device alias accepted");
	EnabledExtensions none;
	ActionRegistry secondRegistry(reinterpret_cast<XrInstance>(200), enabled);
	EventQueue secondEvents;
	TrackerInteraction other(secondRegistry, enabled, secondEvents);
	Require(other.Publish(unassigned) == XR_SUCCESS && secondRegistry.PathString(other.Paths()[0].persistentPath) == persistentText, "Persistent identity changed across instances");
	TrackerInteraction disabled(registry, none, events);
	Require(disabled.Enumerate(0, &count, nullptr) == XR_ERROR_FUNCTION_UNSUPPORTED, "Disabled tracker extension");
	EyeGazeInteraction unavailable(enabled, false);
	InputSnapshot unsupportedInput;
	unavailable.ContributeInput(first, unsupportedInput);
	Require(!unavailable.Supports() && !unsupportedInput.devices.front().connected && unavailable.Pose(first).flags == 0, "Unsupported gaze capability");
	std::cout << "Generalized eye/tracker interaction tests passed\n";
}
