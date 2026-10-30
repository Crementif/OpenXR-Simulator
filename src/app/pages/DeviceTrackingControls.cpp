#include "app/UiPch.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/services/PlayerRig.h"
#include "app/services/SpatialMath.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include "math/Math.h"
#include "simulation/BodyModel.h"
#include "simulation/HeadsetProfiles.h"

namespace simxr::app
{
void DeviceControlPanel::DrawBody()
{
	SectionLabel("Body");
	DrawOwner(ControlChannel::Body);
	if (m_observed && !m_observed->bodyOverridesAvailable)
		ImGui::TextUnformatted("Body unavailable");
	const bool scenario = ScenarioActive(ControlChannel::Body);
	ImGui::BeginDisabled(scenario || !CanControl(ControlChannel::Body) || !m_observed->bodyOverridesAvailable);
	DrawBodyPoses();
	auto& body = m_authoredTracking.body;
	bool edited =
		Field("Body active", ImGui::Checkbox("Body active", &body.active), false, SwitchValue(body.active, true));
	FieldLabel("Confidence");
	edited |= Field("##Confidence", ImGui::SliderFloat("##Confidence", &body.confidence, 0, 1), true,
					NumberValue(body.confidence, 1, .05f, 0, 1));
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Body, SetBodyControls{body});
	ImGui::EndDisabled();
	SectionLabel("Body proportions");
	DrawOwner(ControlChannel::Proportions);
	ImGui::BeginDisabled(!CanControl(ControlChannel::Proportions));
	const BodyProportions defaults;
	FieldLabel("Height");
	bool proportionsEdited =
		Field("##Height", ImGui::SliderFloat("##Height", &m_authoredProportions.height, 0.5f, 2.5f, "%.3f m"), true,
			  NumberValue(m_authoredProportions.height, defaults.height, .01f, .5f, 2.5f));
	FieldLabel("Shoulder width");
	proportionsEdited |=
		Field("##Shoulder width",
			  ImGui::SliderFloat("##Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f, "%.3f m"), true,
			  NumberValue(m_authoredProportions.shoulderWidth, defaults.shoulderWidth, .01f, .1f, .8f));
	FieldLabel("Hip width");
	proportionsEdited |=
		Field("##Hip width", ImGui::SliderFloat("##Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f, "%.3f m"),
			  true, NumberValue(m_authoredProportions.hipWidth, defaults.hipWidth, .01f, .1f, .8f));
	if (proportionsEdited && !m_edit)
		ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
	ImGui::EndDisabled();
}

void DeviceControlPanel::DrawGaze()
{
	SectionLabel("Eye gaze");
	DrawOwner(ControlChannel::Gaze);
	ImGui::BeginDisabled(!CanControl(ControlChannel::Gaze) || ScenarioActive(ControlChannel::Gaze));
	auto& gaze = m_authoredTracking.gaze;
	const GazeInput defaults;
	FieldLabel("Yaw");
	ImGui::PushStyleColor(ImGuiCol_Text, theme::AxisTextY);
	const bool yawEdited = ImGui::SliderAngle("##Yaw", &gaze.yaw, -90, 90, "%.1f°");
	ImGui::PopStyleColor();
	bool edited = Field("##Yaw", yawEdited, true, AngleValue(gaze.yaw, defaults.yaw, -90, 90));
	FieldLabel("Pitch");
	ImGui::PushStyleColor(ImGuiCol_Text, theme::AxisTextX);
	const bool pitchEdited = ImGui::SliderAngle("##Pitch", &gaze.pitch, -90, 90, "%.1f°");
	ImGui::PopStyleColor();
	edited |= Field("##Pitch", pitchEdited, true, AngleValue(gaze.pitch, defaults.pitch, -90, 90));
	edited |= Field("Gaze available", ImGui::Checkbox("Gaze available", &gaze.available), false,
					SwitchValue(gaze.available, defaults.available));
	edited |= Field("Gaze tracked", ImGui::Checkbox("Gaze tracked", &gaze.tracked), false,
					SwitchValue(gaze.tracked, defaults.tracked));
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	ImGui::EndDisabled();
}

void DeviceControlPanel::DrawTrackers()
{
	if (m_observed && !m_observed->trackerAttachmentsAvailable)
		ImGui::TextUnformatted("Trackers unavailable");
	DrawOwner(ControlChannel::Trackers);
	if (m_authoredTracking.trackers.empty())
	{
		ImGui::TextDisabled("No trackers");
		return;
	}
	m_selectedTracker = std::clamp(m_selectedTracker, 0, static_cast<int>(m_authoredTracking.trackers.size() - 1));
	if (m_authoredTracking.trackers.size() > 8)
	{
		const auto name = [&](std::size_t index) {
			const auto& device = m_authoredTracking.trackers[index];
			return std::string(TrackerRoleName(device.role)) + " #" + std::to_string(device.identity.value);
		};
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo("##Tracker device", name(m_selectedTracker).c_str()))
		{
			for (std::size_t index = 0; index < m_authoredTracking.trackers.size(); ++index)
				if (ImGui::Selectable(name(index).c_str(), index == static_cast<std::size_t>(m_selectedTracker)))
					SelectDevice(DeviceSel{DevicePage::Trackers, static_cast<unsigned>(index)});
			ImGui::EndCombo();
		}
	}
	else
	{
		ImGui::PushID("Tracker chips");
		const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		for (int slot = 0; slot < static_cast<int>(m_authoredTracking.trackers.size()); ++slot)
		{
			const auto& device = m_authoredTracking.trackers[static_cast<std::size_t>(slot)];
			const char* name = TrackerRoleName(device.role);
			const float chip = ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2;
			if (slot && ImGui::GetItemRectMax().x + 2 + chip <= right)
				ImGui::SameLine(0, 2);
			const bool current = slot == m_selectedTracker;
			ImGui::PushStyleColor(ImGuiCol_Button, current ? theme::Accent : theme::SurfaceRaised);
			ImGui::PushStyleColor(ImGuiCol_Text, device.connected ? theme::Text : theme::TextDim);
			ImGui::PushID(slot);
			if (ImGui::Button(name) && !current)
			{
				SelectDevice(DeviceSel{DevicePage::Trackers, static_cast<unsigned>(slot)});
			}
			ImGui::PopID();
			ImGui::PopStyleColor(2);
			ImGui::SetItemTooltip("Tracker %d%s", slot + 1, device.connected ? "" : " (disconnected)");
		}
		ImGui::PopID();
	}
	const auto slot = static_cast<std::size_t>(m_selectedTracker);
	auto& tracker = m_authoredTracking.trackers[slot];
	ImGui::BeginDisabled(!CanControl(ControlChannel::Trackers) || ScenarioActive(ControlChannel::Trackers) ||
						 !m_observed->trackerAttachmentsAvailable);
	const auto attachment = m_playerRig.TrackerAt(slot);
	FieldLabel("Attached to");
	const float snapWidth = ImGui::CalcTextSize("Snap to body").x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::SetNextItemWidth(
		std::max(80.0f, ImGui::GetContentRegionAvail().x - snapWidth - ImGui::GetStyle().ItemSpacing.x));
	if (ImGui::BeginCombo("##Attached to", attachment.joint ? BodyJointName(*attachment.joint) : "World"))
	{
		for (const auto joint : AttachableJoints())
			if (ImGui::Selectable(BodyJointName(joint), attachment.joint == joint) && m_playerRig.Ready())
			{
				m_playerRig.AttachTracker(slot, joint);
				ApplyRig();
			}
		if (ImGui::Selectable("World", !attachment.joint) && m_playerRig.Ready())
		{
			m_playerRig.AttachTracker(slot, std::nullopt);
			ApplyRig();
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("Joint offsets follow body animation. World attachments stay fixed.");
	ImGui::SameLine();
	if (ImGui::Button("Snap to body") && m_playerRig.Ready())
	{
		m_playerRig.SnapTracker(slot);
		ApplyRig();
	}
	ImGui::SetItemTooltip("Back on the %s joint, no offset", BodyJointName(BodyModel::TrackerJoint(tracker.role)));
	DrawExactPose(m_selectedTracker + 3);
	static constexpr const char* roles[] = {
		"Waist",		 "Chest",		   "Left foot",	 "Right foot",	"Unassigned", "Handheld object",
		"Left shoulder", "Right shoulder", "Left elbow", "Right elbow", "Left knee",  "Right knee",
		"Left wrist",	 "Right wrist",	   "Left ankle", "Right ankle", "Camera",	  "Keyboard"};
	bool edited = Field("Tracker connected", ImGui::Checkbox("Connected", &tracker.connected), false,
						SwitchValue(tracker.connected, true));
	ImGui::SameLine();
	edited |= Field("Tracker tracked", ImGui::Checkbox("Tracked", &tracker.tracked), false,
					SwitchValue(tracker.tracked, true));
	ImGui::SetItemTooltip("Identity %llu", static_cast<unsigned long long>(tracker.identity.value));
	bool moveToRoleJoint = false;
	if (Section("Role"))
	{
		const auto previousRole = tracker.role;
		int role = static_cast<int>(tracker.role);
		const bool roleChanged = Field("##Role", ImGui::Combo("##Role", &role, roles, IM_ARRAYSIZE(roles)), false);
		const auto path = BodyModel::TrackerRolePath(static_cast<TrackerRole>(role));
		ImGui::SetItemTooltip("The app sees this tracker at %s",
							  path.empty() ? "no role path" : std::string(path).c_str());
		if (roleChanged)
			tracker.role = static_cast<TrackerRole>(role);
		if (roleChanged && attachment.joint == BodyModel::TrackerJoint(previousRole))
		{
			// A tracker on its old role's joint moves with the role.
			moveToRoleJoint = true;
			tracker.anchor = BodyModel::TrackerJoint(tracker.role);
			tracker.poseOverride.reset();
			tracker.jointOffset.reset();
		}
		edited |= roleChanged;
	}
	bool duplicateRole = false;
	for (std::size_t index = 0; index < m_authoredTracking.trackers.size(); ++index)
	{
		const auto& other = m_authoredTracking.trackers[index];
		if (index != slot && tracker.connected && other.connected && tracker.role != TrackerRole::Unassigned &&
			other.role == tracker.role)
		{
			ImGui::TextColored(theme::Vec(theme::Warn), "%s is also tracker %u", TrackerRoleName(other.role),
							   static_cast<unsigned>(index + 1));
			duplicateRole = true;
		}
	}
	if (edited && !duplicateRole && !m_edit)
	{
		if (moveToRoleJoint)
		{
			m_playerRig.AttachTracker(slot, BodyModel::TrackerJoint(tracker.role));
			m_playerRig.SetTrackerOffset(slot, IdentityPose());
		}
		ApplyOwned(ControlChannel::Trackers, SetTracker{static_cast<std::uint32_t>(slot), tracker});
	}
	ImGui::EndDisabled();
}

std::optional<SimulationEdit> DeviceControlPanel::DrawHeadset(bool supported)
{
	m_edit.reset();
	m_continuousEdit = false;
	const bool ready = m_observed && m_configurationLoaded;
	if (ready && !m_pending && !Active())
		LoadObservedSettings();
	// Headset fields keep their pending and rejected state apart from the device pages.
	const auto scope = std::exchange(m_fieldScope, "headset/");
	DrawOwner(ControlChannel::Views);
	ImGui::BeginDisabled(!supported || !ready || !CanControl(ControlChannel::Views));
	const auto close = [](const XrFovf& a, const XrFovf& b) {
		return std::abs(a.angleLeft - b.angleLeft) < 1e-4f && std::abs(a.angleRight - b.angleRight) < 1e-4f &&
			   std::abs(a.angleUp - b.angleUp) < 1e-4f && std::abs(a.angleDown - b.angleDown) < 1e-4f;
	};
	const auto name = [](const HeadsetSpec& profile) {
		std::string label;
		for (const wchar_t* character = profile.menuLabel; *character; ++character)
			if (*character != L'&')
				label += static_cast<char>(*character);
		return label;
	};
	// The model is recognised by its field of view, so a changed IPD keeps it selected.
	const HeadsetSpec* model = nullptr;
	for (const auto& profile : HeadsetProfiles())
	{
		const auto views = ProfileViews(profile);
		if (close(views.outerFov[0], m_authoredViews.outerFov[0]) &&
			close(views.outerFov[1], m_authoredViews.outerFov[1]))
			model = &profile;
	}
	FieldLabel("Model");
	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##Headset model", model ? name(*model).c_str() : "Custom"))
	{
		for (const auto& profile : HeadsetProfiles())
			if (ImGui::Selectable(name(profile).c_str(), &profile == model) && &profile != model)
			{
				const auto views = ProfileViews(profile);
				m_authoredViews.outerFov = views.outerFov;
				m_authoredViews.ipd =
					std::clamp(m_authoredViews.ipd, profile.ipdMinMm / 1000.f, profile.ipdMaxMm / 1000.f);
				// The inset must stay inside the narrower field of view.
				for (const auto& fov : views.outerFov)
					m_authoredViews.insetHalfAngle =
						std::min({m_authoredViews.insetHalfAngle, (fov.angleRight - fov.angleLeft) / 2,
								  (fov.angleUp - fov.angleDown) / 2});
				ApplyOwned(ControlChannel::Views, SetViewSettings{m_authoredViews});
			}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("Sets the field of view. Launch profiles set the render size");
	FieldLabel("IPD");
	const ViewSettings defaults;
	const float minimum = model ? model->ipdMinMm / 1000.f : .04f, maximum = model ? model->ipdMaxMm / 1000.f : .09f;
	float millimetres = m_authoredViews.ipd * 1000;
	ImGui::SetNextItemWidth(-1);
	const bool slid = ImGui::SliderFloat("##IPD", &millimetres, minimum * 1000, maximum * 1000, "%.1f mm");
	if (slid)
		m_authoredViews.ipd = millimetres / 1000;
	bool edited = Field(
		"##IPD", slid, true,
		NumberValue(m_authoredViews.ipd, std::clamp(defaults.ipd, minimum, maximum), .001f, minimum, maximum, "%.4f"));
	if (model)
		ImGui::SetItemTooltip("%s lenses adjust from %d to %d mm", name(*model).c_str(), model->ipdMinMm,
							  model->ipdMaxMm);
	FieldLabel("Inset angle");
	float insetMaximum = std::numbers::pi_v<float> / 2;
	for (const auto& fov : m_authoredViews.outerFov)
		insetMaximum =
			std::min({insetMaximum, (fov.angleRight - fov.angleLeft) / 2, (fov.angleUp - fov.angleDown) / 2});
	const float maximumDegrees = insetMaximum * 180 / std::numbers::pi_v<float>;
	const float minimumDegrees = std::min(.1f, maximumDegrees);
	ImGui::SetNextItemWidth(-1);
	edited |= Field("##Inset half angle",
					ImGui::SliderAngle("##Inset half angle", &m_authoredViews.insetHalfAngle, minimumDegrees,
									   maximumDegrees, "%.1f°"),
					true,
					AngleValue(m_authoredViews.insetHalfAngle, std::min(defaults.insetHalfAngle, insetMaximum),
							   minimumDegrees, maximumDegrees));
	ImGui::SetItemTooltip("Half angle of the sharper inset view, for apps that use foveated views");
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Views, SetViewSettings{m_authoredViews});
	ImGui::EndDisabled();
	ImGui::TextDisabled("Renders %u x %u per eye, inset %u x %u", m_authoredViews.outerWidth,
						m_authoredViews.outerHeight, m_authoredViews.insetWidth, m_authoredViews.insetHeight);
	m_fieldScope = scope;
	return std::exchange(m_edit, {});
}
} // namespace simxr::app
