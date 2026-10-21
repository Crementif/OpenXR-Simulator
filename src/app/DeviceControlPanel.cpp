#include "app/DeviceControlPanel.h"
#include "Math.h"
#include <numbers>

#include <cstdio>
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
	ImGui::BeginDisabled(!supported);
	ImGui::PushItemWidth(-1);
	ImGui::TextUnformatted("Edit, then apply.");
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(
			"Manual inputs release when this window loses focus.\nScripts keep control unless explicitly overridden.");
	DrawHead();
	DrawController();
	DrawBody();
	DrawGaze();
	DrawTrackers();
	DrawViews();
	DrawScenarios();
	if (ImGui::CollapsingHeader("Advanced ownership and scheduling"))
		DrawOwnership();
	if (ImGui::Button("Release my inputs"))
		m_edit = ReleaseManualInputs{};
	ImGui::PopItemWidth();

	ImGui::EndDisabled();
	return std::exchange(m_edit, {});
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
	ImGui::SetNextItemWidth(180);
	ImGui::InputScalar("Apply frame (0 = next observed)", ImGuiDataType_U64, &m_applyFrame);
	ImGui::Checkbox("Reschedule explicitly if the frame is already past", &m_reschedule);
	static constexpr const char* channels[] = {"Head",			 "Left pose",	"Right pose", "Left controls",
											   "Right controls", "Body",		"Gaze",		  "Trackers",
											   "Views",			 "Proportions", "Profiles",	  "Session scenarios"};
	ImGui::Combo("Input ownership", &m_selectedChannel, channels, IM_ARRAYSIZE(channels));
	ImGui::Checkbox("Replace another source's ownership", &m_replaceOwner);
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
void DeviceControlPanel::DrawHead()
{
	if (ImGui::CollapsingHeader("Head", ImGuiTreeNodeFlags_DefaultOpen))
	{
		auto& head = m_authoredTracking.head;
		ImGui::TextUnformatted("Position / metres (X, Y, Z)");
		ImGui::DragFloat3("##Head position", &head.pose.position.x, 0.01f, 0, 0, "%.2f");
		ImGui::TextWrapped("Rotation / degrees (yaw, pitch, roll)");
		if (ImGui::DragFloat3("##Head rotation", m_headAngles.data(), 1, -180, 180, "%.0f"))
		{
			constexpr float radians = std::numbers::pi_v<float> / 180;
			head.pose.orientation =
				EulerAngles(m_headAngles[0] * radians, m_headAngles[1] * radians, m_headAngles[2] * radians);
		}
		if (ImGui::Button("Apply head"))
			ApplyOwned(ControlChannel::Head, SetHeadPose{head});
		ImGui::SameLine();
		if (ImGui::Button("Reset head"))
		{
			head = TrackingState{}.head;
			m_headAngles = {};
			ApplyOwned(ControlChannel::Head, SetHeadPose{head});
		}
	}
}
void DeviceControlPanel::DrawController()
{
	if (ImGui::CollapsingHeader("Controllers", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::RadioButton("Left", &m_controlHand, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Right", &m_controlHand, 1);
		const auto hand = static_cast<std::uint32_t>(m_controlHand);
		auto& controller = m_authoredTracking.controllers[hand];
		auto& controls = m_authoredControls[hand];
		auto& angles = m_controllerAngles[hand];
		ImGui::TextUnformatted("Position / metres (X, Y, Z)");
		ImGui::DragFloat3("##Controller position", &controller.pose.pose.position.x, 0.01f, 0, 0, "%.2f");
		ImGui::TextWrapped("Rotation / degrees (yaw, pitch, roll)");
		if (ImGui::DragFloat3("##Controller rotation", angles.data(), 1, -180, 180, "%.0f"))
		{
			constexpr float radians = std::numbers::pi_v<float> / 180;
			controller.pose.pose.orientation =
				EulerAngles(angles[0] * radians, angles[1] * radians, angles[2] * radians);
		}
		if (ImGui::Button("Apply controller pose"))
			ApplyOwned(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
					   SetControllerPose{hand, controller});
		ImGui::TextUnformatted("Trigger");
		ImGui::SliderFloat("##Trigger", &controls.trigger, 0, 1);
		ImGui::TextUnformatted("Grip");
		ImGui::SliderFloat("##Grip", &controls.grip, 0, 1);
		ImGui::TextUnformatted("Thumbstick (X, Y)");
		ImGui::SliderFloat2("##Stick", &controls.stick.x, -1, 1);
		ImGui::Checkbox("Primary", &controls.primary);
		ImGui::SameLine();
		ImGui::Checkbox("Secondary", &controls.secondary);
		ImGui::Checkbox("Menu", &controls.menu);
		ImGui::SameLine();
		ImGui::Checkbox("Stick click", &controls.thumbClick);
		if (ImGui::Button("Apply buttons and axes"))
		{
			controls.connected = controller.connected;
			ApplyOwned(hand ? ControlChannel::RightControls : ControlChannel::LeftControls,
					   SetControllerControls{hand, controls});
		}
		if (ImGui::TreeNode("Controller connection and profile"))
		{
			ImGui::Checkbox("Connected", &controller.connected);
			ImGui::TextWrapped("Apply controller pose to update connection state.");
			ImGui::InputText("##Interaction profile", m_controllerProfile.data(), m_controllerProfile.size());
			if (ImGui::Button("Apply profile"))
				ApplyOwned(ControlChannel::Profiles, SetControllerProfile{hand, m_controllerProfile.data()});
			ImGui::TreePop();
		}
	}
}
void DeviceControlPanel::DrawBody()
{
	if (ImGui::CollapsingHeader("Body"))
	{
		auto& body = m_authoredTracking.body;
		ImGui::Checkbox("Body active", &body.active);
		ImGui::SliderFloat("Confidence", &body.confidence, 0, 1);
		ImGui::Combo("Pose preset", &m_bodyPreset, "Standing\0T pose\0Seated\0");
		body.preset = static_cast<BodyPreset>(m_bodyPreset);
		if (ImGui::Button("Apply body pose"))
			ApplyOwned(ControlChannel::Body, SetBodyControls{body});
		ImGui::SliderFloat("Height", &m_authoredProportions.height, 0.5f, 2.5f);
		ImGui::SliderFloat("Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f);
		ImGui::SliderFloat("Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f);
		if (ImGui::Button("Apply proportions"))
			ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
	}
}
void DeviceControlPanel::DrawGaze()
{
	if (ImGui::CollapsingHeader("Eye gaze"))
	{
		auto& gaze = m_authoredTracking.gaze;
		ImGui::SliderAngle("Yaw", &gaze.yaw, -90, 90);
		ImGui::SliderAngle("Pitch", &gaze.pitch, -90, 90);
		ImGui::Checkbox("Gaze available", &gaze.available);
		ImGui::SameLine();
		ImGui::Checkbox("Gaze tracked", &gaze.tracked);
		if (ImGui::Button("Apply gaze"))
			ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	}
}
void DeviceControlPanel::DrawTrackers()
{
	if (ImGui::CollapsingHeader("Trackers"))
	{
		ImGui::SliderInt("Tracker slot", &m_selectedTracker, 0, 3);
		auto& tracker = m_authoredTracking.trackers[static_cast<std::size_t>(m_selectedTracker)];
		if (ImGui::IsItemEdited())
			m_trackerRole = static_cast<int>(tracker.role);
		static constexpr const char* roles[] = {
			"Waist",		 "Chest",		   "Left foot",	 "Right foot",	"Unassigned", "Handheld object",
			"Left shoulder", "Right shoulder", "Left elbow", "Right elbow", "Left knee",  "Right knee",
			"Left wrist",	 "Right wrist",	   "Left ankle", "Right ankle", "Camera",	  "Keyboard"};
		ImGui::Combo("Role", &m_trackerRole, roles, IM_ARRAYSIZE(roles));
		tracker.role = static_cast<TrackerRole>(m_trackerRole);
		ImGui::Checkbox("Tracker connected", &tracker.connected);
		ImGui::SameLine();
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
		ImGui::SliderFloat("IPD", &m_authoredViews.ipd, 0.04f, 0.09f, "%.3f m");
		ImGui::SliderAngle("Inset half angle", &m_authoredViews.insetHalfAngle, 5, 60);
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
		ImGui::Combo("Scenario", &m_selectedScenario, scenarios, IM_ARRAYSIZE(scenarios));
		ImGui::Checkbox("Scenario enabled", &m_scenarioEnabled);
		if (ImGui::Button("Apply scenario"))
			Apply({SetScenario{static_cast<FailureScenario>(m_selectedScenario), m_scenarioEnabled}});
		ImGui::TextWrapped("Scenarios are opt-in and require the affected channel's ownership. Disable a scenario "
						   "explicitly to recover.");
	}
}
} // namespace simxr::app
