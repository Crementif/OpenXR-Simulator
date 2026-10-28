#include "../TestSupport.h"
#include "Pch.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "runtime/SessionPublication.h"
#include "simulation/TrackingSystem.h"

#include <iostream>

namespace simxr::tests
{
static void ControllerAim()
{
	TrackingSystem tracking;
	TrackingState state;
	state.controllers[0].pose.flags = TrackedPoseFlags;
	state.controllers[0].pose.pose.position.x = 0;
	state.controllers[0].aim = PoseSample{};
	state.controllers[0].aim->flags = ValidPoseFlags;
	state.controllers[0].aim->pose.position.x = 2;
	tracking.Publish({Duration{10}}, state);
	state.controllers[0].pose.pose.position.x = 4;
	state.controllers[0].aim->pose.position.x = 6;
	tracking.Publish({Duration{20}}, state);
	const auto middle = tracking.Sample({Duration{15}});
	Require(middle && middle->controllers[0].aim, "Independent aim survives historical sampling");
	Require(middle->controllers[0].pose.pose.position.x == 2 && middle->controllers[0].aim->pose.position.x == 4,
			"Grip and aim interpolate independently");
	Require(middle->controllers[0].aim->flags == ValidPoseFlags &&
				middle->controllers[0].pose.flags == TrackedPoseFlags,
			"Grip tracking does not fabricate aim tracking");
	state.controllers[0].aim->flags = 0;
	const auto lost = tracking.Publish({Duration{30}}, state);
	Require(lost.controllers[0].aim->flags == 0 && lost.controllers[0].pose.flags == TrackedPoseFlags,
			"Aim loss does not invalidate grip or fall back to it");
	Require(tracking.Sample({Duration{25}})->controllers[0].aim->flags == ValidPoseFlags,
			"Aim loss does not rewrite earlier observations");
	state.controllers[0].aim->flags = TrackedPoseFlags;
	state.controllers[0].pose.flags = 0;
	const auto gripLost = tracking.Publish({Duration{40}}, state);
	Require(gripLost.controllers[0].aim->flags == TrackedPoseFlags && gripLost.controllers[0].pose.flags == 0,
			"Grip loss does not invalidate aim");
	state.focused = false;
	const auto unfocused = tracking.Publish({Duration{50}}, state);
	Require(unfocused.controllers[0].aim->flags == 0 && unfocused.controllers[0].pose.flags == 0,
			"Focus loss clears both effective poses");
	Require(unfocused.input.controllers[0].aim->flags == TrackedPoseFlags, "Focus does not erase source state");
	state.focused = true;
	state.controllers[0].connected = false;
	Require(TrackingSystem::Evaluate({Duration{60}}, state, BodyModel{}).controllers[0].aim->flags == 0,
			"Disconnection clears aim validity");
	state.controllers[0].aim.reset();
	Require(!TrackingSystem::Evaluate({Duration{60}}, state, BodyModel{}).controllers[0].aim,
			"Legacy input remains distinguishable from explicit aim loss");

	SetControllerPose command{0, gripLost.input.controllers[0]};
	const auto encoded = control::EncodeSimulationCommand(command);
	const auto decoded = std::get<SetControllerPose>(control::DecodeSimulationCommand(encoded));
	Require(decoded.value.aim && decoded.value.aim->flags == TrackedPoseFlags &&
				decoded.value.aim->pose.position.x == 6 && decoded.value.pose.flags == 0,
			"Canonical recording preserves independent aim and grip validity");
	Require(control::EncodeSimulationCommand(decoded).value == encoded.value, "Aim command roundtrip is exact");
	Require(automation::EditableCommand(command).find("aim = {") != std::string::npos,
			"Editable recording retains the independent aim sample");
	command.value.aim.reset();
	Require(!std::get<SetControllerPose>(control::DecodeSimulationCommand(control::EncodeSimulationCommand(command)))
				 .value.aim,
			"Legacy recorded controller commands remain readable");
	state.controllers[0].aim = PoseSample{};
	state.controllers[0].aim->pose.orientation.w = std::numeric_limits<float>::quiet_NaN();
	bool rejected = false;
	try
	{
		TrackingSystem::Evaluate({Duration{70}}, state, BodyModel{});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Invalid independent aim pose must be rejected");
}

static void ActionPoses()
{
	Instance instance(reinterpret_cast<XrInstance>(951), {}, XR_MAKE_VERSION(1, 0, 0));
	auto& registry = instance.Actions();
	const auto path = [&](const char* text)
	{
		XrPath value = XR_NULL_PATH;
		Require(registry.StringToPath(text, value) == XR_SUCCESS, "Intern controller path");
		return value;
	};
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "poses");
	std::strcpy(setInfo.localizedActionSetName, "Poses");
	XrActionSet set = XR_NULL_HANDLE;
	Require(registry.CreateSet(setInfo, set) == XR_SUCCESS, "Create pose action set");
	std::array<XrAction, 2> actions{};
	const std::array names{"grip", "aim"};
	const auto hand = path("/user/hand/left");
	for (std::size_t index = 0; index < actions.size(); ++index)
	{
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(info.actionName, names[index]);
		std::strcpy(info.localizedActionName, names[index]);
		info.actionType = XR_ACTION_TYPE_POSE_INPUT;
		info.countSubactionPaths = 1;
		info.subactionPaths = &hand;
		Require(registry.CreateAction(set, info, actions[index]) == XR_SUCCESS, "Create pose action");
	}
	const std::array bindings{XrActionSuggestedBinding{actions[0], path("/user/hand/left/input/grip/pose")},
							  XrActionSuggestedBinding{actions[1], path("/user/hand/left/input/aim/pose")}};
	const XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														 path("/interaction_profiles/khr/simple_controller"), 2,
														 bindings.data()};
	Require(registry.SuggestBindings(suggested) == XR_SUCCESS, "Suggest independent controller bindings");
	ActionSyncState sync(registry);
	SessionPublication publication(instance, sync);
	InputSnapshot input;
	input.preferredProfiles[0] = "/interaction_profiles/khr/simple_controller";
	Require(publication.PublishInput(input) == XR_SUCCESS, "Set controller profile");
	const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
	Require(sync.Attach(attach, input) == XR_SUCCESS, "Attach pose action set");
	const XrActiveActionSet active{set, XR_NULL_PATH};
	const XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
	TrackingState state;
	state.controllers[0].pose.flags = TrackedPoseFlags;
	state.controllers[0].pose.pose.position.x = 1;
	state.controllers[0].aim = PoseSample{};
	state.controllers[0].aim->flags = ValidPoseFlags;
	state.controllers[0].aim->pose.position.x = 3;
	const RuntimeTime time{Duration{100}};
	publication.PublishTracking(time, state, {}, true);
	Require(sync.Sync(syncInfo, publication.Input().Snapshot(), true) == XR_SUCCESS, "Sync pose actions");
	Require(publication.ActionPose(actions[0], hand, time).pose.position.x == 1 &&
				publication.ActionPose(actions[1], hand, time).pose.position.x == 3,
			"Action binding selects independent pose");
	state.controllers[0].aim->flags = 0;
	publication.PublishTracking({Duration{200}}, state, {}, true);
	Require(publication.ActionPose(actions[1], hand, {Duration{200}}).flags == 0 &&
				publication.ActionPose(actions[0], hand, {Duration{200}}).flags == TrackedPoseFlags,
			"Invalid aim must not fall back to valid grip");
	state.controllers[0].aim.reset();
	publication.PublishTracking({Duration{300}}, state, {}, true);
	Require(publication.ActionPose(actions[1], hand, {Duration{300}}).pose.position.x == 1,
			"Legacy controller action retains grip-as-aim behavior");
}
} // namespace simxr::tests
int main()
{
	try
	{
		simxr::tests::ControllerAim();
		simxr::tests::ActionPoses();
		std::cout << "Independent controller aim checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
