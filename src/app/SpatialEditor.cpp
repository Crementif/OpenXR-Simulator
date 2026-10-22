#include "app/SpatialEditor.h"
#include "Math.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"
#include "simulation/TrackingSystem.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace simxr::app
{
namespace
{
int SelectedDevice(const Selection& selection)
{
	if (const auto device = std::get_if<DeviceSel>(&selection))
	{
		switch (device->page)
		{
		case DevicePage::Head:
			return 0;
		case DevicePage::Left:
			return 1;
		case DevicePage::Right:
			return 2;
		case DevicePage::Trackers:
			return 3 + static_cast<int>(std::min(device->slot, 3u));
		default:
			break;
		}
	}
	return -1;
}
void SelectDevice(Selection& selection, int device)
{
	if (device >= 0)
		selection = DeviceSel{device == 0	? DevicePage::Head
							  : device == 1 ? DevicePage::Left
							  : device == 2 ? DevicePage::Right
											: DevicePage::Trackers,
							  device > 2 ? static_cast<unsigned>(device - 3) : 0};
}
ControlChannel DeviceChannel(int device)
{
	return device == 0	 ? ControlChannel::Head
		   : device == 1 ? ControlChannel::LeftPose
		   : device == 2 ? ControlChannel::RightPose
						 : ControlChannel::Trackers;
}
SpatialScene SceneFromTracking(const TrackingSnapshot& tracking)
{
	SpatialScene scene;
	scene.root = tracking.body.joints[static_cast<std::size_t>(BodyJoint::Root)].pose;
	scene.devices[0] = tracking.head.pose;
	scene.available[0] = (tracking.head.flags & ValidPoseFlags) == ValidPoseFlags;
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		scene.devices[hand + 1] = tracking.controllers[hand].pose.pose;
		scene.available[hand + 1] = tracking.controllers[hand].connected &&
									(tracking.controllers[hand].pose.flags & ValidPoseFlags) == ValidPoseFlags;
	}
	for (std::size_t slot = 0; slot < 4; ++slot)
	{
		scene.devices[slot + 3] = tracking.trackers[slot].pose.pose;
		scene.available[slot + 3] = tracking.trackers[slot].device.connected &&
									(tracking.trackers[slot].pose.flags & ValidPoseFlags) == ValidPoseFlags;
	}
	scene.body = tracking.body;
	if ((tracking.gaze.flags & ValidPoseFlags) == ValidPoseFlags)
		scene.gaze = tracking.gaze.pose;
	return scene;
}
} // namespace
SpatialEditor::SpatialEditor(PlayerRig& rig, ManualInputController& input) : m_rig(rig), m_input(input) {}
void SpatialEditor::Reset()
{
	CancelPoseGizmo(m_view.gizmo);
	for (auto& state : m_previewGizmos)
		CancelPoseGizmo(state);
	m_observed.reset();
	m_historical.reset();
	m_observation = m_historyIdentity = 0;
	m_arcballs = {};
	m_reach = {};
	m_previewGizmos = {};
	m_view.gizmo = {};
	m_editable = false;
}
void SpatialEditor::Observe(const ManualInputSnapshot& snapshot, bool piloting)
{
	m_source = snapshot.source;
	if (snapshot.observation == m_observation)
		return;
	const bool first = m_observation == 0;
	m_observation = snapshot.observation;
	try
	{
		m_observed = ReadObservedSetup(snapshot.state);
		if (first)
			m_rig.Reset(*m_observed);
		else if (!piloting && !Active() && !snapshot.posePending)
			m_rig.Observe(*m_observed);
		m_error = snapshot.error;
	}
	catch (const std::exception& error)
	{
		m_observed.reset();
		m_error = error.what();
	}
}
void SpatialEditor::SetContext(bool editable, bool lookingBack, std::span<const FrameView> eyes,
							   const control::Message* historical, std::uint64_t historyIdentity)
{
	if (!editable || lookingBack)
	{
		CancelPoseGizmo(m_view.gizmo);
		for (auto& state : m_previewGizmos)
			CancelPoseGizmo(state);
	}
	m_editable = editable;
	m_lookingBack = lookingBack;
	m_markerRects = {};
	m_rotationBallRect = m_reachMapRect = {};
	m_eye = eyes.empty() ? std::nullopt : std::optional{eyes.front()};
	if (historyIdentity != m_historyIdentity)
	{
		m_historyIdentity = historyIdentity;
		m_historical.reset();
	}
	if (historical && !m_historical)
		try
		{
			if (const auto tracking = ReadInspectionTracking(*historical))
				m_historical = SceneFromTracking(*tracking);
		}
		catch (const std::exception& error)
		{
			m_error = error.what();
		}
	if (m_historical)
		m_historical->eye = m_eye;
}
bool SpatialEditor::Active() const
{
	return m_view.gizmo.usingGizmo || m_reach.dragging ||
		   std::any_of(m_arcballs.begin(), m_arcballs.end(),
					   [](const auto& state)
					   {
						   return state.dragging;
					   }) ||
		   std::any_of(m_previewGizmos.begin(), m_previewGizmos.end(),
					   [](const auto& state)
					   {
						   return state.usingGizmo;
					   });
}
SpatialScene SpatialEditor::LiveScene() const
{
	if (!m_observed || !m_rig.Ready())
		return {};
	auto tracking = m_observed->values.tracking;
	tracking.head.pose = m_rig.Head();
	tracking.body.rootPose = m_rig.Root();
	for (std::size_t hand = 0; hand < 2; ++hand)
		tracking.controllers[hand].pose.pose = m_rig.Hand(hand);
	for (std::size_t slot = 0; slot < 4; ++slot)
		tracking.trackers[slot].poseOverride = m_rig.Tracker(slot);
	auto scene = SceneFromTracking(
		TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, tracking, BodyModel{m_observed->values.proportions}));
	scene.root = m_rig.Root();
	scene.eye = m_lookingBack ? std::nullopt : m_eye;
	return scene;
}
std::array<bool, 7> SpatialEditor::EditableDevices(const SpatialScene& scene) const
{
	std::array<bool, 7> result{};
	if (!Editable() || m_lookingBack)
		return result;
	for (int device = 0; device < 7; ++device)
	{
		const auto channel = DeviceChannel(device);
		const auto owner = m_observed->owners[static_cast<std::size_t>(channel)];
		result[device] = scene.available[device] && (!owner || owner == m_source);
		for (std::size_t scenario = 0; scenario < m_observed->scenarios.size(); ++scenario)
			if (m_observed->scenarios[scenario] &&
				ScenarioInputChannel(static_cast<FailureScenario>(scenario)) == channel)
				result[device] = false;
	}
	return result;
}
void SpatialEditor::EditPose(int device, XrPosef pose, bool final)
{
	if (!Editable() || device < 0 || !EditableDevices(LiveScene())[device])
		return;
	if (device == 0)
		m_rig.SetHeadWorld(pose);
	else if (device < 3)
		m_rig.SetHandWorld(device - 1, pose);
	else
		m_rig.SetTrackerWorld(device - 3, pose);
	auto edit = m_rig.Transaction(*m_observed, m_source, true);
	edit.continuous = !final;
	if (!m_input.Submit(edit))
		m_error = "Pose rejected — retry";
}
void SpatialEditor::ApplyResult(const SpatialEditResult& result, int device, XrPosef pose)
{
	if (result.changed || result.finished)
		EditPose(device, pose, result.finished);
}
void SpatialEditor::DrawRig(Selection& selection, ImVec2 size)
{
	if (m_lookingBack && !m_historical)
	{
		ImGui::TextDisabled("Pose unavailable");
		return;
	}
	const auto live = LiveScene();
	auto scene = m_lookingBack ? *m_historical : live;
	int selected = SelectedDevice(selection);
	const int previous = selected;
	const auto result =
		RigView("Rig view", m_view, scene, selected, EditableDevices(scene), size, m_lookingBack ? &live : nullptr);
	if (selected != previous)
		SelectDevice(selection, selected);
	if (selected >= 0)
		ApplyResult(result, selected, scene.devices[selected]);
}
void SpatialEditor::DrawRigView(Selection& selection)
{
	if (!std::holds_alternative<DeviceSel>(selection) || (!m_observed && !m_historical) || m_view.expanded)
		return;
	// A short drawer keeps room for the controls below the rig.
	const float scale = ImGui::GetFontSize() / 16;
	const auto available = ImGui::GetContentRegionAvail();
	DrawRig(selection, {available.x / scale, std::clamp(available.y / scale * .3f, 120.0f, 220.0f)});
}
void SpatialEditor::DrawPlacement(Selection& selection)
{
	const auto devicePage = std::get_if<DeviceSel>(&selection);
	if (!devicePage || !m_observed || m_lookingBack)
		return;
	const auto page = devicePage->page;
	const float scale = ImGui::GetFontSize() / 16;
	auto scene = LiveScene();
	const auto editable = EditableDevices(scene);
	const int selected = SelectedDevice(selection);
	const bool reach = page == DevicePage::Player || page == DevicePage::Left || page == DevicePage::Right;
	const bool sideBySide = selected >= 0 && reach && ImGui::GetContentRegionAvail().x / scale >= 340;
	if (selected >= 0)
	{
		if (sideBySide)
			ImGui::BeginGroup();
		ImGui::SeparatorText("Rotation");
		const auto anchor = selected == 0  ? Anchor::Body
							: selected < 3 ? m_rig.HandAnchor(selected - 1)
										   : m_rig.TrackerAnchor(selected - 3);
		const auto reset = anchor == Anchor::World	? IdentityPose().orientation
						   : anchor == Anchor::Head ? m_rig.Head().orientation
													: m_rig.Root().orientation;
		const auto camera = RigCamera(m_view, scene, 1);
		auto pose = scene.devices[selected];
		const auto rotationMinimum = ImGui::GetCursorScreenPos();
		m_rotationBallRect = {rotationMinimum.x, rotationMinimum.y, 160 * scale, 160 * scale};
		const auto result = ArcballWidget("Rotation ball", m_arcballs[selected], pose.orientation,
										  camera.pose.orientation, reset, editable[selected]);
		ApplyResult(result, selected, pose);
		if (!editable[selected])
			ImGui::TextDisabled("Read only");
		if (sideBySide)
		{
			ImGui::EndGroup();
			ImGui::SameLine();
		}
	}
	if (reach)
	{
		if (sideBySide)
			ImGui::BeginGroup();
		ImGui::SeparatorText("Reach");
		if (!m_reach.dragging && (selected == 1 || selected == 2))
			m_reach.hand = selected - 1;
		std::array<XrPosef, 2> hands{m_rig.Hand(0), m_rig.Hand(1)};
		const auto reachMinimum = ImGui::GetCursorScreenPos();
		m_reachMapRect = {reachMinimum.x, reachMinimum.y, ImGui::GetContentRegionAvail().x, 160 * scale};
		const auto result =
			ReachMap("Reach map", m_reach, m_rig.Root(), m_rig.Head(), m_observed->values.proportions.shoulderWidth,
					 m_observed->values.proportions.height * .44f, hands, {editable[1], editable[2]},
					 ImGui::GetContentRegionAvail().x / scale);
		ApplyResult(result, m_reach.hand + 1, hands[m_reach.hand]);
		if (result.active && (page == DevicePage::Left || page == DevicePage::Right))
			SelectDevice(selection, m_reach.hand + 1);
		if (sideBySide)
			ImGui::EndGroup();
	}
	if (!m_error.empty() && ImGui::IsWindowHovered())
		ImGui::SetTooltip("%s", m_error.c_str());
}
void SpatialEditor::DrawExpanded(Selection& selection, ImVec2 size)
{
	const float scale = ImGui::GetFontSize() / 16;
	DrawRig(selection, {size.x / scale, size.y / scale});
}
void SpatialEditor::DrawSpaceToggle()
{
	if (IconButton("Gizmo space", m_view.gizmo.local ? Icon::Axes : Icon::Globe,
				   m_view.gizmo.local ? "Gizmo axes: local. Space cycles move, rotate or both; Ctrl snaps."
									  : "Gizmo axes: world. Space cycles move, rotate or both; Ctrl snaps."))
		m_view.gizmo.local = !m_view.gizmo.local;
	for (auto& gizmo : m_previewGizmos)
		gizmo.local = m_view.gizmo.local;
}
bool SpatialEditor::DrawPreview(Selection& selection, ImVec2 minimum, ImVec2 maximum, const FrameView& camera,
								unsigned eye)
{
	if (eye >= m_previewGizmos.size() || !Editable() || m_lookingBack)
		return false;
	auto scene = LiveScene();
	const auto editable = EditableDevices(scene);
	const int selected = SelectedDevice(selection);
	SpatialEditResult gizmo;
	ImGui::PushID(static_cast<int>(eye));
	if (selected >= 0)
	{
		gizmo = DrawPoseGizmo("Preview pose", m_previewGizmos[eye], camera, minimum, maximum, scene.devices[selected],
							  editable[selected]);
		ApplyResult(gizmo, selected, scene.devices[selected]);
	}
	const auto saved = ImGui::GetCursorScreenPos();
	auto* draw = ImGui::GetWindowDrawList();
	const auto clipMinimum = draw->GetClipRectMin(), clipMaximum = draw->GetClipRectMax();
	const ImVec2 visibleMin{std::max(minimum.x, clipMinimum.x), std::max(minimum.y, clipMinimum.y)};
	const ImVec2 visibleMax{std::min(maximum.x, clipMaximum.x),
							std::min(maximum.y, clipMaximum.y) -
								(maximum.y <= clipMaximum.y ? 46 * ImGui::GetFontSize() / 16 : 0)};
	const float scale = ImGui::GetFontSize() / 16, radius = 10 * scale;
	bool hovered = gizmo.hovered || gizmo.active;
	std::array<ImVec2, 6> arrows;
	std::size_t arrowCount = 0;
	if (visibleMax.x - visibleMin.x > 4 * radius && visibleMax.y - visibleMin.y > 4 * radius)
		for (int device = 1; device < 7; ++device)
		{
			if (!scene.available[device])
				continue;
			const auto projected = ProjectPoint(camera, scene.devices[device].position);
			ImVec2 position{minimum.x + projected.uv.x * (maximum.x - minimum.x),
							minimum.y + projected.uv.y * (maximum.y - minimum.y)};
			const bool outside = projected.behind || position.x < visibleMin.x + radius ||
								 position.x > visibleMax.x - radius || position.y < visibleMin.y + radius ||
								 position.y > std::min(maximum.y, clipMaximum.y) - radius;
			ImVec2 direction{};
			if (outside)
			{
				const ImVec2 center{(visibleMin.x + visibleMax.x) / 2, (visibleMin.y + visibleMax.y) / 2};
				direction = {position.x - center.x, position.y - center.y};
				if (std::hypot(direction.x, direction.y) < .001f)
					direction = {0, 1};
				const float edge =
					std::max(std::abs(direction.x) / ((visibleMax.x - visibleMin.x) / 2 - radius * 1.6f),
							 std::abs(direction.y) / ((visibleMax.y - visibleMin.y) / 2 - radius * 1.6f));
				position = {center.x + direction.x / edge, center.y + direction.y / edge};
				const float length = std::hypot(direction.x, direction.y);
				direction = {direction.x / length, direction.y / length};
			}
			if (outside)
			{
				const auto original = position;
				const bool horizontal = std::abs(position.y - visibleMin.y) < radius * 2 ||
										std::abs(position.y - visibleMax.y) < radius * 2;
				for (int attempt = 0; attempt < 12; ++attempt)
				{
					if (std::none_of(arrows.begin(), arrows.begin() + arrowCount,
									 [&](ImVec2 other)
									 {
										 return std::hypot(position.x - other.x, position.y - other.y) < radius * 2.5f;
									 }))
						break;
					const float offset = (attempt / 2 + 1) * radius * 2.6f * (attempt % 2 ? -1 : 1);
					position = original;
					if (horizontal)
						position.x =
							std::clamp(position.x + offset, visibleMin.x + radius * 1.6f, visibleMax.x - radius * 1.6f);
					else
						position.y =
							std::clamp(position.y + offset, visibleMin.y + radius * 1.6f, visibleMax.y - radius * 1.6f);
				}
				arrows[arrowCount++] = position;
			}
			if (!editable[device])
			{
				const auto owner = m_observed->owners[static_cast<std::size_t>(DeviceChannel(device))];
				draw->AddText(position, theme::Fade(theme::Warn, .9f),
							  owner && owner != m_source ? "Other owner" : "Scenario");
				continue;
			}
			if (!m_markerRects[device].width)
				m_markerRects[device] = {position.x - radius, position.y - radius, radius * 2, radius * 2};
			const auto color = DeviceColor(device);
			if (outside)
				draw->AddTriangleFilled({position.x + direction.x * radius, position.y + direction.y * radius},
										{position.x - direction.x * radius - direction.y * radius,
										 position.y - direction.y * radius + direction.x * radius},
										{position.x - direction.x * radius + direction.y * radius,
										 position.y - direction.y * radius - direction.x * radius},
										color);
			else
				draw->AddCircleFilled(position, radius, color);
			if (device == selected)
				draw->AddCircle(position, radius + 3 * scale, color, 32, 2 * scale);
			const char label[2]{device == 1 ? 'L' : device == 2 ? 'R' : static_cast<char>('1' + device - 3), 0};
			draw->AddText({position.x - 4 * scale, position.y - 7 * scale}, theme::Ink, label);
			ImGui::SetCursorScreenPos({position.x - radius, position.y - radius});
			ImGui::PushID(device);
			ImGui::BeginDisabled(gizmo.hovered || gizmo.active);
			const bool clicked = ImGui::InvisibleButton("Device marker", {radius * 2, radius * 2});
			const bool markerHovered = ImGui::IsItemHovered();
			ImGui::EndDisabled();
			hovered |= markerHovered;
			if (clicked)
			{
				SelectDevice(selection, device);
				if (outside)
				{
					if (device < 3)
					{
						m_rig.Preset(device - 1, HandPreset::Forward);
						EditPose(device, m_rig.Hand(device - 1), true);
					}
					else
						EditPose(device, Compose(m_rig.Root(), {{0, 0, 0, 1}, {0, m_rig.height - .15f, -.55f}}), true);
				}
			}
			if (markerHovered)
				ImGui::SetTooltip("%s", outside ? "Bring forward" : "Select device");
			ImGui::PopID();
		}
	ImGui::SetCursorScreenPos({saved.x, saved.y - ImGui::GetStyle().ItemSpacing.y});
	ImGui::Dummy({0, 0});
	ImGui::PopID();
	return hovered;
}
} // namespace simxr::app
