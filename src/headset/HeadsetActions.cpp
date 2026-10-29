#include "headset/HeadsetActions.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"

namespace simxr::headset
{
struct ButtonBinding
{
	const char* name;
	const char* left;
	const char* right;
	const char* index;
	bool ControllerControls::* member;
};

static constexpr std::array s_buttons{
	ButtonBinding{"primary", "/input/x/click", "/input/a/click", "/input/a/click", &ControllerControls::primary},
	ButtonBinding{"secondary", "/input/y/click", "/input/b/click", "/input/b/click", &ControllerControls::secondary},
	ButtonBinding{"menu", "/input/menu/click", nullptr, nullptr, &ControllerControls::menu},
	ButtonBinding{"stick_click", "/input/thumbstick/click", "/input/thumbstick/click", "/input/thumbstick/click",
				  &ControllerControls::thumbClick},
	ButtonBinding{"primary_touch", "/input/x/touch", "/input/a/touch", "/input/a/touch",
				  &ControllerControls::primaryTouch},
	ButtonBinding{"secondary_touch", "/input/y/touch", "/input/b/touch", "/input/b/touch",
				  &ControllerControls::secondaryTouch},
	ButtonBinding{"trigger_touch", "/input/trigger/touch", "/input/trigger/touch", "/input/trigger/touch",
				  &ControllerControls::triggerTouch},
	ButtonBinding{"stick_touch", "/input/thumbstick/touch", "/input/thumbstick/touch", "/input/thumbstick/touch",
				  &ControllerControls::thumbTouch},
	ButtonBinding{"thumbrest_touch", "/input/thumbrest/touch", "/input/thumbrest/touch", nullptr,
				  &ControllerControls::thumbrestTouch},
	ButtonBinding{"trackpad_touch", nullptr, nullptr, "/input/trackpad/touch", &ControllerControls::trackpadTouch},
	ButtonBinding{"trackpad_click", nullptr, nullptr, nullptr, &ControllerControls::trackpadClick}};

HeadsetActions::HeadsetActions(OpenXrRuntime& runtime) : m_runtime(runtime) {}

HeadsetActions::~HeadsetActions()
{
	if (m_runtime.ResourcesPreserved())
		return;
	m_trackers.reset();
	for (const auto space : m_gripSpaces)
		if (space)
			xrDestroySpace(space);
	for (const auto space : m_aimSpaces)
		if (space)
			xrDestroySpace(space);
	if (m_head)
		xrDestroySpace(m_head);
	if (m_set)
		xrDestroyActionSet(m_set);
}

void HeadsetActions::Open(XrSession session, bool trackers)
{
	if (m_set)
		throw std::logic_error("Physical actions are already open");
	m_session = session;
	m_hands = {Path("/user/hand/left"), Path("/user/hand/right")};
	XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(set.actionSetName, "headset_input");
	std::strcpy(set.localizedActionSetName, "Headset input");
	CheckXr(xrCreateActionSet(m_runtime.Instance(), &set, &m_set), "Create physical input action set");
	m_grip = CreateAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT);
	m_aim = CreateAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT);
	m_select = CreateAction("select", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_trigger = CreateAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
	m_triggerClick = CreateAction("trigger_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_squeeze = CreateAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
	m_squeezeClick = CreateAction("squeeze_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_stick = CreateAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
	m_trackpad = CreateAction("trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT);
	m_gripForce = CreateAction("grip_force", XR_ACTION_TYPE_FLOAT_INPUT);
	m_trackpadForce = CreateAction("trackpad_force", XR_ACTION_TYPE_FLOAT_INPUT);
	m_haptic = CreateAction("vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);
	static_assert(s_buttons.size() == std::tuple_size_v<decltype(m_buttons)>);
	for (std::size_t index = 0; index < s_buttons.size(); ++index)
		m_buttons[index] = CreateAction(s_buttons[index].name, XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_simpleProfile = Path("/interaction_profiles/khr/simple_controller");
	m_touchProfile = Path("/interaction_profiles/oculus/touch_controller");
	m_indexProfile = Path("/interaction_profiles/valve/index_controller");
	m_viveProfile = Path("/interaction_profiles/htc/vive_controller");
	m_motionProfile = Path("/interaction_profiles/microsoft/motion_controller");
	for (const auto profile : {m_simpleProfile, m_touchProfile, m_indexProfile, m_viveProfile, m_motionProfile})
	{
		std::vector<XrActionSuggestedBinding> bindings;
		for (const auto* hand : {"/user/hand/left", "/user/hand/right"})
		{
			const std::string prefix(hand);
			bindings.push_back({m_haptic, Path((prefix + "/output/haptic").c_str())});
			bindings.push_back({m_grip, Path((prefix + "/input/grip/pose").c_str())});
			bindings.push_back({m_aim, Path((prefix + "/input/aim/pose").c_str())});
			bindings.push_back(
				{profile != m_simpleProfile ? m_trigger : m_select,
				 Path((prefix + (profile != m_simpleProfile ? "/input/trigger/value" : "/input/select/click"))
						  .c_str())});
			if (profile == m_touchProfile || profile == m_indexProfile)
			{
				bindings.push_back({m_squeeze, Path((prefix + "/input/squeeze/value").c_str())});
				bindings.push_back({m_stick, Path((prefix + "/input/thumbstick").c_str())});
				for (std::size_t index = 0; index < s_buttons.size(); ++index)
					if (const auto suffix =
							profile == m_indexProfile
								? s_buttons[index].index
								: (prefix == "/user/hand/left" ? s_buttons[index].left : s_buttons[index].right))
						bindings.push_back({m_buttons[index], Path((prefix + suffix).c_str())});
			}
			else
				bindings.push_back({m_buttons[2], Path((prefix + "/input/menu/click").c_str())});
			if (profile == m_viveProfile || profile == m_motionProfile)
			{
				bindings.push_back({m_squeezeClick, Path((prefix + "/input/squeeze/click").c_str())});
				bindings.push_back({m_trackpad, Path((prefix + "/input/trackpad").c_str())});
				bindings.push_back({m_buttons[9], Path((prefix + "/input/trackpad/touch").c_str())});
				bindings.push_back({m_buttons[10], Path((prefix + "/input/trackpad/click").c_str())});
				if (profile == m_viveProfile)
					bindings.push_back({m_triggerClick, Path((prefix + "/input/trigger/click").c_str())});
				else
				{
					bindings.push_back({m_stick, Path((prefix + "/input/thumbstick").c_str())});
					bindings.push_back({m_buttons[3], Path((prefix + "/input/thumbstick/click").c_str())});
				}
			}
			if (profile == m_indexProfile)
			{
				bindings.push_back({m_triggerClick, Path((prefix + "/input/trigger/click").c_str())});
				bindings.push_back({m_trackpad, Path((prefix + "/input/trackpad").c_str())});
				bindings.push_back({m_gripForce, Path((prefix + "/input/squeeze/force").c_str())});
				bindings.push_back({m_trackpadForce, Path((prefix + "/input/trackpad/force").c_str())});
			}
		}
		XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggestion.interactionProfile = profile;
		suggestion.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
		suggestion.suggestedBindings = bindings.data();
		const auto result = xrSuggestInteractionProfileBindings(m_runtime.Instance(), &suggestion);
		if (profile != m_simpleProfile && result == XR_ERROR_PATH_UNSUPPORTED)
			continue;
		CheckXr(result, "Suggest physical controller bindings");
	}
	if (trackers)
	{
		m_trackers.emplace(m_runtime);
		m_trackers->Open(session, m_set);
	}
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &m_set;
	CheckXr(xrAttachSessionActionSets(session, &attach), "Attach physical input actions");
	for (std::size_t hand = 0; hand < m_hands.size(); ++hand)
	{
		XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		space.subactionPath = m_hands[hand];
		space.poseInActionSpace.orientation.w = 1;
		space.action = m_grip;
		CheckXr(xrCreateActionSpace(session, &space, &m_gripSpaces[hand]), "Create physical grip space");
		space.action = m_aim;
		CheckXr(xrCreateActionSpace(session, &space, &m_aimSpaces[hand]), "Create physical aim space");
	}
	XrReferenceSpaceCreateInfo head{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	head.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	head.poseInReferenceSpace.orientation.w = 1;
	CheckXr(xrCreateReferenceSpace(session, &head, &m_head), "Create physical head space");
}

HeadsetInputSample HeadsetActions::Sample(XrTime time, XrSpace baseSpace)
{
	HeadsetInputSample sample;
	sample.time = time;
	CheckXr(xrLocateSpace(m_head, baseSpace, time, &sample.head), "Locate physical head");
	XrActiveActionSet active{m_set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	const auto result = xrSyncActions(m_session, &sync);
	CheckXr(result, "Synchronize physical input actions");
	if (m_trackers && m_trackers->Available())
		sample.trackers = m_trackers->Sample(time, baseSpace, result == XR_SUCCESS);
	if (result == XR_SESSION_NOT_FOCUSED)
	{
		m_requireNeutral = true;
		for (std::uint32_t hand = 0; hand < m_hands.size(); ++hand)
			if (m_hapticActive[hand])
				StopHaptic(hand);
		return sample;
	}
	sample.focused = result == XR_SUCCESS;
	for (std::size_t hand = 0; hand < m_hands.size(); ++hand)
	{
		auto& target = sample.hands[hand];
		XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
		CheckXr(xrGetCurrentInteractionProfile(m_session, m_hands[hand], &profile), "Read physical controller profile");
		target.profile = profile.interactionProfile == m_viveProfile	 ? HeadsetControllerProfile::Vive
						 : profile.interactionProfile == m_motionProfile ? HeadsetControllerProfile::Motion
						 : profile.interactionProfile == m_indexProfile	 ? HeadsetControllerProfile::Index
						 : profile.interactionProfile == m_touchProfile	 ? HeadsetControllerProfile::Touch
						 : profile.interactionProfile == m_simpleProfile ? HeadsetControllerProfile::Simple
																		 : HeadsetControllerProfile::Unknown;
		if (profile.interactionProfile != XR_NULL_PATH)
		{
			std::array<char, XR_MAX_PATH_LENGTH> text{};
			std::uint32_t count = 0;
			CheckXr(xrPathToString(m_runtime.Instance(), profile.interactionProfile,
								   static_cast<std::uint32_t>(text.size()), &count, text.data()),
					"Read physical profile path");
			if (count < 2 || count > text.size() || text[count - 1] != '\0')
				throw std::runtime_error("Invalid physical profile path length");
			target.controls.physicalProfile.assign(text.data(), count - 1);
		}
		XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
		get.subactionPath = m_hands[hand];
		get.action = m_select;
		CheckXr(xrGetActionStateBoolean(m_session, &get, &target.select), "Read physical selection");
		get.action = m_trigger;
		CheckXr(xrGetActionStateFloat(m_session, &get, &target.trigger), "Read physical trigger");
		get.action = m_triggerClick;
		XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
		CheckXr(xrGetActionStateBoolean(m_session, &get, &click), "Read physical trigger click");
		if (click.isActive)
			target.controls.triggerClick = click.currentState != XR_FALSE;
		target.controls.connected = target.select.isActive || target.trigger.isActive || click.isActive;
		for (std::size_t index = 0; index < m_buttons.size(); ++index)
		{
			get.action = m_buttons[index];
			XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
			CheckXr(xrGetActionStateBoolean(m_session, &get, &value), "Read physical controller button");
			target.controls.*(s_buttons[index].member) = value.isActive && value.currentState;
			target.controls.connected = target.controls.connected || value.isActive;
		}
		for (const auto& [action, destination] :
			 std::array{std::pair{m_squeeze, &target.controls.grip}, std::pair{m_gripForce, &target.controls.gripForce},
						std::pair{m_trackpadForce, &target.controls.trackpadForce}})
		{
			get.action = action;
			XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
			CheckXr(xrGetActionStateFloat(m_session, &get, &value), "Read physical controller analog input");
			*destination = value.isActive ? value.currentState : 0;
			target.controls.connected = target.controls.connected || value.isActive;
		}
		get.action = m_squeezeClick;
		XrActionStateBoolean squeeze{XR_TYPE_ACTION_STATE_BOOLEAN};
		CheckXr(xrGetActionStateBoolean(m_session, &get, &squeeze), "Read physical squeeze click");
		if (squeeze.isActive)
			target.controls.grip = squeeze.currentState ? 1.0f : 0.0f;
		target.controls.connected = target.controls.connected || squeeze.isActive;
		for (const auto& [action, destination] :
			 std::array{std::pair{m_stick, &target.controls.stick}, std::pair{m_trackpad, &target.controls.trackpad}})
		{
			get.action = action;
			XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
			CheckXr(xrGetActionStateVector2f(m_session, &get, &value), "Read physical controller axis");
			*destination = value.isActive ? value.currentState : XrVector2f{};
			target.controls.connected = target.controls.connected || value.isActive;
		}
		get.action = m_grip;
		XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
		CheckXr(xrGetActionStatePose(m_session, &get, &pose), "Read physical grip activity");
		target.controls.connected = target.controls.connected || pose.isActive;
		if (pose.isActive)
			CheckXr(xrLocateSpace(m_gripSpaces[hand], baseSpace, time, &target.grip), "Locate physical grip");
		get.action = m_aim;
		pose = {XR_TYPE_ACTION_STATE_POSE};
		CheckXr(xrGetActionStatePose(m_session, &get, &pose), "Read physical aim activity");
		target.controls.connected = target.controls.connected || pose.isActive;
		if (pose.isActive)
			CheckXr(xrLocateSpace(m_aimSpaces[hand], baseSpace, time, &target.aim), "Locate physical aim");
	}
	if (m_requireNeutral)
	{
		const bool observed = std::any_of(sample.hands.begin(), sample.hands.end(),
										  [](const auto& hand) { return hand.controls.connected; });
		if (observed && std::all_of(sample.hands.begin(), sample.hands.end(), NeutralHeadsetControls))
			m_requireNeutral = false;
		// Publish a neutral boundary before allowing fresh presses after recovery.
		for (auto& hand : sample.hands)
		{
			auto profile = std::move(hand.controls.physicalProfile);
			const bool connected = hand.controls.connected;
			hand.controls = {};
			hand.controls.physicalProfile = std::move(profile);
			hand.controls.connected = connected;
			hand.select.currentState = XR_FALSE;
			hand.trigger.currentState = 0;
		}
	}
	else
		sample.controlsReady = true;
	return sample;
}

bool HeadsetActions::ApplyHaptic(std::uint32_t hand, float amplitude, float frequency, XrDuration remaining)
{
	if (!m_haptic || hand >= m_hands.size() || !std::isfinite(amplitude) || amplitude < 0 || amplitude > 1 ||
		!std::isfinite(frequency) || frequency < 0 || remaining <= 0)
		throw std::invalid_argument("Invalid physical haptic request");
	if (amplitude == 0)
		return StopHaptic(hand);
	XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = m_haptic;
	info.subactionPath = m_hands[hand];
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
	vibration.amplitude = amplitude;
	vibration.frequency = frequency;
	// Match the existing physical-controller feedback duration ceiling.
	vibration.duration = std::min<XrDuration>(remaining, 2'000'000'000);
	const auto result =
		xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
	CheckXr(result, "Apply physical vibration");
	if (result == XR_SUCCESS)
		m_hapticActive[hand] = true;
	return result == XR_SUCCESS;
}

bool HeadsetActions::StopHaptic(std::uint32_t hand)
{
	if (!m_haptic || hand >= m_hands.size())
		throw std::invalid_argument("Invalid physical haptic hand");
	XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
	info.action = m_haptic;
	info.subactionPath = m_hands[hand];
	const auto result = xrStopHapticFeedback(m_session, &info);
	CheckXr(result, "Stop physical vibration");
	m_hapticActive[hand] = false;
	return result == XR_SUCCESS;
}

XrPath HeadsetActions::Path(const char* text) const
{
	XrPath path = XR_NULL_PATH;
	CheckXr(xrStringToPath(m_runtime.Instance(), text, &path), "Create physical input path");
	return path;
}

XrAction HeadsetActions::CreateAction(const char* name, XrActionType type)
{
	XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
	std::strcpy(info.actionName, name);
	std::strcpy(info.localizedActionName, name);
	info.actionType = type;
	info.countSubactionPaths = m_hands.size();
	info.subactionPaths = m_hands.data();
	XrAction action = XR_NULL_HANDLE;
	CheckXr(xrCreateAction(m_set, &info, &action), "Create physical input action");
	return action;
}
} // namespace simxr::headset
