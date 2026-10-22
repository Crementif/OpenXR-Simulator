#include "app/WorkspacePanels.h"
#include "app/LaunchAttachment.h"
#include "app/LaunchProfilesDialog.h"
#include "app/Notices.h"
#include "app/PreferencesDialog.h"
#include "app/ProcessLauncher.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimeEndpoints.h"
#include "app/RuntimePanels.h"
#include "app/TimelinePanel.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/RigPanel.h"
#endif

#include "app/DeviceRail.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <stdexcept>
#include <utility>

namespace simxr::app
{
namespace
{
// The application's process name when the endpoint names its process id
// (control.<pid>.sock), otherwise the endpoint file name.
std::string AppName(const std::string& endpoint)
{
	const auto file = std::filesystem::path(endpoint).filename().string();
	const auto first = file.find('.'), last = file.rfind('.');
	if (first != std::string::npos && last > first)
	{
		std::ifstream comm("/proc/" + file.substr(first + 1, last - first - 1) + "/comm");
		std::string name;
		if (comm && std::getline(comm, name) && !name.empty())
			return name;
	}
	return file;
}
} // namespace
struct WorkspacePanels::Impl
{
	bool enabled = false, showLaunch = false, showSettings = false, reset = false, dirty = false;
	bool previewExpanded = false;

	bool showConnection = false, showError = false;
	std::vector<std::string> endpoints;
	std::optional<std::chrono::steady_clock::time_point> recordingSince;
	ShellState shell;
	bool drawer = true;

	std::filesystem::path path;
	CompanionSettings settings;
	ProcessLauncher launcher;
	TimelinePanel timeline;
	RuntimeConnection launchDiscovery;
	std::unique_ptr<LaunchAttachment> attachment;
#ifdef SIMXR_COMPANION_AUTOMATION
	RigPanel rigs;
	// A replay waits until the companion has released its manual inputs and
	// session control, or the script's pause and take_control would conflict.
	struct PendingReplay
	{
		automation::SavedRecording recording;
		std::chrono::steady_clock::time_point deadline;
	};
	std::optional<PendingReplay> replay;
#endif
	PreferencesDialog preferences;
	LaunchProfilesDialog profiles;
	Notices notices;
	std::string error, iniPath;
	float appliedScale = 1;
	ImGuiStyle originalStyle;
	bool styleCaptured = false;

	Impl(ProcessSupervisor& processes, bool enabled, const std::string& endpoint)
		: enabled(enabled), launcher(processes)
	{
		if (enabled)
		{
			try
			{
				path = CompanionSettingsPath();
				settings = LoadSettings(path);
			}
			catch (const std::exception& failure)
			{
				error = failure.what();
			}
		}
		if (enabled && !path.empty())
		{
			iniPath = (path.parent_path() / "companion-v2.ini").string();
			ImGui::GetIO().IniFilename = iniPath.c_str();
		}
		if (!endpoint.empty())
			settings.endpoint = endpoint;
		for (std::size_t index = 0; index < settings.profiles.size(); ++index)
			if (settings.profiles[index].name == settings.activeProfile)
				profiles.Edit(settings, static_cast<int>(index));
	}
	~Impl()
	{
		if (enabled && !iniPath.empty() && ImGui::GetCurrentContext())
		{
			ImGui::SaveIniSettingsToDisk(iniPath.c_str());
			ImGui::GetIO().IniFilename = nullptr;
		}
	}
	void Save()
	{
		if (!enabled)
			return;
		try
		{
			if (path.empty())
				path = CompanionSettingsPath();
			SaveSettings(path, settings);
			dirty = false;
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
		}
	}
	void BeginFrame(RuntimePanels&)
	{
		if (!styleCaptured)
		{
			originalStyle = ImGui::GetStyle();
			styleCaptured = true;
		}
		if (appliedScale != settings.scale)
		{
			ImGui::GetStyle() = originalStyle;
			ImGui::GetStyle().ScaleAllSizes(settings.scale);
			ImGui::GetStyle().FontScaleMain = settings.scale;
			appliedScale = settings.scale;
		}
	}
	void LaunchSelected()
	{
		try
		{
			const auto profile = profiles.Profile();
			auto pending = std::make_unique<LaunchAttachment>(launchDiscovery, profile);
#ifdef SIMXR_COMPANION_AUTOMATION
			rigs.PrepareLaunch(profile);
#else
			if (!profile.rig.empty())
				throw std::runtime_error("This build does not support launch rigs");
#endif
			if (launcher.Launch(profile))
			{
				attachment = std::move(pending);
				error.clear();
			}
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
#ifdef SIMXR_COMPANION_AUTOMATION
			rigs.PrepareLaunch({});
#endif
		}
	}
	void PollLaunch(RuntimePanels& runtime)
	{
		if (!attachment)
			return;
		try
		{
			attachment->Observe(launcher.Snapshot());
			if (auto session = attachment->Poll())
			{
				attachment.reset();
				runtime.ConnectEndpoint(session->endpoint);
#ifdef SIMXR_COMPANION_AUTOMATION
				rigs.AttachLaunch(*session);
#endif
			}
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
			attachment.reset();
#ifdef SIMXR_COMPANION_AUTOMATION
			rigs.PrepareLaunch({});
#endif
		}
	}
	void DrawAppPicker(RuntimePanels& runtime, const RuntimeView& view)
	{
		std::string label = view.connected ? AppName(view.endpoint)
							: profiles.Selected() >= 0 ? settings.activeProfile
													   : "Open app";
		if (IconButton("##App picker", Icon::App, "Launch or connect an OpenXR app", false, label.c_str()) ||
			std::exchange(shell.openAppPicker, false))
		{
			endpoints = DiscoverRuntimeEndpoints();
			ImGui::OpenPopup("App picker");
		}
		if (!ImGui::BeginPopup("App picker"))
			return;
		ImGui::SeparatorText("Running now");
		ImGui::SameLine();
		if (IconButton("Refresh", Icon::Refresh, "Look for running apps again"))
			endpoints = DiscoverRuntimeEndpoints();
		if (endpoints.empty())
			ImGui::TextDisabled("No running apps found");
		for (const auto& endpoint : endpoints)
		{
			ImGui::PushID(endpoint.c_str());
			if (ImGui::Selectable(AppName(endpoint).c_str(), endpoint == view.endpoint))
				runtime.ConnectEndpoint(endpoint);
			ImGui::SetItemTooltip("%s", endpoint.c_str());
			ImGui::PopID();
		}
		ImGui::SeparatorText("Launch");
		for (std::size_t index = 0; index < settings.profiles.size(); ++index)
			if (ImGui::Selectable(settings.profiles[index].name.c_str()))
			{
				profiles.Edit(settings, static_cast<int>(index));
				settings.activeProfile = settings.profiles[index].name;
				LaunchSelected();
			}
		ImGui::Separator();
		if (ImGui::Selectable("Edit profiles"))
			showLaunch = true;
		if (ImGui::Selectable("Connect endpoint"))
			showConnection = true;
		if (view.connected && ImGui::Selectable("Disconnect"))
			runtime.Disconnect();
		ImGui::EndPopup();
	}
	void DrawToolbar(RuntimePanels& runtime)
	{
		const auto& snapshot = runtime.View();
		shell.session = DeriveRunState(snapshot, attachment != nullptr, error);
		if (std::holds_alternative<DeviceSel>(shell.selection) || std::holds_alternative<SessionItemSel>(shell.selection))
			shell.device = shell.selection;
		if (const auto stepped = runtime.TakeSteppedFrame();
			stepped && (stepped->requested || std::holds_alternative<FrameSel>(shell.selection)))
		{
			shell.selection = stepped->frame;
			drawer = drawer || stepped->requested;
		}
		DrawAppPicker(runtime, snapshot);
		ImGui::SameLine();
		const auto state = shell.session.state;
		const auto dot = state == RunState::Running	  ? theme::Ok
						 : state == RunState::Paused  ? theme::Paused
						 : state == RunState::Failed  ? theme::Record
						 : state == RunState::Launching || state == RunState::WaitingForSession ? theme::Warn
																								   : theme::TextDim;
		std::string chip = RunStateLabel(state);
		if (state == RunState::Running && shell.session.fps > 0)
			chip += " \u00b7 " + std::to_string(static_cast<int>(shell.session.fps + .5f)) + " fps";
		const Icon details = Icon::Warning;
		if (StateChip("Run state", dot, chip.c_str(), shell.session.error.empty() ? nullptr : shell.session.error.c_str(),
					  state == RunState::Failed ? &details : nullptr, "Details"))
			showError = true;
		if (snapshot.recording)
		{
			if (!recordingSince)
				recordingSince = std::chrono::steady_clock::now();
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
																				   *recordingSince)
									 .count();
			char label[32];
			std::snprintf(label, sizeof(label), "REC %lld:%02lld", static_cast<long long>(seconds / 60),
						  static_cast<long long>(seconds % 60));
			ImGui::SameLine();
			StateChip("Recording", theme::Record, label,
					  (std::to_string(snapshot.recordedChanges) + " input changes recorded").c_str());
		}
		else
			recordingSince.reset();
#ifdef SIMXR_COMPANION_AUTOMATION
		if (rigs.HasStatus())
		{
			ImGui::SameLine();
			rigs.DrawTransport();
		}
#endif
		const float right = ImGui::GetContentRegionMax().x;
		const float button = ImGui::GetFrameHeight(), spacing = ImGui::GetStyle().ItemSpacing.x;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - button * 3 - spacing * 2));
		if (snapshot.recording ? IconButton("Stop recording", Icon::Stop, "Stop recording (Ctrl+R)", false, nullptr,
										   theme::Record)
							   : IconButton("Rec", Icon::Record, "Record inputs (Ctrl+R)", false, nullptr, theme::Record))
		{
			if (snapshot.recording)
				runtime.StopRecording();
			else
			{
				shell.selection = LibrarySel{};
				runtime.StartRecording();
				drawer = true;
			}
		}
		ImGui::SameLine();
		if (IconButton("Library", Icon::Library, "Library: takes and scripts (Ctrl+L)",
					   std::holds_alternative<LibrarySel>(shell.selection)))
		{
			shell.selection = LibrarySel{};
			drawer = true;
		}
		ImGui::SameLine();
		if (IconButton("...", Icon::More, "More"))
			ImGui::OpenPopup("Options");
		if (ImGui::BeginPopup("Options"))
		{
			ImGui::MenuItem("Preferences", nullptr, &showSettings);
			ImGui::MenuItem("Full preview", "F11", &previewExpanded);
			if (ImGui::MenuItem("Release inputs"))
				runtime.ReleaseInputs();
			if (ImGui::MenuItem("Cancel pending", "Shift+F7"))
				runtime.CancelPending();
			if (ImGui::MenuItem("Advanced stepping"))
			{
				shell.selection = AdvancedSel{};
				drawer = true;
			}
			if (ImGui::MenuItem("Frame details"))
			{
				shell.selection = FrameSel{snapshot.time.cursor, snapshot.time.epoch};
				drawer = true;
			}
			if (ImGui::MenuItem("Reset layout"))
			{
				settings.inspectorWidth = 340;
				settings.timelineHeight = 132;
				settings.timelineResized = false;
				settings.railCollapsed = false;
			}
			ImGui::EndPopup();
		}
		if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive())
		{
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false))
			{
				shell.selection = LibrarySel{};
				drawer = true;
			}
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false))
			{
				if (snapshot.recording)
					runtime.StopRecording();
				else
				{
					shell.selection = LibrarySel{};
					drawer = true;
					runtime.StartRecording();
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
				runtime.Pause();
			if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
			{
				if (ImGui::GetIO().KeyShift)
					runtime.CancelPending();
				else
					runtime.Step();
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
			{
				runtime.ResumeLive();
				LeaveHistory(shell, false);
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
				previewExpanded = !previewExpanded;
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
				drawer = false;
		}
	}
	void Splitter(const char* id, ImVec2 position, ImVec2 size, float& value, float minimum, float maximum,
				  bool vertical, float defaultValue)
	{
		ImGui::SetCursorPos(position);
		ImGui::InvisibleButton(id, size);
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
		if (ImGui::IsItemActive())
		{
			value = std::clamp(value - (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y) /
										   settings.scale,
							   minimum, maximum);
			if (!vertical)
				settings.timelineResized = true;
			dirty = true;
		}
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
		{
			value = defaultValue;
			dirty = true;
		}
	}
	void DrawShell(RuntimePanels& runtime)
	{
		const auto* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::Begin("OpenXR Simulator", nullptr,
					 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
						 ImGuiWindowFlags_NoBringToFrontOnFocus);
		const float scale = settings.scale, width = ImGui::GetContentRegionAvail().x;
		const float height = ImGui::GetContentRegionAvail().y;
		const auto origin = ImGui::GetCursorPos();
		ImGui::BeginChild("Top bar", {width, 40 * scale}, 0, ImGuiWindowFlags_NoScrollbar);
		DrawToolbar(runtime);
		ImGui::EndChild();
		const bool compact = width / scale < 1100, overlay = width / scale < 800;
		const float narrowRail = NarrowRailWidth();
		const bool narrow = compact || settings.railCollapsed;
		const float rail = previewExpanded ? 0 : narrow ? narrowRail : 196 * scale;
		const float inspector =
			previewExpanded ? 0 : std::min(settings.inspectorWidth, compact ? 320.0f : 520.0f) * scale;
		const float timelineSize =
			previewExpanded
				? 0
				: std::min(height * .5f,
						   (!settings.timelineResized && height / scale < 560 ? 56 : settings.timelineHeight) * scale);
		const float top = origin.y + 40 * scale, bodyHeight = std::max(1.0f, height - 46 * scale - timelineSize);
		shell.preview = {origin.x + rail, top, std::max(1.0f, width - rail - (overlay ? 0 : inspector)), bodyHeight};
		shell.inspector = {origin.x + width - inspector, top, std::max(1.0f, inspector), bodyHeight};
		shell.inspectorVisible = !previewExpanded && (!overlay || drawer);
		if (!previewExpanded)
		{
			ImGui::SetCursorPos({origin.x, top});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
			ImGui::BeginChild("Device rail", {rail, bodyHeight}, 0, ImGuiWindowFlags_NoScrollbar);
			if (DrawDeviceRail(shell.selection, runtime.View(), settings))
				drawer = true;
			ImGui::EndChild();
			ImGui::PopStyleColor();
			ImGui::SetCursorPos({origin.x + rail - 3 * scale, top});
			ImGui::InvisibleButton("Rail splitter", {6 * scale, bodyHeight});
			if (!compact && ImGui::IsItemClicked())
			{
				settings.railCollapsed = !settings.railCollapsed;
				dirty = true;
			}
			if (!overlay)
				Splitter("Inspector splitter", {shell.inspector.x - 3 * scale, top}, {6 * scale, bodyHeight},
						 settings.inspectorWidth, 280, compact ? 320 : 520, true, 340);
			Splitter("Timeline splitter", {origin.x, top + bodyHeight}, {width, 6 * scale}, settings.timelineHeight, 56,
					 height / scale * .5f, false, 132);
		}
		runtime.ConfigurePilot(settings.pilotBindings, settings.gamepadBindings);
		runtime.ConfigureSnapshots(settings.snapshotBudgetMiB);
		runtime.DrawPreview(shell);
		if (shell.inspectorVisible)
		{
			ImGui::SetCursorPos({shell.inspector.x, shell.inspector.y});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
			ImGui::BeginChild("Inspector", {shell.inspector.width, shell.inspector.height});
			DrawInspector(runtime);
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}
		for (auto& notice : runtime.TakeNotices())
			notices.Push(std::move(notice));
		const auto window = ImGui::GetWindowPos();
		notices.Draw({window.x + shell.preview.x, window.y + shell.preview.y, shell.preview.width, shell.preview.height});
		if (!previewExpanded)
		{
			ImGui::SetCursorPos({origin.x, top + bodyHeight + 6 * scale});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
			ImGui::BeginChild("Timeline", {width, timelineSize});
			if (timeline.Draw(runtime, shell))
			{
				drawer = true;
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
		}
		const auto state = shell.session.state;
		const ImU32 border = state == RunState::Paused			  ? theme::Paused
							 : !runtime.View().time.following ? theme::Past
							 : state == RunState::Disconnected ? theme::TextDim
															   : 0;
		if (border)
			ImGui::GetForegroundDrawList()->AddRect(
				viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y}, border, 0, 0,
				2 * scale);
		ImGui::End();
	}
	void DrawInspector(RuntimePanels& runtime)
	{
		if (std::holds_alternative<DeviceSel>(shell.selection) || std::holds_alternative<SessionItemSel>(shell.selection))
			runtime.DrawDevicePage(shell);
		else if (std::holds_alternative<FrameSel>(shell.selection))
			runtime.DrawFramePage(shell);
		else if (const auto range = std::get_if<RangeSel>(&shell.selection))
			runtime.DrawRangePage(RangeSel{*range}, shell);
		else if (std::holds_alternative<AdvancedSel>(shell.selection))
			runtime.DrawAdvancedPage();
		else if (std::holds_alternative<LibrarySel>(shell.selection))
		{
			runtime.DrawLibrary();
#ifdef SIMXR_COMPANION_AUTOMATION
			rigs.Draw(runtime, true);
#endif
		}
	}
	void Draw(RuntimePanels& runtime)
	{
		PollLaunch(runtime);
		runtime.BeginFrame();
#ifdef SIMXR_COMPANION_AUTOMATION
		if (auto recording = runtime.TakeRecordingReplay())
		{
			runtime.PrepareReplay();
			replay = PendingReplay{std::move(*recording), std::chrono::steady_clock::now() + std::chrono::seconds(3)};
		}
		if (replay)
		{
			if (runtime.ReplayReady() || std::chrono::steady_clock::now() >= replay->deadline)
			{
				timeline.ReplayTake(replay->recording.path);
				rigs.ReplayRecording(runtime, replay->recording.path, replay->recording.seed);
				replay.reset();
			}
		}
		rigs.Draw(runtime, false);
		runtime.SetReplayActive(rigs.IsActive());
		timeline.ObserveRig(runtime.Snapshot(), rigs.RunId(), rigs.Snapshot(), rigs.MatchesTarget(runtime.Snapshot()));
#endif
		DrawShell(runtime);
		if (showConnection)
			runtime.DrawConnection(&showConnection);
		if (!error.empty() || showError)
		{
			ImGui::OpenPopup("Error details");
			if (ImGui::BeginPopupModal("Error details"))
			{
				ImGui::TextWrapped("%s", (error.empty() ? shell.session.error : error).c_str());
				if (ImGui::Button("Dismiss"))
				{
					error.clear();
					showError = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}
		if (preferences.Draw(settings, showSettings, dirty))
		{
			settings.endpoint = runtime.Endpoint();
			Save();
		}
		bool changed = false;
		if (profiles.Draw(settings, launcher.Snapshot(), showLaunch, changed, error))
			LaunchSelected();
		if (changed)
		{
			dirty = true;
			Save();
		}
		runtime.DrawWindows();
	}
};
WorkspacePanels::WorkspacePanels(ProcessSupervisor& processes, bool enabled, std::string endpoint)
	: m_impl(std::make_unique<Impl>(processes, enabled, endpoint))
{
}
WorkspacePanels::~WorkspacePanels() = default;
std::string WorkspacePanels::InitialEndpoint() const
{
	return m_impl->settings.endpoint;
}
void WorkspacePanels::BeginFrame(RuntimePanels& runtime)
{
	m_impl->BeginFrame(runtime);
}
void WorkspacePanels::Draw(RuntimePanels& runtime)
{
	m_impl->Draw(runtime);
}
const ShellState& WorkspacePanels::State() const
{
	return m_impl->shell;
}
void WorkspacePanels::Select(Selection selection)
{
	m_impl->shell.selection = std::move(selection);
	m_impl->drawer = true;
}
void WorkspacePanels::SetPreviewExpanded(bool expanded)
{
	m_impl->previewExpanded = expanded;
}
void WorkspacePanels::SetScale(float scale)
{
	if (!(scale >= 0.75f && scale <= 2.5f))
		throw std::invalid_argument("UI scale must be between 0.75 and 2.5");
	m_impl->settings.scale = scale;
	m_impl->dirty = true;
	m_impl->reset = true;
}
void WorkspacePanels::Finish()
{
	if (m_impl->enabled && m_impl->dirty)
		m_impl->Save();
}
} // namespace simxr::app
