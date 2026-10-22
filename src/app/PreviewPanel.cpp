#include "app/PreviewPanel.h"
#include "app/DeviceControlPanel.h"
#include "app/PilotController.h"
#include "app/RuntimeSession.h"
#include "app/SpatialEditor.h"
#include "app/Theme.h"
#include "app/Window.h"
#include "app/widgets/Controls.h"

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
	// Composited and display output are the normal case and need no label.
	if (image.target == wire::TargetKind::LayerView)
		return " \u00b7 Layer " + std::to_string(image.layer + 1) + ", view " + std::to_string(image.view + 1);
	if (image.target == wire::TargetKind::ComposedEyes || image.target == wire::TargetKind::VirtualDisplay)
		return "";
	return " \u00b7 Captured image";
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
		const auto image = m_session.Preview().Selected();
		const bool live = view.time.following;
		if (image && image->ready && !image->released)
		{
			// The header row starts at the image's left edge when both fit.
			const float available = ImGui::GetContentRegionAvail().x;
			const float imageLeft = std::max(0.0f, (available - m_previousImageSize.x) / 2);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::min(imageLeft, std::max(0.0f, available - m_headerWidth)));
			const float headerStart = ImGui::GetCursorScreenPos().x;
			if (view.manual || !live)
			{
				ImGui::TextColored(theme::Vec(view.manual ? theme::Paused : theme::Past), "%s",
								   view.manual ? "PAUSED" : "PAST");
				ImGui::SameLine();
			}
			ImGui::TextDisabled("Frame %llu%s", static_cast<unsigned long long>(image->frame),
								ImageLabel(*image).c_str());
			DrawImage(shell, view, pilotArea, headerStart);
		}
		else if (m_spatial.Expanded())
			m_spatial.DrawExpanded(shell.selection, ImGui::GetContentRegionAvail());
		else if (!DrawKept(view))
			DrawEmpty(shell, view);
	}
	ImGui::EndChild();
	return pilotArea;
}
bool PreviewPanel::DrawKept(const RuntimeView& view)
{
	if (view.time.following || !view.connected)
		return false;
	auto& history = m_session.Preview().History();
	const auto shot = history.Nearest(view.time.cursor, view.time.epoch);
	if (!shot)
		return false;
	ImGui::TextColored(theme::Vec(view.manual ? theme::Paused : theme::Past), "%s", view.manual ? "PAUSED" : "PAST");
	ImGui::SameLine();
	ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(view.time.cursor.frame));
	ImGui::SameLine();
	StateChip("Kept preview", theme::TextDim, "Preview", "Downscaled copy of what was shown; not the exact frame");
	if (shot->frame.frame != view.time.cursor.frame)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("from frame %llu", static_cast<unsigned long long>(shot->frame.frame));
	}
	const auto available = ImGui::GetContentRegionAvail();
	const float aspect = history.Aspect();
	const float width = std::max(16.f, std::min(available.x, available.y / aspect));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (available.x - width) / 2));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (available.y - width * aspect) / 2));
	history.Draw(*shot, width);
	return true;
}
void PreviewPanel::DrawEmpty(ShellState& shell, const RuntimeView& view)
{
	const auto available = ImGui::GetContentRegionAvail();
	const auto origin = ImGui::GetCursorScreenPos();
	const float unit = ImGui::GetFontSize() / 17;
	const ImVec2 center{origin.x + available.x / 2, origin.y + available.y / 2};
	auto* draw = ImGui::GetWindowDrawList();
	const auto line = [&](const char* text, float y, ImU32 color)
	{
		const auto size = ImGui::CalcTextSize(text);
		draw->AddText({center.x - size.x / 2, y}, color, text);
	};
	if (!view.connected)
	{
		DrawIcon(draw, Icon::App, {center.x, center.y - 48 * unit}, 40 * unit, theme::TextDim);
		line("Launch or connect an OpenXR app", center.y - 14 * unit, theme::Text);
		const char* label = "Open app";
		const float width = ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x;
		ImGui::SetCursorScreenPos({center.x - width / 2, center.y + 16 * unit});
		if (IconButton("Open app", Icon::App, "Launch or connect an OpenXR app", false, label))
			shell.openAppPicker = true;
		return;
	}
	if (!view.time.following)
	{
		const auto text = "Frame " + std::to_string(view.time.cursor.frame);
		line(text.c_str(), center.y - 20 * unit, theme::Text);
		line("Image not kept", center.y + 4 * unit, theme::TextDim);
		return;
	}
	// A small spinner while the first image is on its way.
	const float angle = static_cast<float>(ImGui::GetTime()) * 6;
	draw->PathArcTo({center.x, center.y - 24 * unit}, 10 * unit, angle, angle + 4.4f);
	draw->PathStroke(theme::TextDim, ImDrawFlags_None, 2 * unit);
	line(view.sessionAvailable ? "Waiting for first frame\u2026" : "Waiting for the app to start XR\u2026",
		 center.y, theme::TextDim);
}
void PreviewPanel::DrawImage(ShellState& shell, const RuntimeView& view, bool& pilotArea, float headerStart)
{
	auto& preview = m_session.Preview();
	const auto image = preview.Selected();
	const bool live = view.time.following;
	if (m_slice >= static_cast<int>(image->slices))
		m_slice = 0;
	// Two slices with two views are the left and right eye, live or captured.
	const bool eyes = image->slices == 2 && image->description.views.size() == 2;
	if (image->slices > 1)
	{
		ImGui::SameLine();
		for (int slice = eyes ? 0 : -1; slice < static_cast<int>(image->slices); ++slice)
		{
			std::string label = eyes ? (slice == 0 ? "Left" : "Right") : "View " + std::to_string(slice + 1);
			if (slice < 0)
				label = "All";
			if (m_slice == slice)
				ImGui::PushStyleColor(ImGuiCol_Button, theme::Mix(theme::SurfaceRaised, theme::Accent, .45f));
			const bool chosen = ImGui::SmallButton(label.c_str());
			if (m_slice == slice)
				ImGui::PopStyleColor();
			if (chosen)
				m_slice = slice;
			ImGui::SameLine(0, 2);
		}
		if (eyes)
		{
			const bool both = m_slice == -1;
			if (both)
				ImGui::PushStyleColor(ImGuiCol_Button, theme::Mix(theme::SurfaceRaised, theme::Accent, .45f));
			if (ImGui::SmallButton("Both"))
				m_slice = -1;
			if (both)
				ImGui::PopStyleColor();
		}
	}
	ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
	if (ImGui::SmallButton("Fit"))
		m_fit = true;
	ImGui::SetItemTooltip("Fit the image to the preview");
	ImGui::SameLine(0, 2);
	if (ImGui::SmallButton("1:1"))
	{
		m_fit = false;
		m_zoom = 1;
	}
	ImGui::SetItemTooltip("1:1 shows native pixels. Middle-drag pans.");
	ImGui::SameLine(0, 2);
	const auto zoom = std::to_string(static_cast<int>(m_zoom * 100 + .5f)) + "%##Zoom";
	if (ImGui::SmallButton(zoom.c_str()))
		ImGui::OpenPopup("Image zoom");
	ImGui::SetItemTooltip("Zoom. Mouse wheel zooms; middle-drag pans.");
	if (ImGui::BeginPopup("Image zoom"))
	{
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
		if (ImGui::SliderFloat("##Zoom", &m_zoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
			m_fit = false;
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	m_spatial.DrawSpaceToggle();
	if (live && !view.manual && view.connected)
	{
		ImGui::SameLine(0, 2);
		if (ImGui::SmallButton("?##Pilot keys"))
			ImGui::OpenPopup("Piloting");
		ImGui::SetItemTooltip("Pilot keys");
		if (ImGui::BeginPopup("Piloting"))
		{
			m_pilot.DrawHelp();
			ImGui::EndPopup();
		}
	}
	m_headerWidth = ImGui::GetItemRectMax().x - headerStart;
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
