#include "app/PreviewPanel.h"
#include "app/DeviceControlPanel.h"
#include "app/PilotController.h"
#include "app/RuntimeSession.h"
#include "app/SpatialEditor.h"
#include "app/Theme.h"
#include "app/platform/Window.h"
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
		return "Layer " + std::to_string(image.layer + 1) + ", view " + std::to_string(image.view + 1);
	if (image.target == wire::TargetKind::ComposedEyes || image.target == wire::TargetKind::VirtualDisplay)
		return "";
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
		const auto image = m_session.Preview().Selected();
		const bool live = view.time.following;
		if (image && image->ready && !image->released)
		{
			DrawImage(shell, view, pilotArea);
			DrawCorner(*image, live);
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
	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(theme::Vec(theme::Past), "PAST");
	ImGui::SameLine();
	ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(view.time.cursor.frame));
	ImGui::SameLine();
	StateChip("Kept preview", theme::TextDim, "Preview", "Downscaled copy of what was shown; not the exact frame");
	if (shot->frame.frame != view.time.cursor.frame)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("from frame %llu", static_cast<unsigned long long>(shot->frame.frame));
	}
	const auto crop = m_crop.value_or(ImVec4{0, 0, 1, 1});
	if (m_crop)
	{
		ImGui::SameLine();
		StateChip("Kept crop", theme::Accent, "Crop", "The pinned region of a downscaled image");
	}
	const auto available = ImGui::GetContentRegionAvail();
	const float aspect = history.Aspect() * (crop.w - crop.y) / (crop.z - crop.x);
	const float width = std::max(16.f, std::min(available.x, available.y / aspect));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (available.x - width) / 2));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (available.y - width * aspect) / 2));
	history.Draw(*shot, width, crop);
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
void PreviewPanel::DrawCorner(const PreviewImageSummary& image, bool live)
{
	// Floats over the image's top right corner; last frame's width places it.
	const float scale = ImGui::GetFontSize() / 16, margin = 8 * scale;
	const ImVec2 padding{4 * scale, 4 * scale};
	const auto origin = ImGui::GetWindowPos();
	ImGui::SetCursorScreenPos({origin.x + ImGui::GetWindowWidth() - m_cornerWidth - padding.x * 2 - margin,
							   origin.y + margin});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Fade(theme::Bg, .72f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::Radius);
	const ImGuiChildFlags fit =
		ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding;
	ImGui::BeginChild("Image controls", {0, 0}, fit, ImGuiWindowFlags_NoScrollbar);
	const float start = ImGui::GetCursorScreenPos().x;
	ImGui::AlignTextToFramePadding();
	// Paused is the amber border; only looking back and an unusual image are said here.
	if (!live)
	{
		ImGui::TextColored(theme::Vec(theme::Past), "PAST");
		ImGui::SameLine();
	}
	if (const auto label = ImageLabel(image); !label.empty())
	{
		ImGui::TextDisabled("%s", label.c_str());
		ImGui::SameLine();
	}
	if (m_slice >= static_cast<int>(image.slices))
		m_slice = 0;
	DrawViewButton(image);
	if (m_crop)
	{
		ImGui::SameLine();
		const auto clear = Icon::Close;
		if (StateChip("Crop", theme::Accent, "Crop", "Showing a pinned region of each frame", &clear,
					  "Show the whole image"))
			m_crop.reset();
	}
	ImGui::SameLine();
	m_spatial.DrawSpaceToggle();
	m_cornerWidth = ImGui::GetItemRectMax().x - start;
	m_cornerHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}
void PreviewPanel::DrawImage(ShellState& shell, const RuntimeView& view, bool& pilotArea)
{
	auto& preview = m_session.Preview();
	const auto image = preview.Selected();
	const bool live = view.time.following;
	if (m_slice >= static_cast<int>(image->slices))
		m_slice = 0;
	ImGui::BeginChild("Image area", {0, 0}, 0, ImGuiWindowFlags_NoScrollWithMouse);
	if (m_spatial.Expanded())
	{
		const auto minimum = ImGui::GetCursorScreenPos();
		const auto available = ImGui::GetContentRegionAvail();
		m_spatial.DrawExpanded(shell.selection, available);
		const auto after = ImGui::GetCursorScreenPos();
		const float width = available.x * .25f;
		const float height = width * image->description.height / image->description.width;
		// The app's image in the corner is the way back to it.
		ImGui::SetCursorScreenPos({minimum.x + available.x - width - 8, minimum.y + available.y - height - 8});
		preview.DrawSelected(width, m_slice < 0 ? 0 : m_slice);
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImGui::SetItemTooltip("Back to the app's image");
		if (ImGui::IsItemClicked())
			m_spatial.SetExpanded(false);
		ImGui::SetCursorScreenPos(
			{minimum.x + available.x - width - 8, minimum.y + available.y - height - 16 - ImGui::GetFrameHeight()});
		if (IconButton("Back to image", Icon::Close, "Show the app's image again", false, "Back to image"))
			m_spatial.SetExpanded(false);
		ImGui::SetCursorScreenPos({after.x, after.y - ImGui::GetStyle().ItemSpacing.y});
		ImGui::Dummy({0, 0});
		ImGui::EndChild();
		return;
	}
	const bool busy = m_pilot.Active() || m_spatial.Active();
	if (!busy && !m_cornerHovered && !m_fit && !m_crop && ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel)
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
	const float spacing = 12 * ImGui::GetFontSize() / 16;
	// A pinned crop always fits the region it shows.
	const auto crop = m_crop.value_or(ImVec4{0, 0, 1, 1});
	const bool fit = m_fit || m_crop;
	const float shownWidth = description.width * (crop.z - crop.x), shownHeight = description.height * (crop.w - crop.y);
	float width = shownWidth * m_zoom;
	if (fit)
		width = std::max(16.0f, std::min((available.x - spacing * (slices - 1)) / slices,
										 std::max(16.0f, available.y) * shownWidth / shownHeight));
	const float totalWidth = width * slices + spacing * (slices - 1);
	const float height = width * shownHeight / shownWidth;
	if (fit)
		m_zoom = width / shownWidth;
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
	preview.DrawSelected(width, m_slice < 0 ? std::nullopt : std::optional<std::size_t>(m_slice), crop);
	if (m_picking)
		PickCrop(imageMin, width, height, static_cast<int>(slices));
	bool overlayHovered = false;
	// Device markers project onto the whole image, so a crop or a pick hides them.
	if (!m_pilot.Active() && !m_crop && !m_picking && (!view.manual || image->frame == m_spatial.ObservedFrame()))
		for (std::size_t ordinal = 0; ordinal < static_cast<std::size_t>(slices); ++ordinal)
		{
			const auto eye = m_slice < 0 ? ordinal : static_cast<std::size_t>(m_slice);
			if (eye < description.views.size())
			{
				const ImVec2 eyeMin{imageMin.x + ordinal * (width + spacing), imageMin.y};
				overlayHovered |= m_spatial.DrawPreview(shell.selection, eyeMin, {eyeMin.x + width, imageMax.y},
														description.views[eye], static_cast<unsigned>(eye));
			}
		}
	pilotArea = live && view.connected && view.sessionRunning && !view.manual && !m_picking;
	if (m_window)
	{
		auto& input = m_session.ManualInput();
		const bool hudHovered = m_pilot.DrawHud(imageMin, imageMax, input);
		const bool overImage = !hudHovered && !overlayHovered && !m_cornerHovered &&
							   ImGui::IsMouseHoveringRect(imageMin, imageMax);
		m_pilot.Update(*m_window, input, pilotArea, overImage);
		m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
		if (!m_pilot.Error().empty() && m_pilot.Error() != m_pilotError)
			m_session.Notify("Piloting stopped", m_pilot.Error());
		m_pilotError = m_pilot.Error();
	}
	ImGui::EndChild();
}
void PreviewPanel::DrawViewButton(const PreviewImageSummary& image)
{
	const auto zoom = m_fit && !m_crop ? std::string{"Fit"} : std::to_string(static_cast<int>(m_zoom * 100 + .5f)) + "%";
	if (IconButton("View", Icon::Zoom, "Eye, zoom and crop. Wheel zooms, middle-drag pans.", false, zoom.c_str()))
		ImGui::OpenPopup("Image view");
	if (!ImGui::BeginPopup("Image view"))
		return;
	const auto choice = [&](const char* label, bool chosen)
	{
		if (chosen)
			ImGui::PushStyleColor(ImGuiCol_Button, theme::Mix(theme::SurfaceRaised, theme::Accent, .45f));
		const bool pressed = ImGui::Button(label);
		if (chosen)
			ImGui::PopStyleColor();
		return pressed;
	};
	// Two slices with two views are the left and right eye, live or captured.
	const bool eyes = image.slices == 2 && image.description.views.size() == 2;
	if (image.slices > 1)
	{
		SectionLabel("Eye");
		for (int slice = 0; slice < static_cast<int>(image.slices); ++slice)
		{
			const auto label = eyes ? std::string{slice == 0 ? "Left" : "Right"} : "View " + std::to_string(slice + 1);
			if (choice(label.c_str(), m_slice == slice))
				m_slice = slice;
			ImGui::SameLine();
		}
		if (choice(eyes ? "Both" : "All", m_slice == -1))
			m_slice = -1;
	}
	SectionLabel("Zoom");
	ImGui::BeginDisabled(m_crop.has_value());
	if (choice("Fit", m_fit))
		m_fit = true;
	for (const int level : {1, 2, 4})
	{
		ImGui::SameLine();
		const auto label = std::to_string(level) + ":1";
		if (choice(label.c_str(), !m_fit && m_zoom == static_cast<float>(level)))
		{
			m_fit = false;
			m_zoom = static_cast<float>(level);
		}
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
	if (ImGui::SliderFloat("##Zoom", &m_zoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
		m_fit = false;
	ImGui::EndDisabled();
	SectionLabel("Crop");
	if (ImGui::Button(m_crop ? "Pick again" : "Pick region"))
	{
		m_picking = true;
		m_pickStart.reset();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SetItemTooltip("Drag over the image; the region stays while you step and scrub frames");
	if (m_crop)
	{
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
			m_crop.reset();
	}
	ImGui::EndPopup();
}
void PreviewPanel::PickCrop(ImVec2 imageMin, float eyeWidth, float height, int eyes)
{
	const float spacing = 12 * ImGui::GetFontSize() / 16;
	auto* draw = ImGui::GetWindowDrawList();
	const auto mouse = ImGui::GetIO().MousePos;
	const ImVec2 imageMax{imageMin.x + eyes * eyeWidth + (eyes - 1) * spacing, imageMin.y + height};
	draw->AddRectFilled(imageMin, imageMax, theme::Fade(theme::Bg, .35f));
	if (ImGui::IsMouseHoveringRect(imageMin, imageMax))
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		m_picking = false;
		m_pickStart.reset();
		return;
	}
	if (!m_pickStart && ImGui::IsMouseHoveringRect(imageMin, imageMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		m_pickStart = mouse;
	const char* hint = m_pickStart ? "Release to pin this region" : "Drag a region to pin it · Esc cancels";
	const auto size = ImGui::CalcTextSize(hint);
	draw->AddRectFilled({imageMin.x + 8, imageMin.y + 8}, {imageMin.x + 20 + size.x, imageMin.y + 16 + size.y},
						theme::Bg, theme::Radius);
	draw->AddText({imageMin.x + 14, imageMin.y + 12}, theme::Text, hint);
	if (!m_pickStart)
		return;
	const ImVec2 first{std::min(m_pickStart->x, mouse.x), std::min(m_pickStart->y, mouse.y)};
	const ImVec2 last{std::max(m_pickStart->x, mouse.x), std::max(m_pickStart->y, mouse.y)};
	draw->AddRect(first, last, theme::Accent, 0, 0, 2);
	if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		return;
	// The eye under the drag start is the reference; the crop applies to every eye.
	const int eye = std::clamp(static_cast<int>((m_pickStart->x - imageMin.x) / (eyeWidth + spacing)), 0, eyes - 1);
	const float left = imageMin.x + eye * (eyeWidth + spacing);
	const auto normalized = [&](ImVec2 point)
	{
		return ImVec2{std::clamp((point.x - left) / eyeWidth, 0.0f, 1.0f),
					  std::clamp((point.y - imageMin.y) / height, 0.0f, 1.0f)};
	};
	// A pick inside a crop narrows it further, in the whole image's coordinates.
	const auto current = m_crop.value_or(ImVec4{0, 0, 1, 1});
	const auto map = [&](ImVec2 point)
	{
		return ImVec2{current.x + point.x * (current.z - current.x), current.y + point.y * (current.w - current.y)};
	};
	const auto from = map(normalized(first)), to = map(normalized(last));
	if (to.x - from.x > .01f && to.y - from.y > .01f)
		m_crop = ImVec4{from.x, from.y, to.x, to.y};
	m_picking = false;
	m_pickStart.reset();
}
} // namespace simxr::app
