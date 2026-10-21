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
DeviceControlPanel::DeviceControlPanel()
{
	std::snprintf(m_controllerProfile.data(), m_controllerProfile.size(), "%s",
				  "/interaction_profiles/oculus/touch_controller");
}
std::optional<SimulationEdit> DeviceControlPanel::Draw(bool supported)
{
	m_edit.reset();
	if (!supported)
		ImGui::TextWrapped("Connect a running XR application to edit devices.");
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
		if (ImGui::BeginTabItem("More"))
		{
			ImGui::BeginDisabled(!supported);
			ImGui::BeginChild("Configuration details");
			ImGui::PushItemWidth(-1);
			DrawBody();
			DrawGaze();
			DrawTrackers();
			DrawViews();
			DrawScenarios();
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
	ImGui::Checkbox("Replace current owner", &m_replaceOwner);
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
void DeviceControlPanel::ObservePoses(const control::Message& state)
{
	for (unsigned index = 0; index < m_poseObserved.size(); ++index)
	{
		if (m_poseObserved[index])
			continue;
		try
		{
			const auto channel = static_cast<ControlChannel>(index);
			SetPose(channel, ObservedDevicePose(state, channel));
			m_poseObserved[index] = true;
		}
		catch (const std::exception&)
		{
			// A controller or its first valid publication may not be available yet.
		}
	}
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
	auto& head = m_authoredTracking.head;
	ImGui::Spacing();
	if (ImGui::Button("Recenter view", {-1, 0}))
		ApplyOwned(ControlChannel::Session, SetScenario{FailureScenario::Recenter, true});
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Makes the current head position and facing direction the new origin.\nThis changes the "
						  "reference space; it does not reset the head pose.");
	ImGui::SeparatorText("Head pose");
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
}
void DeviceControlPanel::DrawController()
{
	const auto hand = static_cast<std::uint32_t>(m_controlHand);
	auto& controller = m_authoredTracking.controllers[hand];
	auto& controls = m_authoredControls[hand];
	auto& angles = m_controllerAngles[hand];
	ImGui::SeparatorText("Buttons and axes");
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
		ImGui::InputText("##Interaction profile", m_controllerProfile.data(), m_controllerProfile.size());
		if (ImGui::Button("Apply profile"))
			ApplyOwned(ControlChannel::Profiles, SetControllerProfile{hand, m_controllerProfile.data()});
	}
}
void DeviceControlPanel::DrawBody()
{
	if (ImGui::CollapsingHeader("Body"))
	{
		auto& body = m_authoredTracking.body;
		ImGui::Checkbox("Body active", &body.active);
		ImGui::TextWrapped("Confidence");
		ImGui::SliderFloat("##Confidence", &body.confidence, 0, 1);
		ImGui::TextWrapped("Pose preset");
		ImGui::Combo("##Pose preset", &m_bodyPreset, "Standing\0T pose\0Seated\0");
		body.preset = static_cast<BodyPreset>(m_bodyPreset);
		if (ImGui::Button("Apply body pose"))
			ApplyOwned(ControlChannel::Body, SetBodyControls{body});
		ImGui::TextWrapped("Height");
		ImGui::SliderFloat("##Height", &m_authoredProportions.height, 0.5f, 2.5f);
		ImGui::TextWrapped("Shoulder width");
		ImGui::SliderFloat("##Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f);
		ImGui::TextWrapped("Hip width");
		ImGui::SliderFloat("##Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f);
		if (ImGui::Button("Apply proportions"))
			ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
	}
}
void DeviceControlPanel::DrawGaze()
{
	if (ImGui::CollapsingHeader("Eye gaze"))
	{
		auto& gaze = m_authoredTracking.gaze;
		ImGui::TextWrapped("Yaw");
		ImGui::SliderAngle("##Yaw", &gaze.yaw, -90, 90);
		ImGui::TextWrapped("Pitch");
		ImGui::SliderAngle("##Pitch", &gaze.pitch, -90, 90);
		ImGui::Checkbox("Gaze available", &gaze.available);
		ImGui::Checkbox("Gaze tracked", &gaze.tracked);
		if (ImGui::Button("Apply gaze"))
			ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	}
}
void DeviceControlPanel::DrawTrackers()
{
	if (ImGui::CollapsingHeader("Trackers"))
	{
		ImGui::TextWrapped("Tracker slot");
		ImGui::SliderInt("##Tracker slot", &m_selectedTracker, 0, 3);
		auto& tracker = m_authoredTracking.trackers[static_cast<std::size_t>(m_selectedTracker)];
		if (ImGui::IsItemEdited())
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
		if (ImGui::Button("Apply tracker"))
		{
			tracker.anchor.reset();
			ApplyOwned(ControlChannel::Trackers, SetTracker{static_cast<std::uint32_t>(m_selectedTracker), tracker});
		}
	}
}
void DeviceControlPanel::DrawViews()
{
	if (ImGui::CollapsingHeader("View settings"))
	{
		ImGui::TextWrapped("IPD");
		ImGui::SliderFloat("##IPD", &m_authoredViews.ipd, 0.04f, 0.09f, "%.3f m");
		ImGui::TextWrapped("Inset half angle");
		ImGui::SliderAngle("##Inset half angle", &m_authoredViews.insetHalfAngle, 5, 60);
		if (ImGui::Button("Apply view settings"))
			ApplyOwned(ControlChannel::Views, SetViewSettings{m_authoredViews});
		ImGui::TextWrapped("Applied view settings are used by future frame publications.");
	}
}
void DeviceControlPanel::DrawScenarios()
{
	if (ImGui::CollapsingHeader("Failure scenarios"))
	{
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
		ImGui::Checkbox("Scenario enabled", &m_scenarioEnabled);
		if (ImGui::Button("Apply scenario"))
			Apply({SetScenario{static_cast<FailureScenario>(m_selectedScenario), m_scenarioEnabled}});
		ImGui::TextWrapped("Scenarios are opt-in and require the affected channel's ownership. Disable a scenario "
						   "explicitly to recover.");
	}
}
} // namespace simxr::app
