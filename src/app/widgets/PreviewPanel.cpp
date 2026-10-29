#include "app/widgets/PreviewPanel.h"
#include "app/UiPch.h"
#include "app/RuntimeView.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/pages/FramePage.h"
#include "app/platform/Window.h"
#include "app/services/RuntimeSession.h"
#include "app/widgets/Controls.h"
#include "app/widgets/PilotController.h"
#include "app/widgets/SpatialEditor.h"
#include "app/widgets/Theme.h"

#include <imgui_internal.h>

namespace simxr::app
{
namespace wire = control;

static std::string ImageLabel(const PreviewImageSummary& image)
{
	// Composited and display output are the normal case and need no label.
	if (image.target == wire::TargetKind::LayerView)
		return "Layer " + std::to_string(image.layer + 1) + ", view " + std::to_string(image.view + 1);
	if (image.target == wire::TargetKind::ComposedEyes || image.target == wire::TargetKind::VirtualDisplay)
		return "";
	return "Captured image";
}

static std::string ZoomLabel(bool fit, bool cropped, float zoom)
{
	return fit && !cropped ? std::string{"Fit"} : std::to_string(static_cast<int>(zoom * 100 + .5f)) + "%";
}

static bool SameRect(ImVec4 first, ImVec4 second)
{
	return first.x == second.x && first.y == second.y && first.z == second.z && first.w == second.w;
}

PreviewPanel::PreviewPanel(RuntimeSession& session, PilotController& pilot, SpatialEditor& spatial, FramePage& frame,
						   DeviceControlPanel& devices, Window* window)
	: m_session(session), m_pilot(pilot), m_spatial(spatial), m_frame(frame), m_devices(devices), m_window(window)
{}

bool PreviewPanel::Draw(ShellState& shell, const RuntimeView& view, bool inputAvailable, bool automaticGamepad)
{
	const int frame = ImGui::GetFrameCount();
	if (m_lastDraw != frame - 1 || !view.connected || (m_window && !m_window->Focused()))
		CancelInteraction();
	m_lastDraw = frame;
	bool pilotArea = false;
	ImGui::SetCursorPos({shell.preview.x, shell.preview.y});
	if (ImGui::BeginChild("GPU preview", {shell.preview.width, shell.preview.height}))
	{
		const auto image = m_session.Preview().Selected();
		const bool live = view.time.following;
		if (!view.connected || (m_lastImage && m_lastImage->scope.epoch != view.time.epoch))
			m_lastImage.reset();
		if (image && image->ready && !image->released)
		{
			m_lastImage = image;
			if ((m_picking || m_panning) && !SameGestureTarget(*image))
				CancelInteraction();
			const auto area = DrawImage(shell, view, inputAvailable);
			const bool cornerHovered = DrawCorner(*image, live, area.has_value());
			if (area)
			{
				const bool wasPicking = m_picking;
				if ((m_picking || m_panning) && !SameGestureTarget(*image))
					CancelInteraction();
				if (m_picking)
					PickCrop(*area, area->hovered && !cornerHovered);
				pilotArea = inputAvailable && live && view.connected && view.sessionRunning && !view.manual &&
							!wasPicking && !m_picking;
				UpdateImage(*area, *image, cornerHovered, pilotArea, automaticGamepad);
			}
		}
		else
		{
			CancelInteraction();
			if (ImGui::BeginChild("Image area", {0, 0}, 0, ImGuiWindowFlags_NoScrollWithMouse))
			{
				if (std::holds_alternative<FrameSel>(shell.selection) && m_frame.Expanded())
					m_frame.DrawExpanded(ImGui::GetContentRegionAvail());
				else if (!std::holds_alternative<FrameSel>(shell.selection) && m_spatial.Expanded())
					m_spatial.DrawExpanded(shell.selection, ImGui::GetContentRegionAvail());
				else if (!DrawKept(view))
					DrawEmpty(shell, view);
			}
			ImGui::EndChild();
			if (m_lastImage)
				DrawCorner(*m_lastImage, live, false);
		}
	}
	else
		CancelInteraction();
	ImGui::EndChild();
	return pilotArea;
}

void PreviewPanel::CancelInteraction()
{
	m_panning = false;
	m_picking = false;
	m_pickStart.reset();
	m_gestureImage.reset();
}

std::optional<PreviewPanel::ImageArea> PreviewPanel::DrawImage(ShellState& shell, const RuntimeView& view,
															   bool inputAvailable)
{
	auto& preview = m_session.Preview();
	const auto image = preview.Selected();
	if (m_slice >= static_cast<int>(image->slices))
		m_slice = 0;
	if (!ImGui::BeginChild("Image area", {0, 0}, 0, ImGuiWindowFlags_NoScrollWithMouse))
	{
		CancelInteraction();
		ImGui::EndChild();
		return {};
	}
	const bool recorded = std::holds_alternative<FrameSel>(shell.selection);
	if (recorded ? m_frame.Expanded() : m_spatial.Expanded())
	{
		CancelInteraction();
		const auto minimum = ImGui::GetCursorScreenPos();
		const auto available = ImGui::GetContentRegionAvail();
		if (recorded)
			m_frame.DrawExpanded(available);
		else
			m_spatial.DrawExpanded(shell.selection, available);
		const auto after = ImGui::GetCursorScreenPos();
		const float width = available.x * .25f;
		const float height = width * PreviewAspect(image->description, m_slice < 0 ? 0 : m_slice);
		const float buttonHeight = ImGui::GetFrameHeight();
		const float overlayWidth =
			std::max(width, buttonHeight + ImGui::CalcTextSize("Back to image").x + ImGui::GetStyle().FramePadding.x);
		const float overlayHeight = height + buttonHeight + 8;
		// A child window keeps return controls above the rig's full-area orbit target.
		ImGui::SetCursorScreenPos(
			{minimum.x + available.x - overlayWidth - 8, minimum.y + available.y - overlayHeight - 8});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		bool close = false;
		if (ImGui::BeginChild("Return to image", {overlayWidth, overlayHeight}, 0,
							  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
								  ImGuiWindowFlags_NoBackground))
		{
			close = IconButton("Back to image", Icon::Close, "Show the app's image again", false, "Back to image");
			ImGui::SetCursorPos({0, buttonHeight + 8});
			preview.DrawSelected(width, m_slice < 0 ? 0 : m_slice);
			if (ImGui::IsItemHovered())
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::SetItemTooltip("Back to the app's image");
			close |= ImGui::IsItemClicked();
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
		if (close)
		{
			if (recorded)
				m_frame.SetExpanded(false);
			else
				m_spatial.SetExpanded(false);
		}
		ImGui::SetCursorScreenPos({after.x, after.y - ImGui::GetStyle().ItemSpacing.y});
		ImGui::Dummy({0, 0});
		ImGui::EndChild();
		return {};
	}
	const auto available = ImGui::GetContentRegionAvail();
	const auto& description = image->description;
	const float slices = m_slice < 0 ? static_cast<float>(image->slices) : 1;
	const float spacing = 12 * ImGui::GetFontSize() / 16;
	const auto crop = m_crop.value_or(ImVec4{0, 0, 1, 1});
	const bool fit = m_fit || m_crop;
	const auto selectedSlice = m_slice < 0 ? std::nullopt : std::optional<std::size_t>(m_slice);
	const float aspect = PreviewAspect(description, selectedSlice, fit);
	const float shownWidth = description.width * (crop.z - crop.x),
				shownHeight = description.width * aspect * (crop.w - crop.y);
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
	preview.DrawSelected(width, selectedSlice, crop, fit);
	bool overlayHovered = false;
	// Device markers project onto the whole image, so a crop or a pick hides them.
	if (!m_pilot.Active() && !m_crop && !m_picking && (!view.manual || image->frame == m_spatial.ObservedFrame()))
		for (std::size_t ordinal = 0; ordinal < static_cast<std::size_t>(slices); ++ordinal)
		{
			const auto eye = m_slice < 0 ? ordinal : static_cast<std::size_t>(m_slice);
			if (eye < description.views.size())
			{
				const ImVec2 eyeMin{imageMin.x + ordinal * (width + spacing), imageMin.y};
				overlayHovered |=
					m_spatial.DrawPreview(shell.selection, eyeMin,
										  {eyeMin.x + width, eyeMin.y + width * PreviewAspect(description, eye, fit)},
										  description.views[eye], static_cast<unsigned>(eye));
			}
		}
	bool hudHovered = false;
	if (m_window)
	{
		ImGui::BeginDisabled(!inputAvailable);
		hudHovered = m_pilot.DrawHud(imageMin, imageMax, m_session.ManualInput());
		ImGui::EndDisabled();
	}
	const ImageArea area{ImGui::GetCurrentWindow(),
						 imageMin,
						 imageMax,
						 width,
						 static_cast<int>(slices),
						 !hudHovered && !overlayHovered && ImGui::IsWindowHovered() &&
							 ImGui::IsMouseHoveringRect(imageMin, imageMax)};
	ImGui::EndChild();
	return area;
}

void PreviewPanel::UpdateImage(const ImageArea& area, const PreviewImageSummary& image, bool cornerHovered,
							   bool pilotArea, bool automaticGamepad)
{
	const bool hovered = area.hovered && !cornerHovered;
	const bool busy = m_pilot.Active() || m_spatial.Active() || m_picking;
	if (!busy && hovered && !m_fit && !m_crop && ImGui::GetIO().MouseWheel)
		m_zoom = std::clamp(m_zoom * std::pow(1.15f, ImGui::GetIO().MouseWheel), .05f, 16.0f);
	if (!busy && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && !m_fit && !m_crop)
	{
		m_panning = true;
		m_gestureImage = image;
		m_gestureSlice = m_slice;
		m_gestureCrop = m_crop.value_or(ImVec4{0, 0, 1, 1});
	}
	if (m_panning && (busy || !ImGui::IsMouseDown(ImGuiMouseButton_Middle) || m_fit || m_crop))
		CancelInteraction();
	if (m_panning)
	{
		ImGui::SetScrollX(area.window, area.window->Scroll.x - ImGui::GetIO().MouseDelta.x);
		ImGui::SetScrollY(area.window, area.window->Scroll.y - ImGui::GetIO().MouseDelta.y);
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	}
	if (m_window)
	{
		m_pilot.Update(*m_window, m_session.ManualInput(), pilotArea, hovered, automaticGamepad);
		m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active() || m_devices.Active());
		if (!m_pilot.Error().empty() && m_pilot.Error() != m_pilotError)
			m_session.Notify("Piloting stopped", m_pilot.Error());
		m_pilotError = m_pilot.Error();
	}
}

bool PreviewPanel::DrawCorner(const PreviewImageSummary& image, bool live, bool imageVisible)
{
	const float scale = ImGui::GetFontSize() / 17, margin = 8 * scale;
	const ImVec2 padding{4 * scale, 4 * scale};
	const auto origin = ImGui::GetWindowPos();
	const auto& style = ImGui::GetStyle();
	const float height = ImGui::GetFrameHeight();
	const float availableWidth = std::max(1.0f, std::floor(ImGui::GetWindowWidth() - margin * 2));
	const float contentWidth = std::max(1.0f, availableWidth - padding.x * 2);
	const auto zoom = ZoomLabel(m_fit, m_crop.has_value(), m_zoom);
	const float labelWidth = height + ImGui::CalcTextSize(zoom.c_str()).x + style.FramePadding.x;
	const bool compact = labelWidth + height + style.ItemSpacing.x > contentWidth;
	const float viewWidth = compact ? height : labelWidth;
	const float cropWidth = height + ImGui::CalcTextSize("Reset crop").x + style.FramePadding.x;
	const bool showCrop = m_crop && viewWidth + cropWidth + height + style.ItemSpacing.x * 2 <= contentWidth;
	const float controlsWidth =
		viewWidth + height + style.ItemSpacing.x + (showCrop ? cropWidth + style.ItemSpacing.x : 0);
	const float rowHeight = height;
	const float primaryHeight = std::ceil(padding.y * 2 + rowHeight);
	std::string header = live ? "" : "PAST";
	if (const auto label = ImageLabel(image); !label.empty())
		header += (header.empty() ? "" : ", ") + label;
	const float headerHeight = header.empty() ? 0 : ImGui::GetFontSize() + style.ItemSpacing.y;
	const bool showHeader = headerHeight && primaryHeight + headerHeight <= ImGui::GetWindowHeight();
	const float panelHeight = std::min(primaryHeight + (showHeader ? headerHeight : 0), ImGui::GetWindowHeight());
	const float verticalMargin = std::min(margin, std::max(0.0f, (ImGui::GetWindowHeight() - panelHeight) / 2));
	const float width = std::min(
		availableWidth,
		std::ceil(std::max(controlsWidth, showHeader ? ImGui::CalcTextSize(header.c_str()).x : 0) + padding.x * 2));
	ImGui::SetCursorScreenPos({origin.x + ImGui::GetWindowWidth() - width - margin, origin.y + verticalMargin});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Fade(theme::Bg, .72f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::Radius * scale);
	// A scrollbar must not steal width and push the primary controls onto a clipped row.
	ImGui::SetNextWindowScroll({0, 0});
	if (ImGui::BeginChild("Image controls", {width, panelHeight}, ImGuiChildFlags_AlwaysUseWindowPadding,
						  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		if (showHeader)
		{
			const auto minimum = ImGui::GetCursorScreenPos();
			const ImVec2 maximum{minimum.x + width - padding.x * 2, minimum.y + ImGui::GetFontSize()};
			auto* draw = ImGui::GetWindowDrawList();
			draw->PushClipRect(minimum, maximum, true);
			draw->AddText(minimum, live ? theme::TextDim : theme::Past, header.c_str());
			draw->PopClipRect();
			ImGui::Dummy({maximum.x - minimum.x, ImGui::GetFontSize()});
			ImGui::SetItemTooltip("%s", header.c_str());
		}
		DrawViewButton(image, imageVisible, compact);
		if (showCrop)
		{
			ImGui::SameLine();
			if (IconButton("Reset crop", Icon::Crop, "Show the whole image", false, "Reset crop"))
			{
				CancelInteraction();
				m_crop.reset();
			}
		}
		ImGui::SameLine();
		m_spatial.DrawSpaceToggle();
	}
	const auto position = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
	const bool hovered = ImGui::IsMouseHoveringRect(position, {position.x + size.x, position.y + size.y}, false);
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	return hovered;
}

void PreviewPanel::DrawViewButton(const PreviewImageSummary& image, bool imageVisible, bool compact)
{
	const auto zoom = ZoomLabel(m_fit, m_crop.has_value(), m_zoom);
	if (IconButton("View", Icon::Zoom, "Eye, zoom and crop. Wheel zooms, middle-drag pans.", false,
				   compact ? nullptr : zoom.c_str()))
		ImGui::OpenPopup("Image view");
	if (!ImGui::BeginPopup("Image view"))
		return;
	const auto choice = [&](const char* label, bool chosen) {
		if (chosen)
			ImGui::PushStyleColor(ImGuiCol_Button, theme::Mix(theme::SurfaceRaised, theme::Accent, .45f));
		const bool pressed = ImGui::Button(label);
		if (pressed)
			CancelInteraction();
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
	ImGui::SetItemTooltip(
		"Fit preserves proportions using the recorded field of view. Other zoom levels show image pixels.");
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
	{
		CancelInteraction();
		m_fit = false;
	}
	ImGui::EndDisabled();
	SectionLabel("Crop");
	ImGui::BeginDisabled(!imageVisible);
	if (ImGui::Button(m_crop ? "Pick again" : "Pick region"))
	{
		CancelInteraction();
		m_picking = true;
		m_gestureImage = image;
		m_gestureSlice = m_slice;
		m_gestureCrop = m_crop.value_or(ImVec4{0, 0, 1, 1});
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", imageVisible ? "Drag over the image. The region stays while you step and scrub frames"
											 : "Return to the image to pick a region");
	if (m_crop)
	{
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
		{
			CancelInteraction();
			m_crop.reset();
		}
	}
	ImGui::EndPopup();
}

void PreviewPanel::PickCrop(const ImageArea& area, bool hovered)
{
	const auto imageMin = area.minimum, imageMax = area.maximum;
	const float eyeWidth = area.eyeWidth;
	const ImVec4 bounds{imageMin.x, imageMin.y, imageMax.x, imageMax.y};
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
		(m_pickStart && (!SameRect(bounds, m_pickBounds) || (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
															 !ImGui::IsMouseReleased(ImGuiMouseButton_Left)))))
	{
		CancelInteraction();
		return;
	}
	const auto mouse = ImGui::GetIO().MousePos;
	if (hovered)
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	if (!m_pickStart && hovered && !ImGui::IsAnyItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_pickStart = mouse;
		m_pickBounds = bounds;
	}
	auto* draw = area.window->DrawList;
	draw->PushClipRect(area.window->InnerClipRect.Min, area.window->InnerClipRect.Max, false);
	draw->AddRectFilled(imageMin, imageMax, theme::Fade(theme::Bg, .35f));
	const char* hint = m_pickStart ? "Release to pin this region" : "Drag to select a region. Esc cancels.";
	const float inset = 8 * ImGui::GetFontSize() / 17;
	const float hintWidth = std::max(1.0f, std::min(imageMax.x, area.window->InnerClipRect.Max.x) -
											   std::max(imageMin.x, area.window->InnerClipRect.Min.x) - inset * 2);
	const auto size = ImGui::CalcTextSize(hint, nullptr, false, hintWidth);
	const ImVec2 hintMin{std::max(imageMin.x, area.window->InnerClipRect.Min.x) + inset,
						 std::max(imageMin.y, area.window->InnerClipRect.Min.y) + inset};
	draw->AddRectFilled({hintMin.x - inset / 2, hintMin.y - inset / 2},
						{hintMin.x + size.x + inset / 2, hintMin.y + size.y + inset / 2}, theme::Bg, theme::Radius);
	draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), hintMin, theme::Text, hint, nullptr, hintWidth);
	if (m_pickStart)
	{
		const ImVec2 first{std::min(m_pickStart->x, mouse.x), std::min(m_pickStart->y, mouse.y)};
		const ImVec2 last{std::max(m_pickStart->x, mouse.x), std::max(m_pickStart->y, mouse.y)};
		draw->AddRect(first, last, theme::Accent, 0, 0, 2);
	}
	draw->PopClipRect();
	if (!m_pickStart || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		return;
	const float spacing = 12 * ImGui::GetFontSize() / 16;
	const int eye =
		std::clamp(static_cast<int>((m_pickStart->x - imageMin.x) / (eyeWidth + spacing)), 0, area.eyes - 1);
	const float left = imageMin.x + eye * (eyeWidth + spacing);
	const auto slice = m_gestureSlice < 0 ? eye : m_gestureSlice;
	const float height = eyeWidth * PreviewAspect(m_gestureImage->description, slice, m_fit || m_crop) *
						 (m_gestureCrop.w - m_gestureCrop.y) / (m_gestureCrop.z - m_gestureCrop.x);
	if (m_pickStart->x > left + eyeWidth || m_pickStart->y > imageMin.y + height)
	{
		CancelInteraction();
		return;
	}
	const auto normalized = [&](ImVec2 point) {
		return ImVec2{std::clamp((point.x - left) / eyeWidth, 0.0f, 1.0f),
					  std::clamp((point.y - imageMin.y) / height, 0.0f, 1.0f)};
	};
	const auto from = normalized(*m_pickStart), to = normalized(mouse);
	const ImVec4 picked{m_gestureCrop.x + std::min(from.x, to.x) * (m_gestureCrop.z - m_gestureCrop.x),
						m_gestureCrop.y + std::min(from.y, to.y) * (m_gestureCrop.w - m_gestureCrop.y),
						m_gestureCrop.x + std::max(from.x, to.x) * (m_gestureCrop.z - m_gestureCrop.x),
						m_gestureCrop.y + std::max(from.y, to.y) * (m_gestureCrop.w - m_gestureCrop.y)};
	if (picked.z - picked.x > .01f && picked.w - picked.y > .01f)
		m_crop = picked;
	CancelInteraction();
}

bool PreviewPanel::SameGestureTarget(const PreviewImageSummary& image) const
{
	return m_gestureImage && m_gestureImage->scope == image.scope && m_gestureImage->live == image.live &&
		   (image.live || m_gestureImage->id == image.id) && m_gestureImage->target == image.target &&
		   m_gestureImage->layer == image.layer && m_gestureImage->view == image.view &&
		   m_gestureImage->slices == image.slices && m_gestureImage->description.width == image.description.width &&
		   m_gestureImage->description.height == image.description.height && m_gestureSlice == m_slice &&
		   SameRect(m_gestureCrop, m_crop.value_or(ImVec4{0, 0, 1, 1}));
}

void PreviewPanel::DrawEmpty(ShellState& shell, const RuntimeView& view)
{
	const auto available = ImGui::GetContentRegionAvail();
	const auto origin = ImGui::GetCursorScreenPos();
	const float unit = ImGui::GetFontSize() / 17;
	const ImVec2 center{origin.x + available.x / 2, origin.y + available.y / 2};
	auto* draw = ImGui::GetWindowDrawList();
	const auto line = [&](const char* text, float y, ImU32 color) {
		const auto size = ImGui::CalcTextSize(text);
		draw->AddText({center.x - size.x / 2, y}, color, text);
	};
	if (!view.connected)
	{
		DrawIcon(draw, Icon::App, {center.x, center.y - 72 * unit}, 40 * unit, theme::TextDim);
		line("OpenXR Simulator", center.y - 44 * unit, theme::Text);
		line("Launch or connect an OpenXR app", center.y - 14 * unit, theme::Text);
		const auto& style = ImGui::GetStyle();
		const float addWidth = ImGui::CalcTextSize("Add application").x + style.FramePadding.x * 2;
		const float connectWidth = ImGui::CalcTextSize("Connect").x + style.FramePadding.x * 2;
		const bool stacked = addWidth + style.ItemSpacing.x + connectWidth > available.x;
		const float width = stacked ? std::max(addWidth, connectWidth) : addWidth + style.ItemSpacing.x + connectWidth;
		ImGui::SetCursorScreenPos({center.x - width / 2, center.y + 16 * unit});
		if (ImGui::Button("Add application"))
			shell.addApplication = true;
		ImGui::SetItemTooltip("Choose an application to launch with OpenXR Simulator");
		if (!stacked)
			ImGui::SameLine();
		else
			ImGui::SetCursorScreenPos({center.x - connectWidth / 2, ImGui::GetCursorScreenPos().y});
		if (ImGui::Button("Connect"))
			shell.openAppPicker = true;
		ImGui::SetItemTooltip("Find or connect to an application that is already running");
		return;
	}
	if (!view.time.following)
	{
		const auto text = "Frame " + std::to_string(view.time.cursor.frame);
		line(text.c_str(), center.y - 20 * unit, theme::Text);
		line(m_session.LoadingPastImage() ? "Loading frame image..." : "Image not kept", center.y + 4 * unit,
			 theme::TextDim);
		return;
	}
	const float angle = static_cast<float>(ImGui::GetTime()) * 6;
	draw->PathArcTo({center.x, center.y - 24 * unit}, 10 * unit, angle, angle + 4.4f);
	draw->PathStroke(theme::TextDim, ImDrawFlags_None, 2 * unit);
	line(view.sessionAvailable ? "Waiting for first frame\u2026" : "Waiting for the app to start XR\u2026", center.y,
		 theme::TextDim);
}

bool PreviewPanel::DrawKept(const RuntimeView& view)
{
	if (view.time.following || !view.connected || m_slice != 0 || (!m_fit && !m_crop))
		return false;
	auto& history = m_session.Preview().History();
	const auto shot = history.Nearest(view.time.cursor, view.time.epoch);
	if (!shot || shot->frame != view.time.cursor || shot->epoch != view.time.epoch)
		return false;
	const auto crop = m_crop.value_or(ImVec4{0, 0, 1, 1});
	const auto available = ImGui::GetContentRegionAvail();
	const float aspect = shot->aspect * (crop.w - crop.y) / (crop.z - crop.x);
	const float width = std::max(16.f, std::min(available.x, available.y / aspect));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (available.x - width) / 2));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (available.y - width * aspect) / 2));
	history.Draw(*shot, width, crop);
	const auto origin = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddText({origin.x + 8, origin.y + 8}, theme::TextDim, "Preview");
	return true;
}
} // namespace simxr::app
