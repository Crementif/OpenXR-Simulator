#include "app/PreviewPanel.h"
#include "app/DeviceControlPanel.h"
#include "app/PilotController.h"
#include "app/RuntimeSession.h"
#include "app/SpatialEditor.h"
#include "app/Window.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace simxr::app
{
namespace
{
namespace wire = control;
std::string ImageLabel(const PreviewImageSummary& image)
{
	if (image.target == wire::TargetKind::ComposedEyes)
		return "Composited output";
	if (image.target == wire::TargetKind::VirtualDisplay)
		return "Display output";
	if (image.target == wire::TargetKind::LayerView)
		return "Submitted layer " + std::to_string(image.layer + 1) + " / view " + std::to_string(image.view + 1);
	return "Captured image";
}
} // namespace
PreviewPanel::PreviewPanel(RuntimeSession& session, PilotController& pilot, SpatialEditor& spatial,
						   DeviceControlPanel& devices, Window* window)
	: m_session(session), m_pilot(pilot), m_spatial(spatial), m_devices(devices), m_window(window)
{
}
bool PreviewPanel::Draw(ShellState& shell, const RuntimeView& view)
{
	bool pilotArea = false;
	ImGui::SetCursorPos({shell.preview.x, shell.preview.y});
	if (ImGui::BeginChild("GPU preview", {shell.preview.width, shell.preview.height}))
	{
		auto& preview = m_session.Preview();
		const auto image = preview.Selected();
		const bool live = view.time.following;
		const auto accent = !view.connected ? ImVec4{0.62f, 0.66f, 0.72f, 1}
							: view.manual	? ImVec4{0.96f, 0.69f, 0.32f, 1}
							: !live			? ImVec4{0.48f, 0.72f, 0.95f, 1}
											: ImVec4{0.35f, 0.80f, 0.65f, 1};
		ImGui::TextColored(accent, "%s",
						   !view.connected ? "DISCONNECTED"
						   : view.manual   ? "PAUSED"
						   : !live		   ? "PAST"
										   : "LIVE");
		if (image)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Frame %llu  /  %s", static_cast<unsigned long long>(image->frame),
								ImageLabel(*image).c_str());
		}
		if (!live && !view.manual && view.connected)
		{
			if (ImGui::SmallButton("Show live output"))
				m_session.SetLive(true);
			ImGui::SetItemTooltip("Follows new images. The application is already running.");
		}
		if (image && image->ready && !image->released)
			DrawImage(shell, view, pilotArea);
		else
		{
			ImGui::Spacing();
			if (m_spatial.Expanded())
				m_spatial.DrawExpanded(shell.selection, ImGui::GetContentRegionAvail());
			else if (!live)
				ImGui::TextDisabled("Frame %llu / No image", static_cast<unsigned long long>(view.time.cursor.frame));
			else
				ImGui::TextDisabled("%s", view.connected ? "Waiting for image" : "No app");
		}
	}
	ImGui::EndChild();
	return pilotArea;
}
void PreviewPanel::DrawImage(ShellState& shell, const RuntimeView& view, bool& pilotArea)
{
	auto& preview = m_session.Preview();
	const auto image = preview.Selected();
	const bool live = view.time.following;
	if (m_slice >= static_cast<int>(image->slices))
		m_slice = 0;
	const bool eyes = image->target == wire::TargetKind::ComposedEyes && image->slices == 2;
	if (eyes)
	{
		for (const auto [label, slice] : {std::pair{"Left eye", 0}, {"Right eye", 1}, {"Both", -1}})
		{
			if (m_slice == slice)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			const bool chosen = ImGui::Button(label);
			if (m_slice == slice)
				ImGui::PopStyleColor();
			if (chosen)
				m_slice = slice;
			ImGui::SameLine();
		}
	}
	else if (image->slices > 1)
	{
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
		const auto label = m_slice < 0 ? "All slices" : "Slice " + std::to_string(m_slice + 1);
		if (ImGui::BeginCombo("##Image slice", label.c_str()))
		{
			for (int slice = -1; slice < static_cast<int>(image->slices); ++slice)
				if (ImGui::Selectable(slice < 0 ? "All slices" : ("Slice " + std::to_string(slice + 1)).c_str(),
									  m_slice == slice))
					m_slice = slice;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
	}
	if (ImGui::Button("Fit"))
		m_fit = true;
	ImGui::SameLine();
	if (ImGui::Button("1:1"))
	{
		m_fit = false;
		m_zoom = 1;
	}
	ImGui::SameLine();
	if (ImGui::Button("Zoom..."))
		ImGui::OpenPopup("Image zoom");
	if (ImGui::BeginPopup("Image zoom"))
	{
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
		if (ImGui::SliderFloat("##Zoom", &m_zoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
			m_fit = false;
		ImGui::TextUnformatted("Middle-drag pans the image.\n1:1 shows native pixels.");
		ImGui::EndPopup();
	}
	if (live && !view.manual && view.connected)
	{
		ImGui::SameLine();
		if (ImGui::Button("Pilot help"))
			ImGui::OpenPopup("Piloting");
		if (ImGui::BeginPopup("Piloting"))
		{
			m_pilot.DrawHelp();
			ImGui::EndPopup();
		}
	}
	ImGui::SameLine();
	m_spatial.DrawSpaceToggle();
	ImGui::BeginChild("Image area", {0, 0}, 0, ImGuiWindowFlags_NoScrollWithMouse);
	if (m_spatial.Expanded())
	{
		const auto minimum = ImGui::GetCursorScreenPos();
		const auto available = ImGui::GetContentRegionAvail();
		m_spatial.DrawExpanded(shell.selection, available);
		const auto after = ImGui::GetCursorScreenPos();
		const float width = available.x * .25f;
		const float height = width * image->description.height / image->description.width;
		ImGui::SetCursorScreenPos({minimum.x + available.x - width - 8, minimum.y + available.y - height - 8});
		preview.DrawSelected(width, m_slice < 0 ? 0 : m_slice);
		ImGui::SetCursorScreenPos({after.x, after.y - ImGui::GetStyle().ItemSpacing.y});
		ImGui::Dummy({0, 0});
		ImGui::EndChild();
		return;
	}
	const bool busy = m_pilot.Active() || m_spatial.Active();
	if (!busy && !m_fit && ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel)
		m_zoom = std::clamp(m_zoom * std::pow(1.15f, ImGui::GetIO().MouseWheel), .05f, 16.0f);
	if (!busy && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && !m_fit)
		m_panning = true;
	if (busy || !ImGui::IsMouseDown(ImGuiMouseButton_Middle) || m_fit)
		m_panning = false;
	if (m_panning)
	{
		ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseDelta.x);
		ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	}
	const auto available = ImGui::GetContentRegionAvail();
	const auto& description = image->description;
	const float slices = m_slice < 0 ? static_cast<float>(image->slices) : 1;
	float width = description.width * m_zoom;
	if (m_fit)
		width = std::max(16.0f, std::min((available.x - 12 * (slices - 1)) / slices,
										 std::max(16.0f, available.y) * description.width / description.height));
	const float totalWidth = width * slices + 12 * (slices - 1);
	const float height = width * description.height / description.width;
	if (m_fit)
		m_zoom = width / description.width;
	else if (totalWidth != m_previousImageSize.x || height != m_previousImageSize.y)
	{
		ImGui::SetScrollX(std::max(0.0f, (totalWidth - available.x) / 2));
		ImGui::SetScrollY(std::max(0.0f, (height - available.y) / 2));
	}
	m_previousImageSize = {totalWidth, height};
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (available.x - totalWidth) / 2));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (available.y - height) / 2));
	const auto imageMin = ImGui::GetCursorScreenPos();
	const ImVec2 imageMax{imageMin.x + totalWidth, imageMin.y + height};
	preview.DrawSelected(width, m_slice < 0 ? std::nullopt : std::optional<std::size_t>(m_slice));
	bool overlayHovered = false;
	if (!m_pilot.Active() && (!view.manual || image->frame == m_spatial.ObservedFrame()))
		for (std::size_t ordinal = 0; ordinal < static_cast<std::size_t>(slices); ++ordinal)
		{
			const auto eye = m_slice < 0 ? ordinal : static_cast<std::size_t>(m_slice);
			if (eye < description.views.size())
			{
				const ImVec2 eyeMin{imageMin.x + ordinal * (width + 12), imageMin.y};
				overlayHovered |= m_spatial.DrawPreview(shell.selection, eyeMin, {eyeMin.x + width, imageMax.y},
														description.views[eye], static_cast<unsigned>(eye));
			}
		}
	pilotArea = live && view.connected && view.sessionRunning && !view.manual;
	if (m_window)
	{
		auto& input = m_session.ManualInput();
		const bool hudHovered = m_pilot.DrawHud(imageMin, imageMax, input);
		m_pilot.Update(*m_window, input, pilotArea,
					   !hudHovered && !overlayHovered && ImGui::IsMouseHoveringRect(imageMin, imageMax));
		m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
		if (!m_pilot.Error().empty() && m_pilot.Error() != m_pilotError)
			m_session.Notify("Piloting stopped", m_pilot.Error());
		m_pilotError = m_pilot.Error();
	}
	ImGui::EndChild();
}
} // namespace simxr::app
