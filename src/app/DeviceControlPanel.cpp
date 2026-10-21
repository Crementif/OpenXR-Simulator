#include "app/DeviceControlPanel.h"
#include "Math.h"
#include <algorithm>
#include <cmath>
#include <numbers>

#include <cstdio>
#include <exception>
#include <imgui.h>
#include <utility>

namespace simxr::app
{
DeviceControlPanel::DeviceControlPanel() = default;
std::optional<SimulationEdit> DeviceControlPanel::Draw(bool supported)
{
	m_edit.reset();
	if (!supported)
		ImGui::TextWrapped("Connect a running XR application to edit devices.");
	if (supported && !m_observed)
		ImGui::TextWrapped("Configuration unavailable: %s",
						   m_observationError.empty() ? "waiting for runtime values" : m_observationError.c_str());
	supported = supported && m_observed.has_value() && m_configurationLoaded;
	ImGui::PushItemWidth(-1);
	if (ImGui::BeginTabBar("Devices"))
	{
		if (ImGui::BeginTabItem("Head"))
		{
			ImGui::BeginDisabled(!supported);
			m_pilotChannel = ControlChannel::Head;
			ImGui::BeginChild("Head details");
			ImGui::PushItemWidth(-1);
			DrawHead();
			DrawDetailFooter();
			ImGui::PopItemWidth();
			ImGui::EndChild();
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}
		for (int hand = 0; hand < 2; ++hand)
		{
			if (ImGui::BeginTabItem(hand ? "Right hand" : "Left hand"))
			{
				m_controlHand = hand;
				m_pilotChannel = hand ? ControlChannel::RightPose : ControlChannel::LeftPose;
				ImGui::PushID(hand);
				ImGui::BeginDisabled(!supported);
				ImGui::BeginChild("Hand details");
				ImGui::PushItemWidth(-1);
				DrawController();
				DrawDetailFooter();
				ImGui::PopItemWidth();
				ImGui::EndChild();
				ImGui::EndDisabled();
				ImGui::PopID();
				ImGui::EndTabItem();
			}
		}
		if (ImGui::BeginTabItem("Setup"))
		{
			ImGui::TextUnformatted("Configure");
			ImGui::Combo("##Setup section", &m_setupSection, "Headset & views\0Body\0Eye gaze\0Trackers\0Scenarios\0");
			ImGui::BeginDisabled(!supported);
			ImGui::BeginChild("Configuration details");
			ImGui::PushItemWidth(-1);
			switch (m_setupSection)
			{
			case 0:
				DrawViews();
				break;
			case 1:
				DrawBody();
				break;
			case 2:
				DrawGaze();
				break;
			case 3:
				DrawTrackers();
				break;
			case 4:
				DrawScenarios();
				break;
			}
			DrawDetailFooter();
			ImGui::PopItemWidth();
			ImGui::EndChild();
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::PopItemWidth();
	return std::exchange(m_edit, {});
}
void DeviceControlPanel::DrawDetailFooter()
{
	ImGui::Spacing();
	if (ImGui::Button("Reload current values", {-1, 0}))
		LoadObservedSettings();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Replaces all device and setup drafts with the latest observed runtime values.\nDoes not "
						  "send any commands.");
	if (m_configurationLoaded)
		ImGui::TextWrapped("Values loaded from frame %llu", static_cast<unsigned long long>(m_loadedFrame));
	if (ImGui::CollapsingHeader("Editing and ownership"))
	{
		ImGui::Checkbox("Apply edits automatically", &m_autoApply);
		ImGui::TextWrapped("Sliders apply when released; typed values when confirmed. Turn off automatic edits to "
						   "prepare values before applying.");
		DrawOwnership();
	}
}
void DeviceControlPanel::Apply(std::vector<SimulationCommand> commands)
{
	ManualTransaction transaction;
	if (m_applyFrame)
		transaction.frame = FrameId{m_applyFrame};
	transaction.latePolicy = m_reschedule ? CommandLatePolicy::RescheduleNext : CommandLatePolicy::Reject;
	transaction.commands = std::move(commands);
	m_edit = std::move(transaction);
}
void DeviceControlPanel::ApplyOwned(ControlChannel channel, SimulationCommand command)
{
	Apply({TakeControl{channel, m_replaceOwner}, std::move(command)});
}
void DeviceControlPanel::DrawOwnership()
{
	ImGui::TextWrapped("Apply frame (0 = next observed)");
	ImGui::SetNextItemWidth(180);
	ImGui::InputScalar("##Apply frame (0 = next observed)", ImGuiDataType_U64, &m_applyFrame);
	ImGui::Checkbox("Reschedule late edits", &m_reschedule);
	static constexpr const char* channels[] = {"Head",			 "Left pose",	"Right pose", "Left controls",
											   "Right controls", "Body",		"Gaze",		  "Trackers",
											   "Views",			 "Proportions", "Profiles",	  "Session scenarios"};
	ImGui::TextWrapped("Input ownership");
	ImGui::Combo("##Input ownership", &m_selectedChannel, channels, IM_ARRAYSIZE(channels));
	DrawOwner(static_cast<ControlChannel>(m_selectedChannel));
	ImGui::Checkbox("Allow takeover on Apply", &m_replaceOwner);
	if (ImGui::Button("Take control"))
		Apply({TakeControl{static_cast<ControlChannel>(m_selectedChannel), m_replaceOwner}});
	ImGui::SameLine();
	if (ImGui::Button("Release control"))
		Apply({ReleaseControl{static_cast<ControlChannel>(m_selectedChannel)}});
	if (ImGui::Button("Release all manual inputs"))
		m_edit = ReleaseManualInputs{};
	ImGui::TextWrapped("Apply claims the matching input channel. Enable replacement only when you intend to take over "
					   "a script or another source.");
}
void DeviceControlPanel::ClearHeldInputs()
{
	m_authoredControls = {};
}
void DeviceControlPanel::ResetObservations()
{
	m_observed.reset();
	m_observationError.clear();
	m_configurationLoaded = false;
	m_headAvailable = false;
	m_applyFrame = 0;
	m_replaceOwner = false;
	m_edit.reset();
}
void DeviceControlPanel::LoadObservedSettings()
{
	if (!m_observed)
		return;
	const auto& values = m_observed->values;
	m_authoredTracking = values.tracking;
	m_authoredControls = values.input.controllers;
	m_authoredViews = values.views;
	m_authoredProportions = values.proportions;
	m_bodyPreset = static_cast<int>(values.tracking.body.preset);
	m_trackerRole = static_cast<int>(values.tracking.trackers[static_cast<std::size_t>(m_selectedTracker)].role);
	SetPose(ControlChannel::Head, values.tracking.head);
	for (unsigned hand = 0; hand != 2; ++hand)
	{
		SetPose(hand ? ControlChannel::RightPose : ControlChannel::LeftPose, values.tracking.controllers[hand].pose);
		auto& profile = m_controllerProfiles[hand];
		std::snprintf(profile.data(), profile.size(), "%s", values.input.preferredProfiles[hand].c_str());
	}
	m_loadedFrame = m_observed->frame;
	m_configurationLoaded = true;
}
void DeviceControlPanel::ObserveState(const control::Message& state)
{
	try
	{
		const auto previous = m_observed ? m_observed->scenarios : ScenarioState{};
		m_observed = ReadObservedSetup(state);
		for (std::size_t index = 0; index < previous.size(); ++index)
			if (previous[index] && !m_observed->scenarios[index])
				ReloadRecoveredChannel(ScenarioInputChannel(static_cast<FailureScenario>(index)));
		m_observationError.clear();
		if (!m_configurationLoaded)
			LoadObservedSettings();
	}
	catch (const std::exception& error)
	{
		m_observed.reset();
		m_observationError = error.what();
		m_configurationLoaded = false;
	}
	try
	{
		m_observedHead = ObservedDevicePose(state, ControlChannel::Head);
		m_headAvailable = true;
	}
	catch (const std::exception&)
	{
		m_headAvailable = false;
	}
}
void DeviceControlPanel::ReloadRecoveredChannel(ControlChannel channel)
{
	// Failure observations include their effects. Once restored, refresh only
	// that device so an edit cannot latch the simulated failure into its values.
	const auto& values = m_observed->values;
	switch (channel)
	{
	case ControlChannel::Head:
		SetPose(channel, values.tracking.head);
		break;
	case ControlChannel::LeftPose:
	case ControlChannel::RightPose:
	{
		const auto hand = channel == ControlChannel::LeftPose ? 0 : 1;
		m_authoredTracking.controllers[hand] = values.tracking.controllers[hand];
		m_authoredControls[hand].connected = values.input.controllers[hand].connected;
		SetPose(channel, values.tracking.controllers[hand].pose);
		break;
	}
	case ControlChannel::Body:
		m_authoredTracking.body = values.tracking.body;
		m_bodyPreset = static_cast<int>(values.tracking.body.preset);
		break;
	case ControlChannel::Gaze:
		m_authoredTracking.gaze = values.tracking.gaze;
		break;
	case ControlChannel::Trackers:
		m_authoredTracking.trackers = values.tracking.trackers;
		m_trackerRole = static_cast<int>(values.tracking.trackers[static_cast<std::size_t>(m_selectedTracker)].role);
		break;
	default:
		break;
	}
}
bool DeviceControlPanel::DrawScenarioNotice(ControlChannel channel)
{
	if (m_observed)
		for (std::size_t index = 0; index < m_observed->scenarios.size(); ++index)
			if (m_observed->scenarios[index] && ScenarioInputChannel(static_cast<FailureScenario>(index)) == channel)
			{
				ImGui::TextWrapped("A failure scenario affects this device. Restore it in Setup > Scenarios before "
								   "editing. Restoring reloads this device's values.");
				return true;
			}
	return false;
}
void DeviceControlPanel::DrawOwner(ControlChannel channel)
{
	if (!m_observed)
	{
		ImGui::TextUnformatted("Owner: unknown");
		return;
	}
	const auto& owner = m_observed->owners[static_cast<std::size_t>(channel)];
	if (!owner)
	{
		ImGui::TextUnformatted("Owner: free (claimed on Apply)");
		return;
	}
	const char* name = "Unknown source";
	switch (owner->kind)
	{
	case CommandSourceKind::Manual:
		name = "Manual input";
		break;
	case CommandSourceKind::Mcp:
		name = "MCP client";
		break;
	case CommandSourceKind::Rig:
		name = "Replay / script";
		break;
	}
	ImGui::TextWrapped("Owner: %s #%llu", name, static_cast<unsigned long long>(owner->id));
}
void DeviceControlPanel::SetPose(ControlChannel channel, PoseSample pose)
{
	const auto q = pose.pose.orientation;
	constexpr float degrees = 180 / std::numbers::pi_v<float>;
	const std::array<float, 3> angles{
		std::atan2(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y)) * degrees,
		std::asin(std::clamp(2 * (q.w * q.x - q.y * q.z), -1.0f, 1.0f)) * degrees,
		std::atan2(2 * (q.x * q.y + q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z)) * degrees};
	if (channel == ControlChannel::Head)
	{
		m_authoredTracking.head = pose;
		m_headAngles = angles;
	}
	else
	{
		const auto hand = channel == ControlChannel::LeftPose ? 0 : 1;
		m_authoredTracking.controllers[hand].pose = pose;
		m_controllerAngles[hand] = angles;
	}
}
void DeviceControlPanel::DrawHead()
{
	ImGui::BeginDisabled(DrawScenarioNotice(ControlChannel::Head));
	auto& head = m_authoredTracking.head;
	ImGui::Spacing();
	if (ImGui::Button("Recenter view", {-1, 0}))
		ApplyOwned(ControlChannel::Session, SetScenario{FailureScenario::Recenter, true});
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Makes the current head position and facing direction the new origin.\nThis changes the "
						  "reference space; it does not reset the head pose.");
	DrawHandRest();
	ImGui::SeparatorText("Head pose");
	DrawOwner(ControlChannel::Head);
	ImGui::TextUnformatted("Position / metres (X, Y, Z)");
	ImGui::DragFloat3("##Head position", &head.pose.position.x, 0.01f, 0, 0, "%.2f");
	bool edited = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::TextWrapped("Rotation / degrees (yaw, pitch, roll)");
	if (ImGui::DragFloat3("##Head rotation", m_headAngles.data(), 1, -180, 180, "%.0f"))
	{
		constexpr float radians = std::numbers::pi_v<float> / 180;
		head.pose.orientation =
			EulerAngles(m_headAngles[0] * radians, m_headAngles[1] * radians, m_headAngles[2] * radians);
	}
	edited |= ImGui::IsItemDeactivatedAfterEdit();
	if (ImGui::Button("Apply head") || (m_autoApply && edited))
		ApplyOwned(ControlChannel::Head, SetHeadPose{head});
	ImGui::SameLine();
	if (ImGui::Button("Reset pose"))
	{
		head = TrackingState{}.head;
		m_headAngles = {};
		ApplyOwned(ControlChannel::Head, SetHeadPose{head});
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Restores the default standing head pose in the current reference space.");
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawHandRest()
{
	const bool handsDisconnected =
		m_observed && (m_observed->scenarios[static_cast<std::size_t>(FailureScenario::LeftDisconnected)] ||
					   m_observed->scenarios[static_cast<std::size_t>(FailureScenario::RightDisconnected)]);
	ImGui::BeginDisabled(!m_headAvailable || handsDisconnected);
	if (ImGui::Button("Upright hands", {-1, 0}))
	{
		const auto q = m_observedHead.pose.orientation;
		const auto yaw = std::atan2(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y));
		const XrPosef reference{EulerAngles(yaw, 0, 0), m_observedHead.pose.position};
		std::vector<SimulationCommand> commands;
		for (std::uint32_t hand = 0; hand != 2; ++hand)
		{
			const auto channel = hand ? ControlChannel::RightPose : ControlChannel::LeftPose;
			PoseSample pose{Compose(reference, {EulerAngles(0, std::numbers::pi_v<float> / 2, 0),
												{hand ? 0.24f : -0.24f, -0.55f, -0.30f}}),
							TrackedPoseFlags};
			SetPose(channel, pose);
			commands.push_back(TakeControl{channel, m_replaceOwner});
			commands.push_back(SetControllerPose{hand, {pose, m_authoredTracking.controllers[hand].connected}});
		}
		Apply(std::move(commands));
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(
			"Places both controllers near the waist, slightly forward and pointing upward.\n"
			"Uses the head's current position and facing direction. Precise pose controls remain adjustable.");
}
void DeviceControlPanel::DrawController()
{
	const auto hand = static_cast<std::uint32_t>(m_controlHand);
	ImGui::BeginDisabled(DrawScenarioNotice(hand ? ControlChannel::RightPose : ControlChannel::LeftPose));
	auto& controller = m_authoredTracking.controllers[hand];
	auto& controls = m_authoredControls[hand];
	auto& angles = m_controllerAngles[hand];
	DrawHandRest();
	ImGui::SeparatorText("Buttons and axes");
	DrawOwner(hand ? ControlChannel::RightControls : ControlChannel::LeftControls);
	bool edited = ImGui::Checkbox("Primary", &controls.primary);
	ImGui::SameLine();
	edited |= ImGui::Checkbox("Secondary", &controls.secondary);
	edited |= ImGui::Checkbox("Menu", &controls.menu);
	ImGui::SameLine();
	edited |= ImGui::Checkbox("Stick click", &controls.thumbClick);
	ImGui::TextUnformatted("Trigger");
	ImGui::SliderFloat("##Trigger", &controls.trigger, 0, 1, "%.2f");
	edited |= ImGui::IsItemDeactivatedAfterEdit();
	ImGui::TextUnformatted("Grip");
	ImGui::SliderFloat("##Grip", &controls.grip, 0, 1, "%.2f");
	edited |= ImGui::IsItemDeactivatedAfterEdit();
	ImGui::TextUnformatted("Thumbstick (X, Y)");
	ImGui::SliderFloat2("##Stick", &controls.stick.x, -1, 1, "%.2f");
	edited |= ImGui::IsItemDeactivatedAfterEdit();
	const bool apply = ImGui::Button("Apply inputs");
	ImGui::SameLine();
	const bool clear = ImGui::Button("Clear hand");
	if (clear)
		controls = {};
	if (apply || clear || (m_autoApply && edited))
	{
		controls.connected = controller.connected;
		ApplyOwned(hand ? ControlChannel::RightControls : ControlChannel::LeftControls,
				   SetControllerControls{hand, controls});
	}
	if (ImGui::CollapsingHeader("Controller pose"))
	{
		ImGui::TextUnformatted("Position / metres (X, Y, Z)");
		ImGui::DragFloat3("##Controller position", &controller.pose.pose.position.x, 0.01f, 0, 0, "%.2f");
		bool poseEdited = ImGui::IsItemDeactivatedAfterEdit();
		ImGui::TextWrapped("Rotation / degrees (yaw, pitch, roll)");
		if (ImGui::DragFloat3("##Controller rotation", angles.data(), 1, -180, 180, "%.0f"))
		{
			constexpr float radians = std::numbers::pi_v<float> / 180;
			controller.pose.pose.orientation =
				EulerAngles(angles[0] * radians, angles[1] * radians, angles[2] * radians);
		}
		poseEdited |= ImGui::IsItemDeactivatedAfterEdit();
		if (ImGui::Button("Apply pose") || (m_autoApply && poseEdited))
			ApplyOwned(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
					   SetControllerPose{hand, controller});
	}
	if (ImGui::CollapsingHeader("Connection and profile"))
	{
		if (ImGui::Checkbox("Connected", &controller.connected) && m_autoApply)
			ApplyOwned(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
					   SetControllerPose{hand, controller});
		if (!m_autoApply && ImGui::Button("Apply connection"))
			ApplyOwned(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
					   SetControllerPose{hand, controller});
		ImGui::TextWrapped("Interaction profile path");
		auto& profile = m_controllerProfiles[hand];
		ImGui::InputText("##Interaction profile", profile.data(), profile.size());
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", profile.data());
		if (ImGui::Button("Apply profile"))
			ApplyOwned(ControlChannel::Profiles, SetControllerProfile{hand, profile.data()});
	}
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawBody()
{
	ImGui::SeparatorText("Body");
	DrawOwner(ControlChannel::Body);
	if (m_observed && !m_observed->bodyOverridesAvailable)
		ImGui::TextWrapped("This runtime does not expose body overrides. Update the runtime to edit body pose "
						   "without losing joint settings.");
	const bool scenario = DrawScenarioNotice(ControlChannel::Body);
	ImGui::BeginDisabled(scenario || !m_observed || !m_observed->bodyOverridesAvailable);
	auto& body = m_authoredTracking.body;
	ImGui::Checkbox("Body active", &body.active);
	ImGui::TextWrapped("Confidence");
	ImGui::SliderFloat("##Confidence", &body.confidence, 0, 1);
	ImGui::TextWrapped("Pose preset");
	ImGui::Combo("##Pose preset", &m_bodyPreset, "Standing\0T pose\0Seated\0");
	body.preset = static_cast<BodyPreset>(m_bodyPreset);
	if (ImGui::Button("Apply body pose"))
		ApplyOwned(ControlChannel::Body, SetBodyControls{body});
	ImGui::EndDisabled();
	ImGui::SeparatorText("Body proportions");
	DrawOwner(ControlChannel::Proportions);
	ImGui::TextWrapped("Height / metres");
	ImGui::SliderFloat("##Height", &m_authoredProportions.height, 0.5f, 2.5f);
	ImGui::TextWrapped("Shoulder width / metres");
	ImGui::SliderFloat("##Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f);
	ImGui::TextWrapped("Hip width / metres");
	ImGui::SliderFloat("##Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f);
	if (ImGui::Button("Apply proportions"))
		ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
}
void DeviceControlPanel::DrawGaze()
{
	ImGui::SeparatorText("Eye gaze");
	DrawOwner(ControlChannel::Gaze);
	ImGui::BeginDisabled(DrawScenarioNotice(ControlChannel::Gaze));
	auto& gaze = m_authoredTracking.gaze;
	ImGui::TextWrapped("Yaw");
	ImGui::SliderAngle("##Yaw", &gaze.yaw, -90, 90);
	ImGui::TextWrapped("Pitch");
	ImGui::SliderAngle("##Pitch", &gaze.pitch, -90, 90);
	ImGui::Checkbox("Gaze available", &gaze.available);
	ImGui::Checkbox("Gaze tracked", &gaze.tracked);
	if (ImGui::Button("Apply gaze"))
		ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawTrackers()
{
	ImGui::SeparatorText("Trackers");
	if (m_observed && !m_observed->trackerAttachmentsAvailable)
		ImGui::TextWrapped("This runtime does not expose tracker attachments. Update the runtime to edit trackers "
						   "without overwriting those settings.");
	const bool scenario = DrawScenarioNotice(ControlChannel::Trackers);
	ImGui::BeginDisabled(scenario || !m_observed || !m_observed->trackerAttachmentsAvailable);
	DrawOwner(ControlChannel::Trackers);
	ImGui::TextWrapped("Tracker slot");
	const bool selected =
		ImGui::Combo("##Tracker slot", &m_selectedTracker, "Tracker 1\0Tracker 2\0Tracker 3\0Tracker 4\0");
	auto& tracker = m_authoredTracking.trackers[static_cast<std::size_t>(m_selectedTracker)];
	if (selected)
		m_trackerRole = static_cast<int>(tracker.role);
	static constexpr const char* roles[] = {
		"Waist",		 "Chest",		   "Left foot",	 "Right foot",	"Unassigned", "Handheld object",
		"Left shoulder", "Right shoulder", "Left elbow", "Right elbow", "Left knee",  "Right knee",
		"Left wrist",	 "Right wrist",	   "Left ankle", "Right ankle", "Camera",	  "Keyboard"};
	ImGui::TextWrapped("Role");
	ImGui::Combo("##Role", &m_trackerRole, roles, IM_ARRAYSIZE(roles));
	tracker.role = static_cast<TrackerRole>(m_trackerRole);
	ImGui::Checkbox("Tracker connected", &tracker.connected);
	ImGui::Checkbox("Tracker tracked", &tracker.tracked);
	ImGui::Text("Persistent identity: %llu", static_cast<unsigned long long>(tracker.identity.value));
	bool duplicateRole = false;
	for (std::size_t index = 0; index < m_authoredTracking.trackers.size(); ++index)
	{
		const auto& other = m_authoredTracking.trackers[index];
		if (index != static_cast<std::size_t>(m_selectedTracker) && tracker.connected && other.connected &&
			tracker.role != TrackerRole::Unassigned && other.role == tracker.role)
		{
			ImGui::TextWrapped("This role is already assigned to tracker %u. Choose another role or disconnect "
							   "that tracker first.",
							   static_cast<unsigned>(index + 1));
			duplicateRole = true;
		}
	}
	ImGui::BeginDisabled(duplicateRole);
	if (ImGui::Button("Apply tracker"))
	{
		ApplyOwned(ControlChannel::Trackers, SetTracker{static_cast<std::uint32_t>(m_selectedTracker), tracker});
	}
	ImGui::EndDisabled();
	ImGui::TextWrapped("Existing attachment and pose overrides are preserved.");
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawViews()
{
	ImGui::SeparatorText("View settings");
	DrawOwner(ControlChannel::Views);
	ImGui::TextWrapped("Eye separation (IPD)");
	ImGui::SliderFloat("##IPD", &m_authoredViews.ipd, 0.04f, 0.09f, "%.3f m");
	ImGui::TextWrapped("Inset half angle (quad views)");
	float maximum = std::numbers::pi_v<float> / 2;
	for (const auto& fov : m_authoredViews.outerFov)
		maximum = std::min({maximum, (fov.angleRight - fov.angleLeft) / 2, (fov.angleUp - fov.angleDown) / 2});
	ImGui::SliderAngle("##Inset half angle", &m_authoredViews.insetHalfAngle, 0.1f,
					   maximum * 180 / std::numbers::pi_v<float>);
	if (ImGui::Button("Apply view settings"))
		ApplyOwned(ControlChannel::Views, SetViewSettings{m_authoredViews});
	ImGui::TextWrapped("View size: %u x %u; inset: %u x %u", m_authoredViews.outerWidth, m_authoredViews.outerHeight,
					   m_authoredViews.insetWidth, m_authoredViews.insetHeight);
	ImGui::TextWrapped("Changes affect subsequent frames. View sizes and outer fields of view are preserved.");
}
void DeviceControlPanel::DrawScenarios()
{
	ImGui::SeparatorText("Failure scenarios");
	static constexpr const char* scenarios[] = {"Head untracked",
												"Head unavailable",
												"Body untracked",
												"Body unavailable",
												"Gaze untracked",
												"Gaze unavailable",
												"Left controller disconnected",
												"Right controller disconnected",
												"Trackers disconnected",
												"Unfocused",
												"Hidden",
												"Recenter",
												"Session loss"};
	ImGui::TextWrapped("Scenario");
	ImGui::Combo("##Scenario", &m_selectedScenario, scenarios, IM_ARRAYSIZE(scenarios));
	const auto scenario = static_cast<FailureScenario>(m_selectedScenario);
	const auto channel = ScenarioInputChannel(scenario);
	DrawOwner(channel);
	if (scenario == FailureScenario::Recenter)
	{
		ImGui::TextWrapped("Moves the reference-space origin to the current head position and facing direction.");
		if (ImGui::Button("Recenter now", {-1, 0}))
			ApplyOwned(channel, SetScenario{scenario, true});
	}
	else if (scenario == FailureScenario::SessionLoss)
	{
		ImGui::TextWrapped(
			"Requests session loss. The application must recreate its XR session or be relaunched to recover.");
		if (ImGui::Button("Request session loss", {-1, 0}))
			ApplyOwned(channel, SetScenario{scenario, true});
	}
	else
	{
		const bool active = m_observed && m_observed->scenarios[static_cast<std::size_t>(m_selectedScenario)];
		ImGui::TextUnformatted(active ? "Applied state: active" : "Applied state: inactive");
		if (ImGui::Button(active ? "Restore normal behavior" : "Activate scenario", {-1, 0}))
			ApplyOwned(channel, SetScenario{scenario, !active});
		ImGui::TextWrapped("Restore clears this scenario; other scenarios and device settings stay as configured.");
	}
}
} // namespace simxr::app
