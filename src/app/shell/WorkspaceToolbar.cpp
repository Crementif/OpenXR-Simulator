#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/WorkspacePanels.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"

#include <imgui.h>

namespace simxr::app
{
void WorkspacePanels::DrawAppPicker(RuntimePanels& runtime, const RuntimeView& view, float maximumWidth, bool compact)
{
	const auto state = m_shell.session.state;
	const auto slash = view.endpoint.find_last_of("/\\");
	const std::string label = !view.connected ? (!m_workspaceProfile.empty() ? m_workspaceProfile : "Apps")
							  : view.endpoint == m_launchedEndpoint && !m_launchedName.empty()	 ? m_launchedName
							  : view.endpoint == m_connectedEndpoint && !m_connectedName.empty() ? m_connectedName
							  : !view.appName.empty()
								  ? view.appName
								  : view.endpoint.substr(slash == std::string::npos ? 0 : slash + 1);
	const ImU32 dot = state == RunState::Running											 ? theme::Ok
					  : state == RunState::Paused											 ? theme::Paused
					  : state == RunState::Failed											 ? theme::Record
					  : state == RunState::Launching || state == RunState::WaitingForSession ? theme::Warn
					  : state == RunState::Disconnected										 ? theme::TextDim
																							 : 0;
	std::string tooltip = std::string(RunStateLabel(state)) + ", " + label;
	if (view.connected)
		tooltip += "\n" + view.endpoint;
	tooltip += "\nLaunch, connect or switch apps";
	const auto session = [&]
	{
		runtime.DrawSessionMenu();
		ImGui::Separator();
		if (ImGui::MenuItem("Release inputs"))
			runtime.ReleaseInputs();
		ImGui::SetItemTooltip("Hand every device back to the app's own input");
		if (ImGui::MenuItem("Cancel pending", "Shift+F7"))
			runtime.CancelPending();
	};
	auto choice = m_picker.Draw(m_settings, view, label, dot, tooltip,
									  std::exchange(m_shell.openAppPicker, false), session, maximumWidth, compact);
	if (choice.connect && m_openRunning && m_openRunning(*choice.connect))
		choice.connect.reset();
	if (!choice.discoveryError.empty())
		m_notices.Push({"Application discovery failed", choice.discoveryError});
	if (choice.connect || choice.disconnect)
	{
		CancelRestart();
		m_attachment.reset();
		m_launchError.clear();
		m_launchedEndpoint.clear();
		m_launchedName.clear();
		m_connectedEndpoint.clear();
		m_connectedName.clear();
#ifdef SIMXR_COMPANION_AUTOMATION
		m_rigs.PrepareLaunch({});
#endif
	}
	if (choice.connect)
		ConnectApp(runtime, *choice.connect);
	if (choice.launchProfile)
	{
		const auto index = *choice.launchProfile;
		m_profiles.Edit(m_settings, index);
		m_settings.activeProfile = m_settings.profiles[static_cast<std::size_t>(index)].name;
		Launch(m_settings.profiles[static_cast<std::size_t>(index)]);
	}
	if (choice.launchRecent)
		Launch(*choice.launchRecent);
	if (choice.saveRecent && *choice.saveRecent < m_settings.recent.size())
	{
		if (m_profiles.Create(m_settings, m_settings.recent[*choice.saveRecent].launch))
			Save();
		m_showLaunch = true;
	}
	if (choice.addProfile)
	{
		m_profiles.Edit(m_settings, -1);
		m_showLaunch = true;
	}
	if (choice.editProfiles)
	{
		if (m_profiles.Selected() < 0)
			m_profiles.Edit(m_settings, 0);
		m_showLaunch = true;
	}
	if (choice.connectEndpoint)
		m_showConnection = true;
	if (choice.disconnect)
		runtime.Disconnect();
}

void WorkspacePanels::ConnectApp(RuntimePanels& runtime, const RunningApp& app)
{
	m_workspaceProfile.clear();
	m_connectedEndpoint = app.endpoint;
	m_connectedName = app.name;
	runtime.ConnectEndpoint(app.endpoint);
	if (auto launch = DescribeProcess(app.process))
		Remember(std::move(*launch));
}

void WorkspacePanels::DrawHistory(RuntimePanels& runtime)
{
	const bool unseen = m_notices.Unseen();
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{});
	const bool pressed = IconButton("History", Icon::History, "Recent changes and notices",
									ImGui::IsPopupOpen("History"));
	ImGui::PopStyleColor();
	if (pressed)
	{
		ImGui::OpenPopup("History");
		m_notices.MarkSeen();
	}
	if (unseen)
	{
		// A dot while a notice arrived that the history has not shown yet.
		const auto corner = ImGui::GetItemRectMax();
		const float radius = ImGui::GetFrameHeight() * .12f;
		ImGui::GetWindowDrawList()->AddCircleFilled(
			{corner.x - radius * 1.6f, ImGui::GetItemRectMin().y + radius * 1.6f}, radius, theme::Warn);
	}
	const float scale = m_settings.scale;
	ImGui::SetNextWindowSize({300 * scale, 0});
	if (!ImGui::BeginPopup("History"))
		return;
	const auto now = std::chrono::steady_clock::now();
	const auto ago = [&](std::chrono::steady_clock::time_point time)
	{
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - time).count();
		if (seconds < 5)
			return std::string("now");
		if (seconds < 60)
			return std::to_string(seconds) + " s";
		if (seconds < 3600)
			return std::to_string(seconds / 60) + " min";
		return std::to_string(seconds / 3600) + " h";
	};
	const auto time = [&](const std::string& text)
	{
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(text.c_str()).x);
		ImGui::TextDisabled("%s", text.c_str());
	};
	constexpr std::size_t shown = 12;
	SectionLabel("Changes");
	const auto changes = runtime.Changes();
	if (changes.empty())
		ImGui::TextDisabled("No changes yet");
	for (std::size_t index = changes.size(); index-- > 0 && changes.size() - index <= shown;)
	{
		const std::size_t steps = changes.size() - index;
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::Selectable(changes[index].label.c_str()))
			runtime.UndoChanges(steps);
		if (steps == 1)
			ImGui::SetItemTooltip("Undo this change (Ctrl+Z)");
		else
			ImGui::SetItemTooltip("Undo this and the %zu later changes", steps - 1);
		time(ago(changes[index].time));
		ImGui::PopID();
	}
	SectionLabel("Notices");
	const auto& logged = m_notices.Log();
	if (logged.empty())
		ImGui::TextDisabled("No notices");
	for (std::size_t index = logged.size(); index-- > 0 && logged.size() - index <= shown;)
	{
		const auto& entry = logged[index];
		ImGui::PushID(static_cast<int>(index));
		ImGui::TextColored(theme::Vec(theme::Warn), "!");
		ImGui::SameLine();
		ImGui::TextUnformatted(entry.title.c_str());
		if (!entry.detail.empty() && ImGui::BeginItemTooltip())
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
			ImGui::TextUnformatted(entry.detail.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		time(ago(entry.time));
		ImGui::PopID();
	}
	ImGui::EndPopup();
}

void WorkspacePanels::DrawToolbar(RuntimePanels& runtime)
{
	const auto& snapshot = runtime.View();
	m_shell.session = DeriveRunState(snapshot, m_attachment != nullptr, m_launchError);
	if (std::holds_alternative<DeviceSel>(m_shell.selection))
		m_shell.device = m_shell.selection;
	if (const auto stepped = runtime.TakeSteppedFrame();
		stepped && (stepped->requested || std::holds_alternative<FrameSel>(m_shell.selection)))
	{
		m_shell.selection = FrameSel{stepped->frame, stepped->epoch};
		m_drawer = m_drawer || stepped->requested;
	}

	if (!snapshot.recording)
		m_shell.timeline.recordingSince.reset();
	else if (!m_shell.timeline.recordingSince)
		m_shell.timeline.recordingSince = std::chrono::steady_clock::now();
	// Focus hides the timeline, so the Focus button carries a running recording's time.
	std::string elapsed;
	if (m_previewExpanded && m_shell.timeline.recordingSince)
	{
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
																			  *m_shell.timeline.recordingSince)
								 .count();
		elapsed = std::to_string(seconds / 60) + ":";
		if (seconds % 60 < 10)
			elapsed += '0';
		elapsed += std::to_string(seconds % 60);
	}
	const float right = ImGui::GetContentRegionMax().x;
	const auto& style = ImGui::GetStyle();
	const float button = ImGui::GetFrameHeight();
	const float focus =
		elapsed.empty() ? button : button + ImGui::CalcTextSize(elapsed.c_str()).x + style.FramePadding.x;
	const float actions = button * 2 + focus + style.ItemSpacing.x * 2;
	const float reserved =
		actions + style.ItemSpacing.x + (m_shell.session.state == RunState::Failed ? button + style.ItemSpacing.x : 0);
	ImGui::SetCursorPosY(std::max(0.0f, (ImGui::GetWindowHeight() - button) / 2));
	DrawAppPicker(runtime, snapshot, std::max(1.f, ImGui::GetContentRegionAvail().x - reserved), bool(m_tabBar));
	if (m_tabBar)
	{
		ImGui::SameLine();
		const float tabsWidth = std::max(1.f, ImGui::GetContentRegionAvail().x - reserved);
		ImGui::BeginChild("Applications", {tabsWidth, button + style.ItemSpacing.y}, 0, ImGuiWindowFlags_NoScrollbar);
		m_tabBar();
		ImGui::EndChild();
	}
	if (m_shell.session.state == RunState::Failed && !m_shell.session.error.empty())
	{
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{});
		if (IconButton("Run error", Icon::Warning, "Show the connection or launch error", false, nullptr,
					   theme::Record))
			m_showError = true;
		ImGui::PopStyleColor();
	}
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - actions));
	DrawHistory(runtime);
	ImGui::SameLine();
	const std::string focusTip = std::string(m_previewExpanded ? "Exit focus (F11)" : "Focus: only the image (F11)") +
								 (elapsed.empty() ? "" : "\nRecording " + elapsed + ". Ctrl+R stops");
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{});
	if (IconButton("Focus", Icon::Focus, focusTip.c_str(), m_previewExpanded,
				   elapsed.empty() ? nullptr : elapsed.c_str(), elapsed.empty() ? 0 : theme::Record))
		m_previewExpanded = !m_previewExpanded;
	ImGui::SameLine();
	if (IconButton("Settings", Icon::Settings, "Preferences: scale, snapshots, keys and layout", m_showSettings))
		m_showSettings = true;
	ImGui::PopStyleColor();
	runtime.DrawRecordingPopovers();
	if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
		!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false))
			ToggleLibrary();
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T, false))
			FoldTimeline();
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I, false))
			FoldInspector();
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false))
		{
			if (snapshot.recording)
				runtime.StopRecording();
			else
				runtime.StartRecording();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
		{
			if (m_shell.timeline.playing)
				m_shell.timeline.togglePlayback = !m_shell.timeline.playbackPaused;
			else
				runtime.Pause();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
		{
			if (ImGui::GetIO().KeyShift)
				runtime.CancelPending();
			else
				runtime.Step();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
		{
			if (m_shell.timeline.playing)
				m_shell.timeline.togglePlayback = m_shell.timeline.playbackPaused;
			else
			{
				runtime.ResumeLive();
				LeaveHistory(m_shell, false);
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
			m_previewExpanded = !m_previewExpanded;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !runtime.PilotActive())
		{
			if (m_previewExpanded)
				m_previewExpanded = false;
			else if (!std::holds_alternative<DeviceSel>(m_shell.selection))
				m_shell.selection = m_shell.device;
			else if (m_shell.library)
				m_shell.library = false;
			else
				m_drawer = false;
		}
	}
}
} // namespace simxr::app
