#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/WorkspacePanels.h"
#include "app/widgets/Controls.h"
#include "app/widgets/DeviceRail.h"
#include "app/widgets/Theme.h"

#include <imgui.h>

namespace simxr::app
{
void WorkspacePanels::DrawInspectorPanel(RuntimePanels& runtime, bool rail)
{
	const float scale = m_settings.scale;
	auto area = m_shell.inspector;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
	if (!rail)
	{
		const float strip = DeviceTabsHeight() + 6 * scale;
		ImGui::SetCursorPos({area.x, area.y});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * scale, 4 * scale});
		ImGui::BeginChild("Device tabs", {area.width, strip}, ImGuiChildFlags_AlwaysUseWindowPadding,
						  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		if (auto picked = DrawDeviceTabs(m_shell.selection, runtime.View(), m_settings))
			PickTab(std::move(*picked), false);
		ImGui::EndChild();
		area.y += strip;
		area.height = std::max(1.0f, area.height - strip);
	}
	ImGui::SetCursorPos({area.x, area.y});
	// Pages keep a margin from the edges.
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12 * scale, 8 * scale});
	ImGui::BeginChild("Inspector", {area.width, area.height}, ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	DrawInspector(runtime);
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void WorkspacePanels::DrawLibrary(RuntimePanels& runtime)
{
	const auto rect = m_timeline.DrawerRect();
	if (!m_shell.library || rect.width <= 0 || rect.height <= 0)
		return;
	const float scale = m_settings.scale;
	ImGui::SetCursorScreenPos({rect.x, rect.y});
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * scale, 4 * scale});
	ImGui::BeginChild("Library", {rect.width, rect.height}, ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
#ifdef SIMXR_COMPANION_AUTOMATION
	const auto before = m_shell.selection;
	if (auto run = m_library.DrawBrowser(m_shell.selection, runtime, m_rigs))
		QueueScript(runtime, std::move(*run));
	if (m_shell.selection != before)
		ShowInspector();
#else
	(void)runtime;
	ImGui::TextDisabled("Takes and scripts need a Luau-enabled build");
#endif
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void WorkspacePanels::DrawInspector(RuntimePanels& runtime)
{
	if (std::holds_alternative<DeviceSel>(m_shell.selection))
		runtime.DrawDevicePage(m_shell);
	else if (std::holds_alternative<FrameSel>(m_shell.selection))
		runtime.DrawFramePage(m_shell);
	else if (const auto range = std::get_if<RangeSel>(&m_shell.selection))
		runtime.DrawRangePage(RangeSel{*range}, m_shell);
	else if (std::holds_alternative<AdvancedSel>(m_shell.selection))
	{
		PageTitle(Icon::StepForward, "Advanced stepping");
		if (PageClose())
			m_shell.selection = std::monostate{};
		else
			runtime.DrawAdvancedPage();
	}
	else if (std::holds_alternative<TakeSel>(m_shell.selection) || std::holds_alternative<ScriptSel>(m_shell.selection))
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		if (auto run = m_library.Draw(m_shell.selection, runtime, m_rigs))
			QueueScript(runtime, std::move(*run));
#else
		ImGui::TextDisabled("Takes and scripts need a Luau-enabled build");
#endif
	}
	// A page's × (or its file deleted) returns to the device page it was opened from.
	if (std::holds_alternative<std::monostate>(m_shell.selection))
		m_shell.selection = m_shell.device;
}
} // namespace simxr::app
