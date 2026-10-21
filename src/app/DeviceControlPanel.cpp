#include "app/DeviceControlPanel.h"

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
		ImGui::TextWrapped("Connect to a runtime with simulation command support to edit devices.");
	ImGui::BeginDisabled(!supported);
	ImGui::TextWrapped("Changes are scheduled before a frame's tracking publication.");
	DrawOwnership();
	DrawHead();
	DrawController();
	DrawBody();
	DrawGaze();
	DrawTrackers();
	DrawViews();
	DrawScenarios();

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
	ImGui::TextWrapped("Take the matching channel before applying values. Ownership conflicts are reported; "
					   "applying a value never silently takes over a rig.");
}
void DeviceControlPanel::DrawHead()
{
	if (ImGui::CollapsingHeader("Head pose"))
	{
		auto& head = m_authoredTracking.head;
		ImGui::DragFloat3("Head position", &head.pose.position.x, 0.01f);
		ImGui::DragFloat4("Head orientation (xyzw)", &head.pose.orientation.x, 0.01f, -1, 1);
		if (ImGui::Button("Apply head pose"))
			Apply({SetHeadPose{head}});
	}
}
void DeviceControlPanel::DrawController()
{
	if (ImGui::CollapsingHeader("Controller"))
	{
		ImGui::Combo("Hand", &m_controlHand, "Left\0Right\0");
		auto& controller = m_authoredTracking.controllers[static_cast<std::size_t>(m_controlHand)];
		ImGui::Checkbox("Controller connected", &controller.connected);
		ImGui::DragFloat3("Controller position", &controller.pose.pose.position.x, 0.01f);
		ImGui::DragFloat4("Controller orientation", &controller.pose.pose.orientation.x, 0.01f, -1, 1);
		if (ImGui::Button("Apply controller pose"))
			Apply({SetControllerPose{static_cast<std::uint32_t>(m_controlHand), controller}});
		ImGui::InputText("Interaction profile", m_controllerProfile.data(), m_controllerProfile.size());
		if (ImGui::Button("Apply interaction profile"))
			Apply({SetControllerProfile{static_cast<std::uint32_t>(m_controlHand), m_controllerProfile.data()}});
		ImGui::SliderFloat("Trigger", &m_authoredControls.trigger, 0, 1);
		ImGui::SliderFloat("Grip", &m_authoredControls.grip, 0, 1);
		ImGui::SliderFloat2("Stick", &m_authoredControls.stick.x, -1, 1);
		ImGui::Checkbox("Primary", &m_authoredControls.primary);
		ImGui::SameLine();
		ImGui::Checkbox("Secondary", &m_authoredControls.secondary);
		ImGui::Checkbox("Menu", &m_authoredControls.menu);
		ImGui::SameLine();
		ImGui::Checkbox("Thumb click", &m_authoredControls.thumbClick);
		if (ImGui::Button("Apply controller controls"))
		{
			m_authoredControls.connected = controller.connected;
			Apply({SetControllerControls{static_cast<std::uint32_t>(m_controlHand), m_authoredControls}});
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
			Apply({SetBodyControls{body}});
		ImGui::SliderFloat("Height", &m_authoredProportions.height, 0.5f, 2.5f);
		ImGui::SliderFloat("Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f);
		ImGui::SliderFloat("Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f);
		if (ImGui::Button("Apply proportions"))
			Apply({SetBodyProportions{m_authoredProportions}});
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
			Apply({SetGaze{gaze}});
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
			Apply({SetTracker{static_cast<std::uint32_t>(m_selectedTracker), tracker}});
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
			Apply({SetViewSettings{m_authoredViews}});
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
