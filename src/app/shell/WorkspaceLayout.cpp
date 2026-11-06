#include "app/UiPch.h"
#include "app/RuntimePanels.h"
#include "app/WorkspacePanels.h"
#include "app/widgets/Controls.h"
#include "app/widgets/DeviceRail.h"
#include "app/widgets/Theme.h"

#include <tuple>

namespace simxr::app
{
static std::pair<std::size_t, int> PageKey(const Selection& selection)
{
	const auto* device = std::get_if<DeviceSel>(&selection);
	return {selection.index(), device ? static_cast<int>(device->page) : -1};
}

void WorkspacePanels::ToggleLibrary()
{
	m_shell.library = !m_shell.library;
	m_previewExpanded = m_previewExpanded && !m_shell.library;
	if (m_shell.library && std::exchange(m_settings.timelineFolded, false))
		m_dirty = true;
}

void WorkspacePanels::ShowInspector()
{
	m_drawer = true;
	if (std::exchange(m_settings.inspectorFolded, false))
		m_dirty = true;
}

void WorkspacePanels::FoldTimeline()
{
	m_settings.timelineFolded = !m_settings.timelineFolded;
	m_dirty = true;
}

void WorkspacePanels::FoldInspector()
{
	m_settings.inspectorFolded = !m_settings.inspectorFolded;
	m_dirty = true;
}

void WorkspacePanels::PickTab(Selection picked, bool overlay)
{
	if (picked.index() == m_shell.selection.index() && PageKey(picked) == PageKey(m_shell.selection))
	{
		if (overlay)
			m_drawer = !m_drawer;
		else
			FoldInspector();
		return;
	}
	m_shell.selection = std::move(picked);
	ShowInspector();
}

void WorkspacePanels::StoreCrop(const std::optional<ImVec4>& crop)
{
	auto stored = std::ranges::find(m_settings.crops, m_workspaceProfile, &PreviewCrop::profile);
	if (!crop)
	{
		if (stored != m_settings.crops.end())
		{
			m_settings.crops.erase(stored);
			m_dirty = true;
		}
		return;
	}
	const PreviewCrop value{m_workspaceProfile, crop->x, crop->y, crop->z, crop->w};
	if (stored != m_settings.crops.end() && *stored == value)
		return;
	if (stored != m_settings.crops.end())
		*stored = value;
	else if (m_settings.crops.size() < 32)
		m_settings.crops.push_back(value);
	m_dirty = true;
}

bool WorkspacePanels::Divider(const char* id, ShellRect strip, bool vertical, float& value, float minimum,
							  float maximum, float defaultValue, bool folded, float foldBelow)
{
	const float scale = m_settings.scale;
	const auto& io = ImGui::GetIO();
	ImGui::PushID(id);
	const auto region = [&](const char* part, ShellRect rect) {
		ImGui::SetCursorPos({rect.x, rect.y});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		ImGui::BeginChild((std::string(id) + " " + part).c_str(), {rect.width, rect.height}, 0,
						  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
							  ImGuiWindowFlags_NoBackground);
		ImGui::PopStyleVar();
		ImGui::InvisibleButton("##Edge", {rect.width, rect.height}, ImGuiButtonFlags_EnableNav);
		const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
		const bool released = ImGui::IsItemDeactivated(), doubleClicked = hovered && ImGui::IsMouseDoubleClicked(0);
		const auto itemMin = ImGui::GetItemRectMin(), itemMax = ImGui::GetItemRectMax();
		auto* draw = ImGui::GetWindowDrawList();
		ImGui::EndChild();
		return std::tuple{hovered, active, released, doubleClicked, itemMin, itemMax, draw};
	};
	const auto [lineHovered, lineActive, lineReleased, lineDoubleClicked, lineMin, lineMax, lineDraw] =
		region("line", strip);
	const ImVec2 tabSize = vertical ? ImVec2{14 * scale, 40 * scale} : ImVec2{40 * scale, 14 * scale};
	const ShellRect tab{strip.x + (strip.width - tabSize.x) / 2, strip.y + (strip.height - tabSize.y) / 2, tabSize.x,
						tabSize.y};
	const auto [tabHovered, tabActive, tabReleased, tabDoubleClicked, tabMin, tabMax, tabDraw] = region("tab", tab);
	bool toggle = false;
	const ImGuiID key = ImGui::GetID("Drag");
	const bool active = lineActive || tabActive;
	if (active)
	{
		// Starting folded, the pointer begins at the fold line so a short drag opens it.
		if (!m_dividerDrag || m_dividerDrag->id != key)
			m_dividerDrag = DividerDrag{key, folded ? foldBelow : value, 0};
		const float delta = (vertical ? io.MouseDelta.x : io.MouseDelta.y) / scale;
		m_dividerDrag->request -= delta;
		m_dividerDrag->travel += std::abs(delta);
		const float request = m_dividerDrag->request;
		if (!folded && request < foldBelow)
			toggle = true;
		else if (folded && request > foldBelow + 40)
		{
			toggle = true;
			value = std::clamp(request, minimum, maximum);
		}
		else if (!folded)
			value = std::clamp(request, minimum, maximum);
		if (!vertical)
			m_settings.timelineResized = true;
		m_dirty = true;
	}
	if (tabReleased && m_dividerDrag && m_dividerDrag->id == key && m_dividerDrag->travel * scale < 3)
		toggle = !toggle;
	if (!active && m_dividerDrag && m_dividerDrag->id == key)
		m_dividerDrag.reset();
	if (lineDoubleClicked || tabDoubleClicked)
	{
		value = defaultValue;
		toggle = folded;
		m_dirty = true;
	}
	const bool hot = lineHovered || tabHovered || active;
	if (hot)
		ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
	const ImVec2 center{(lineMin.x + lineMax.x) / 2, (lineMin.y + lineMax.y) / 2};
	lineDraw->AddLine(vertical ? ImVec2{center.x, lineMin.y} : ImVec2{lineMin.x, center.y},
					  vertical ? ImVec2{center.x, lineMax.y} : ImVec2{lineMax.x, center.y},
					  active ? theme::Accent
					  : hot	 ? theme::Fade(theme::Accent, .6f)
							 : theme::Line,
					  (hot ? 2.0f : 1.0f) * scale);
	tabDraw->AddRectFilled(tabMin, tabMax,
						   tabHovered || active ? theme::Mix(theme::SurfaceRaised, theme::Text, .14f)
												: theme::SurfaceRaised,
						   theme::Radius * scale);
	tabDraw->AddRect(tabMin, tabMax, hot ? theme::Fade(theme::Accent, .7f) : theme::Line, theme::Radius * scale);
	const Icon chevron =
		vertical ? (folded ? Icon::ChevronLeft : Icon::ChevronRight) : (folded ? Icon::ChevronUp : Icon::ChevronDown);
	DrawIcon(tabDraw, chevron, {(tabMin.x + tabMax.x) / 2, (tabMin.y + tabMax.y) / 2}, 12 * scale,
			 tabHovered || active ? theme::Text : theme::TextDim);
	if (tabHovered && !active)
		ImGui::SetTooltip("%s", vertical ? (folded ? "Show the inspector (Ctrl+I). Drag to resize"
												   : "Hide the inspector (Ctrl+I). Drag to resize")
										 : (folded ? "Show the timeline (Ctrl+T). Drag to resize"
												   : "Hide the timeline (Ctrl+T). Drag to resize"));
	else if (lineHovered && !active)
		ImGui::SetTooltip("Drag to resize. Drag past the edge to hide. Double-click resets");
	ImGui::PopID();
	return toggle;
}

void WorkspacePanels::DrawShell(RuntimePanels& runtime)
{
	const auto* viewport = ImGui::GetMainViewport();
	const ImVec2 origin{0, 0};
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
	ImGui::Begin("OpenXR Simulator", nullptr,
				 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
					 ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar();
	const float scale = m_settings.scale, width = ImGui::GetContentRegionAvail().x;
	const float height = ImGui::GetContentRegionAvail().y;
	ImGui::SetCursorPos(origin);
	ImGui::BeginChild("Top bar", {width, 40 * scale}, 0, ImGuiWindowFlags_NoScrollbar);
	// The app tabs sit on this rule; the panels below start on it.
	const auto bar = ImGui::GetWindowPos();
	ImGui::GetWindowDrawList()->AddRectFilled({bar.x, bar.y + 39 * scale},
											  {bar.x + ImGui::GetWindowWidth(), bar.y + 40 * scale}, theme::Line);
	DrawToolbar(runtime);
	ImGui::EndChild();
	const bool compact = width / scale < 1100, overlay = width / scale < 800;
	const bool focus = m_previewExpanded;
	// Narrowing into the drawer layout must not cover the image with the drawer.
	if (overlay && !std::exchange(m_wasOverlay, true))
		m_drawer = false;
	m_wasOverlay = overlay;
	const bool inspectorFolded = m_settings.inspectorFolded && !overlay;
	const bool rail = !focus && (overlay || inspectorFolded);
	const float railWidth = rail ? RailWidth() : 0;
	const float inspector = focus || inspectorFolded
								? 0
								: std::min(std::min(m_settings.inspectorWidth, compact ? 320.0f : 520.0f) * scale,
										   std::max(0.0f, width - railWidth - 120 * scale));
	const float top = origin.y + 40 * scale, bodyHeight = std::max(1.0f, height - 40 * scale);
	const float left = std::max(1.0f, width - railWidth - (overlay ? 0 : inspector));
	const float gap = focus ? 0 : 6 * scale;
	float timelineSize =
		focus ? 0
		: m_settings.timelineFolded
			? TimelinePanel::FoldedHeight(scale)
			: std::min(height * .5f,
					   (!m_settings.timelineResized && height / scale < 560 ? 56 : m_settings.timelineHeight) * scale);
	// The Library drawer needs a few rows; opening it grows a short timeline without saving that height.
	if (m_shell.library && !focus && !m_settings.timelineFolded)
		timelineSize = std::max(timelineSize, std::min(height * .5f, 220 * scale));
	const float previewHeight = std::max(1.0f, bodyHeight - gap - timelineSize);
	const float railX = origin.x + width - railWidth;
	m_shell.preview = {origin.x, top, left, previewHeight};
	m_shell.inspector = {overlay ? railX - inspector : origin.x + left, top, std::max(1.0f, inspector), bodyHeight};
	m_shell.inspectorVisible = !focus && !inspectorFolded && (!overlay || m_drawer);
	if (rail)
	{
		ImGui::SetCursorPos({railX, top});
		ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Bg);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 4 * scale});
		ImGui::BeginChild("Device rail", {railWidth, bodyHeight}, ImGuiChildFlags_AlwaysUseWindowPadding,
						  ImGuiWindowFlags_NoScrollbar);
		ImGui::PopStyleVar();
		if (auto picked = DrawDeviceRail(m_shell.selection, runtime.View(), m_settings))
			PickTab(std::move(*picked), overlay);
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}
	runtime.ConfigurePilot(m_settings.pilotBindings, m_settings.gamepadBindings, m_settings.remoteMouseDrag);
	runtime.ConfigureSnapshots(m_settings.snapshotBudgetMiB);
	runtime.ConfigurePreviews(m_settings.previewBudgetMiB, m_settings.previewRamMiB);
	const auto window = ImGui::GetWindowPos();
	const auto contains = [&](const ShellRect& rect, ImVec2 point) {
		return point.x >= window.x + rect.x && point.x < window.x + rect.x + rect.width &&
			   point.y >= window.y + rect.y && point.y < window.y + rect.y + rect.height;
	};
	const auto selected = m_shell.selection;
	if (overlay && m_drawer && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
		!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
		contains(m_shell.preview, ImGui::GetIO().MousePos) && !contains(m_shell.inspector, ImGui::GetIO().MousePos))
		m_drawer = false;
	runtime.HudFolded() = m_settings.hudFolded;
	if (m_cropProfile != m_workspaceProfile)
	{
		runtime.CancelPreviewInteraction();
		const auto crop = std::ranges::find(m_settings.crops, m_workspaceProfile, &PreviewCrop::profile);
		runtime.PreviewCrop() = crop == m_settings.crops.end()
									? std::nullopt
									: std::optional<ImVec4>(ImVec4{crop->x0, crop->y0, crop->x1, crop->y1});
		m_cropProfile = m_workspaceProfile;
	}
	runtime.DrawPreview(m_shell);
	if (std::exchange(m_shell.addApplication, false))
	{
		m_profiles.Edit(m_settings, -1);
		m_showLaunch = true;
	}
	if (std::exchange(m_settings.hudFolded, runtime.HudFolded()) != m_settings.hudFolded)
		m_dirty = true;
	StoreCrop(runtime.PreviewCrop());
	if (!focus)
	{
		const float timelineTop = std::floor(top + previewHeight + gap / 2);
		ImGui::SetCursorPos({0, timelineTop});
		ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
		ImGui::BeginChild("Timeline", {origin.x + left, viewport->WorkSize.y - timelineTop}, 0,
						  m_settings.timelineFolded ? ImGuiWindowFlags_NoScrollbar : ImGuiWindowFlags_None);
		const bool libraryOpen = m_shell.library;
		if (m_timeline.Draw(runtime, m_shell, m_settings.timelineFolded))
			ShowInspector();
		if (m_shell.library != libraryOpen && m_shell.library && std::exchange(m_settings.timelineFolded, false))
			m_dirty = true;
		ImGui::EndChild();
		ImGui::PopStyleColor();
		DrawLibrary(runtime);
	}
	if (m_shell.selection != selected)
		m_drawer = true;
	if (m_shell.inspectorVisible)
		DrawInspectorPanel(runtime, rail);
	for (auto& notice : runtime.TakeNotices())
		m_notices.Push(std::move(notice));
	m_notices.Draw(
		{window.x + m_shell.preview.x, window.y + m_shell.preview.y, m_shell.preview.width, m_shell.preview.height});
	if (focus)
	{
		// Leave the image controls at the top right clear.
		const char* label = "Exit focus";
		const float chip =
			ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 3;
		ImGui::SetCursorPos({m_shell.preview.x + 8 * scale, m_shell.preview.y + 4 * scale});
		ImGui::BeginChild("Focus exit", {chip, ImGui::GetFrameHeight()}, 0,
						  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
		if (IconButton("Exit focus", Icon::Focus, "Back to the full layout (F11 or Esc)", true, label))
			m_previewExpanded = false;
		ImGui::EndChild();
	}
	else
	{
		if (!overlay && Divider("Inspector divider", {origin.x + left - 3 * scale, top, 6 * scale, bodyHeight}, true,
								m_settings.inspectorWidth, 280, compact ? 320 : 520, 340, inspectorFolded, 200))
			FoldInspector();
		if (Divider("Timeline divider", {0, top + previewHeight, origin.x + left, gap}, false,
					m_settings.timelineHeight, 56, height / scale * .5f, 132, m_settings.timelineFolded, 40))
			FoldTimeline();
	}
	const auto state = m_shell.session.state;
	const ImU32 border = state == RunState::Paused		   ? theme::Paused
						 : !runtime.View().time.following  ? theme::Past
						 : state == RunState::Disconnected ? theme::TextDim
														   : 0;
	if (border)
		ImGui::GetForegroundDrawList()->AddRect(
			viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y}, border, 0, 0,
			2 * scale);
	ImGui::End();
}
} // namespace simxr::app
