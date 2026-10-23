#include "Pch.h"
#include "app/widgets/Theme.h"
#include "app/services/PlayerRig.h"
#include "app/widgets/SpatialEditor.h"
#include <cmath>

namespace simxr::app
{
bool SpatialEditor::DrawPreview(Selection& selection, ImVec2 minimum, ImVec2 maximum, const FrameView& camera,
								unsigned eye)
{
	PrepareSelection(selection);
	if (eye >= m_previewGizmos.size() || !Editable() || m_lookingBack)
		return false;
	auto scene = LiveScene();
	const auto editable = EditableDevices(scene);
	const int selected = SelectedDevice(selection);
	SpatialEditResult gizmo;
	ImGui::PushID(static_cast<int>(eye));
	// The head gizmo would sit on the eye itself; the rig view owns it.
	if (selected > 0)
	{
		ImGui::PushID(selected);
		gizmo = DrawPoseGizmo("Preview pose", m_previewGizmos[eye], camera, minimum, maximum, scene.devices[selected],
							  editable[selected]);
		ImGui::PopID();
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
			// Only devices resolved by the app's pose bindings get preview markers.
			if (device > 2 &&
				(!m_observed->boundTrackers[device - 3] || !m_observed->values.tracking.trackers[device - 3].connected))
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
			// The letter sits on the marker's visual centre; a triangle's is a third of the way back from its tip.
			const char label[2]{scene.labels[device], 0};
			const auto letter = ImGui::CalcTextSize(label);
			const float back = outside ? radius / 3 : 0;
			const ImVec2 middle{position.x - direction.x * back, position.y - direction.y * back};
			draw->AddText({std::round(middle.x - letter.x / 2), std::round(middle.y - letter.y / 2)}, theme::Ink,
						  label);
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
				if (outside && device < 3)
				{
					m_rig.Preset(device - 1, HandPreset::Forward);
					EditPose(device, m_rig.Hand(device - 1), true);
				}
			}
			if (markerHovered)
			{
				if (device > 2)
					ImGui::SetTooltip("%s tracker", TrackerRoleName(m_rig.TrackerDeviceAt(device - 3).role));
				else
					ImGui::SetTooltip("%s", outside ? "Bring forward" : "Select hand");
			}
			ImGui::PopID();
		}
	ImGui::SetCursorScreenPos({saved.x, saved.y - ImGui::GetStyle().ItemSpacing.y});
	ImGui::Dummy({0, 0});
	ImGui::PopID();
	return hovered;
}
} // namespace simxr::app
