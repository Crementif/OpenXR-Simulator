#include "app/WorkspacePanels.h"
#include "app/AppPicker.h"
#include "app/LaunchAttachment.h"
#include "app/LaunchProfilesDialog.h"
#include "app/Notices.h"
#include "app/PreferencesDialog.h"
#include "app/ProcessLauncher.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimePanels.h"
#include "app/TimelinePanel.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/Library.h"
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
// Which page a selection shows, for noticing a new selection.
std::pair<std::size_t, int> SelectionKey(const Selection& selection)
{
	int page = -1;
	if (const auto device = std::get_if<DeviceSel>(&selection))
		page = static_cast<int>(device->page);
	else if (const auto item = std::get_if<SessionItemSel>(&selection))
		page = static_cast<int>(item->item);
	return {selection.index(), page};
}
} // namespace
struct WorkspacePanels::Impl
{
	bool enabled = false, showLaunch = false, showSettings = false, reset = false, dirty = false;
	bool previewExpanded = false;

	bool showConnection = false, showError = false;
	AppPicker picker;
	std::optional<std::chrono::steady_clock::time_point> recordingSince;
	ShellState shell;
	bool drawer = true;
	// Where clicking Library again (or Esc on the Library) returns to.
	Selection beforeLibrary = DeviceSel{};
	// The launch profile whose pinned crop the preview holds.
	std::optional<std::string> cropProfile;
	bool wasOverlay = false;

	std::filesystem::path path;
	CompanionSettings settings;
	ProcessLauncher launcher;
	TimelinePanel timeline;
	RuntimeConnection launchDiscovery;
	std::unique_ptr<LaunchAttachment> attachment;
#ifdef SIMXR_COMPANION_AUTOMATION
	RigPanel rigs;
	Library library;
	// A replay waits until the companion has released its manual inputs and
	// session control, or the script's pause and take_control would conflict.
	struct PendingReplay
	{
		automation::SavedRecording recording;
		// Set for a Library script; otherwise a take replay.
		std::optional<ScriptOptions> script;
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
	static std::int64_t Now()
	{
		return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();
	}
	void Remember(LaunchProfile launch)
	{
		try
		{
			RememberRecentApp(settings, std::move(launch), Now());
			Save();
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
		}
	}
	void Launch(const LaunchProfile& profile)
	{
		try
		{
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
				Remember(profile);
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
		const std::string label = view.connected ? AppName(view.endpoint)
								  : profiles.Selected() >= 0 ? settings.activeProfile
															 : "Open app";
		const auto choice = picker.Draw(settings, view, label, std::exchange(shell.openAppPicker, false));
		if (choice.connect)
		{
			// Only an explicit Connect attaches; the picker names the process first.
			runtime.ConnectEndpoint(choice.connect->endpoint);
			if (auto launch = DescribeProcess(choice.connect->process))
				Remember(std::move(*launch));
		}
		if (choice.launchProfile)
		{
			const auto index = *choice.launchProfile;
			profiles.Edit(settings, index);
			settings.activeProfile = settings.profiles[static_cast<std::size_t>(index)].name;
			Launch(settings.profiles[static_cast<std::size_t>(index)]);
		}
		if (choice.launchRecent)
			Launch(*choice.launchRecent);
		if (choice.saveRecent && *choice.saveRecent < settings.recent.size())
		{
			if (profiles.Create(settings, settings.recent[*choice.saveRecent].launch))
				Save();
			showLaunch = true;
		}
		if (choice.addProfile)
		{
			profiles.Edit(settings, -1);
			showLaunch = true;
		}
		if (choice.editProfiles)
		{
			if (profiles.Selected() < 0)
				profiles.Edit(settings, 0);
			showLaunch = true;
		}
		if (choice.connectEndpoint)
			showConnection = true;
		if (choice.disconnect)
			runtime.Disconnect();
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
					  state == RunState::Failed ? &details : nullptr, "Show the connection or launch error"))
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
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - button * 4 - spacing * 3));
		if (snapshot.recording ? IconButton("Stop recording", Icon::Stop, "Stop recording (Ctrl+R)", false, nullptr,
										   theme::Record)
							   : IconButton("Rec", Icon::Record, "Record inputs (Ctrl+R)", false, nullptr, theme::Record))
		{
			if (snapshot.recording)
				runtime.StopRecording();
			else
				runtime.StartRecording();
		}
		ImGui::SameLine();
		if (IconButton("Library", Icon::Library,
					   InLibrary() ? "Close the Library (Ctrl+L)" : "Library: takes and scripts (Ctrl+L)", InLibrary()))
			ToggleLibrary();
		ImGui::SameLine();
		if (IconButton("Focus", Icon::Focus, previewExpanded ? "Exit focus (F11)" : "Focus: only the image (F11)",
					   previewExpanded))
			previewExpanded = !previewExpanded;
		ImGui::SameLine();
		if (IconButton("...", Icon::More, "Preferences, layout and advanced controls"))
			ImGui::OpenPopup("Options");
		if (ImGui::BeginPopup("Options"))
		{
			ImGui::MenuItem("Preferences", nullptr, &showSettings);
			ImGui::MenuItem("Focus", "F11", &previewExpanded);
			if (ImGui::MenuItem("Hide timeline", "Ctrl+T", settings.timelineFolded))
				FoldTimeline();
			if (ImGui::MenuItem("Hide inspector", "Ctrl+I", settings.inspectorFolded))
				FoldInspector();
			if (ImGui::MenuItem("Compact device tabs", nullptr, settings.railCollapsed))
			{
				settings.railCollapsed = !settings.railCollapsed;
				dirty = true;
			}
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
				settings.inspectorFolded = false;
				settings.timelineFolded = false;
				dirty = true;
			}
			ImGui::EndPopup();
		}
		runtime.DrawRecordingPopovers();
		if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive())
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
				if (shell.timeline.playing)
					shell.timeline.togglePlayback = !shell.timeline.playbackPaused;
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
				if (shell.timeline.playing)
					shell.timeline.togglePlayback = shell.timeline.playbackPaused;
				else
				{
					runtime.ResumeLive();
					LeaveHistory(shell, false);
				}
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
				previewExpanded = !previewExpanded;
			if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !runtime.PilotActive())
			{
				if (previewExpanded)
					previewExpanded = false;
				else if (InLibrary())
					ToggleLibrary();
				else
					drawer = false;
			}
		}
	}
	bool InLibrary() const
	{
		return std::holds_alternative<LibrarySel>(shell.selection) || std::holds_alternative<TakeSel>(shell.selection) ||
			   std::holds_alternative<ScriptSel>(shell.selection);
	}
	void ToggleLibrary()
	{
		if (InLibrary())
			shell.selection = beforeLibrary;
		else
		{
			beforeLibrary = shell.selection;
			shell.selection = LibrarySel{};
		}
		ShowInspector();
	}
	void ShowInspector()
	{
		drawer = true;
		if (std::exchange(settings.inspectorFolded, false))
			dirty = true;
	}
	void FoldTimeline()
	{
		settings.timelineFolded = !settings.timelineFolded;
		dirty = true;
	}
	void FoldInspector()
	{
		settings.inspectorFolded = !settings.inspectorFolded;
		dirty = true;
	}
	// A rail tab: Library toggles back to the previous page; the open tab
	// clicked again folds or unfolds the inspector.
	void PickTab(Selection picked, bool overlay)
	{
		if (std::holds_alternative<LibrarySel>(picked))
		{
			ToggleLibrary();
			return;
		}
		if (picked.index() == shell.selection.index() && SelectionKey(picked) == SelectionKey(shell.selection))
		{
			if (overlay)
				drawer = !drawer;
			else
				FoldInspector();
			return;
		}
		shell.selection = std::move(picked);
		ShowInspector();
	}
	// The preview's pinned crop belongs to the active launch profile.
	void StoreCrop(const std::optional<ImVec4>& crop)
	{
		auto stored = std::ranges::find(settings.crops, settings.activeProfile, &PreviewCrop::profile);
		if (!crop)
		{
			if (stored != settings.crops.end())
			{
				settings.crops.erase(stored);
				dirty = true;
			}
			return;
		}
		const PreviewCrop value{settings.activeProfile, crop->x, crop->y, crop->z, crop->w};
		if (stored != settings.crops.end() && *stored == value)
			return;
		if (stored != settings.crops.end())
			*stored = value;
		else if (settings.crops.size() < 32)
			settings.crops.push_back(value);
		dirty = true;
	}
	void Splitter(const char* id, ImVec2 position, ImVec2 size, float& value, float minimum, float maximum,
				  bool vertical, float defaultValue)
	{
		ImGui::SetCursorPos(position);
		ImGui::InvisibleButton(id, size);
		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
		{
			ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
			// A thin accent line shows what will move.
			const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
			const ImVec2 center{(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
			const float thickness = 2 * settings.scale;
			ImGui::GetWindowDrawList()->AddLine(vertical ? ImVec2{center.x, minimum.y} : ImVec2{minimum.x, center.y},
												vertical ? ImVec2{center.x, maximum.y} : ImVec2{maximum.x, center.y},
												ImGui::IsItemActive() ? theme::Accent : theme::Fade(theme::Accent, .6f),
												thickness);
			ImGui::SetItemTooltip("Drag to resize; double-click to reset");
		}
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
		// Preview | rail | inspector, timeline below. The rail sits beside the
		// inspector so its tabs join the page they open.
		const bool compact = width / scale < 1100, overlay = width / scale < 800;
		const bool focus = previewExpanded;
		const float rail = focus		  ? 0
						   : overlay	  ? IconRailWidth()
						   : compact || settings.railCollapsed ? NarrowRailWidth()
														 : 196 * scale;
		// Narrowing into the drawer layout must not cover the image with the drawer.
		if (overlay && !std::exchange(wasOverlay, true))
			drawer = false;
		wasOverlay = overlay;
		const bool inspectorFolded = settings.inspectorFolded && !overlay;
		const float inspector = focus || inspectorFolded
									? 0
									: std::min(std::min(settings.inspectorWidth, compact ? 320.0f : 520.0f) * scale,
											   std::max(0.0f, width - rail - 120 * scale));
		const float gap = focus ? 0 : 6 * scale;
		const float timelineSize =
			focus						? 0
			: settings.timelineFolded ? TimelinePanel::FoldedHeight(scale)
									  : std::min(height * .5f, (!settings.timelineResized && height / scale < 560
																	? 56
																	: settings.timelineHeight) *
																   scale);
		const float top = origin.y + 40 * scale, bodyHeight = std::max(1.0f, height - 40 * scale - gap - timelineSize);
		const float railX = origin.x + width - rail - (overlay ? 0 : inspector);
		shell.preview = {origin.x, top, std::max(1.0f, railX - origin.x), bodyHeight};
		shell.inspector = {overlay ? railX - inspector : railX + rail, top, std::max(1.0f, inspector), bodyHeight};
		shell.inspectorVisible = !focus && !inspectorFolded && (!overlay || drawer);
		if (!focus)
		{
			ImGui::SetCursorPos({railX, top});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Bg);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 4 * scale});
			ImGui::BeginChild("Device rail", {rail, bodyHeight}, ImGuiChildFlags_AlwaysUseWindowPadding,
							  ImGuiWindowFlags_NoScrollbar);
			ImGui::PopStyleVar();
			const float footer = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
			if (auto picked = DrawDeviceRail(shell.selection, runtime.View(), settings, overlay, footer))
				PickTab(std::move(*picked), overlay);
			if (!overlay)
			{
				ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeight() - 4 * scale);
				ImGui::SetCursorPosX((rail - ImGui::GetFrameHeight()) / 2);
				if (IconButton("Fold inspector", inspectorFolded ? Icon::ChevronLeft : Icon::ChevronRight,
							   inspectorFolded ? "Show the inspector (Ctrl+I)" : "Hide the inspector (Ctrl+I)"))
					FoldInspector();
			}
			ImGui::EndChild();
			ImGui::PopStyleColor();
			if (!overlay && !inspectorFolded)
				Splitter("Inspector splitter", {railX - 3 * scale, top}, {6 * scale, bodyHeight}, settings.inspectorWidth,
						 280, compact ? 320 : 520, true, 340);
			if (!settings.timelineFolded)
				Splitter("Timeline splitter", {origin.x, top + bodyHeight}, {width, 6 * scale}, settings.timelineHeight,
						 56, height / scale * .5f, false, 132);
		}
		runtime.ConfigurePilot(settings.pilotBindings, settings.gamepadBindings);
		runtime.ConfigureSnapshots(settings.snapshotBudgetMiB);
		// The overlay drawer closes on a click into the preview and opens on any new selection.
		const auto window = ImGui::GetWindowPos();
		const auto contains = [&](const ShellRect& rect, ImVec2 point)
		{
			return point.x >= window.x + rect.x && point.x < window.x + rect.x + rect.width &&
				   point.y >= window.y + rect.y && point.y < window.y + rect.y + rect.height;
		};
		const auto selected = SelectionKey(shell.selection);
		if (overlay && drawer && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
			contains(shell.preview, ImGui::GetIO().MousePos) && !contains(shell.inspector, ImGui::GetIO().MousePos))
			drawer = false;
		runtime.HudFolded() = settings.hudFolded;
		if (cropProfile != settings.activeProfile)
		{
			const auto crop = std::ranges::find(settings.crops, settings.activeProfile, &PreviewCrop::profile);
			runtime.PreviewCrop() = crop == settings.crops.end()
										? std::nullopt
										: std::optional<ImVec4>(ImVec4{crop->x0, crop->y0, crop->x1, crop->y1});
			cropProfile = settings.activeProfile;
		}
		runtime.DrawPreview(shell);
		if (std::exchange(settings.hudFolded, runtime.HudFolded()) != settings.hudFolded)
			dirty = true;
		StoreCrop(runtime.PreviewCrop());
		if (SelectionKey(shell.selection) != selected)
			drawer = true;
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
		notices.Draw({window.x + shell.preview.x, window.y + shell.preview.y, shell.preview.width, shell.preview.height});
		if (focus)
		{
			// Focus mode keeps one visible way back at the image's top right.
			const char* label = "Exit focus";
			const float chip = ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 3;
			ImGui::SetCursorPos({shell.preview.x + shell.preview.width - chip - 8 * scale, shell.preview.y + 4 * scale});
			// Its own child, created after the preview, so it draws and hovers above the image.
			ImGui::BeginChild("Focus exit", {chip, ImGui::GetFrameHeight()}, 0,
							  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
			if (IconButton("Exit focus", Icon::Focus, "Back to the full layout (F11 or Esc)", true, label))
				previewExpanded = false;
			ImGui::EndChild();
		}
		else
		{
			ImGui::SetCursorPos({origin.x, top + bodyHeight + gap});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
			ImGui::BeginChild("Timeline", {width, timelineSize}, 0,
							  settings.timelineFolded ? ImGuiWindowFlags_NoScrollbar : ImGuiWindowFlags_None);
			bool fold = false;
			if (timeline.Draw(runtime, shell, settings.timelineFolded, fold))
				ShowInspector();
			if (fold)
				FoldTimeline();
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
		else if (std::holds_alternative<LibrarySel>(shell.selection) ||
				 std::holds_alternative<TakeSel>(shell.selection) ||
				 std::holds_alternative<ScriptSel>(shell.selection))
		{
#ifdef SIMXR_COMPANION_AUTOMATION
			if (auto run = library.Draw(shell.selection, runtime, rigs))
			{
				runtime.PrepareReplay();
				replay = PendingReplay{{run->path}, run->options, std::chrono::steady_clock::now() + std::chrono::seconds(3)};
			}
#else
			ImGui::TextDisabled("Takes and scripts need a Luau-enabled build");
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
			replay = PendingReplay{std::move(*recording), {}, std::chrono::steady_clock::now() + std::chrono::seconds(3)};
		}
		if (replay)
		{
			if (runtime.ReplayReady() || std::chrono::steady_clock::now() >= replay->deadline)
			{
				if (replay->script)
					rigs.RunScript(runtime, replay->recording.path, *replay->script);
				else
				{
					timeline.ReplayTake(replay->recording.path);
					rigs.ReplayRecording(runtime, replay->recording.path, replay->recording.seed);
				}
				replay.reset();
			}
		}
		runtime.SetReplayActive(rigs.IsActive());
		timeline.ObserveRig(runtime.Snapshot(), rigs.RunId(), rigs.Snapshot(), rigs.MatchesTarget(runtime.Snapshot()));
		shell.timeline.playing = rigs.IsActive() && rigs.ReplayingTake();
		shell.timeline.playbackPaused = rigs.Snapshot().phase == automation::RigPhase::Paused;
#endif
		DrawShell(runtime);
#ifdef SIMXR_COMPANION_AUTOMATION
		if (std::exchange(shell.timeline.togglePlayback, false))
			shell.timeline.playbackPaused ? rigs.Resume() : rigs.Pause();
#endif
		if (auto saved = runtime.TakeSavedTake())
		{
			shell.timeline.recordedTake = *saved;
#ifdef SIMXR_COMPANION_AUTOMATION
			library.Refresh();
#endif
			shell.selection = TakeSel{std::move(*saved)};
			drawer = true;
		}
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
		if (preferences.Draw(settings, showSettings, dirty, runtime.KeptPreviews().Bytes(), runtime.KeptPreviews().Budget()))
		{
			settings.endpoint = runtime.Endpoint();
			Save();
		}
		bool changed = false;
		if (profiles.Draw(settings, launcher.Snapshot(), showLaunch, changed))
			Launch(profiles.Profile());
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
	m_impl->ShowInspector();
	// An explicit selection opens the drawer even when the window just narrowed.
	m_impl->wasOverlay = true;
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
