#include "app/WorkspacePanels.h"
#include "app/AppPicker.h"
#include "app/services/LaunchAttachment.h"
#include "app/LaunchProfilesDialog.h"
#include "app/Notices.h"
#include "app/PreferencesDialog.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/RuntimeConnection.h"
#include "app/RuntimePanels.h"
#include "app/widgets/TimelinePanel.h"
#include "app/services/Utf8Path.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/pages/Library.h"
#include "app/services/RigController.h"
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
#include <tuple>
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
	return {selection.index(), page};
}
} // namespace
struct WorkspacePanels::Impl
{
	bool enabled = false, showLaunch = false, showSettings = false, reset = false, dirty = false;
	bool previewExpanded = false;

	bool showConnection = false, showError = false;
	AppPicker picker;
	ShellState shell;
	bool drawer = true;
	// The app this companion launched, named by its profile, until another endpoint connects.
	std::string launchName, launchedName, launchedEndpoint;
	// The launch profile whose pinned crop the preview holds.
	std::optional<std::string> cropProfile;
	bool wasOverlay = false;
	struct DividerDrag
	{
		ImGuiID id;
		// The size the pointer asks for, before clamping or folding.
		float request;
		float travel;
	};
	std::optional<DividerDrag> dividerDrag;

	std::filesystem::path path;
	CompanionSettings settings;
	ProcessLauncher launcher;
	TimelinePanel timeline;
	RuntimeConnection launchDiscovery;
	std::unique_ptr<LaunchAttachment> attachment;
#ifdef SIMXR_COMPANION_AUTOMATION
	RigController rigs;
	Library library;
	// A replay waits until the companion has released its manual inputs and
	// session control, or the script's pause and take_control would conflict.
	struct PendingReplay
	{
		automation::SavedRecording recording;
		// Set for a Library script; otherwise a take replay.
		std::optional<ScriptOptions> script;
		std::chrono::steady_clock::time_point deadline;
		RuntimeFrameRun target;
	};
	std::optional<PendingReplay> replay;
	bool replayFailed = false;
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
				launchName = profile.name.empty() ? PathText(Utf8Path(profile.executable).stem()) : profile.name;
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
				launchedEndpoint = session->endpoint;
				launchedName = launchName;
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
		const auto state = shell.session.state;
		const std::string label = !view.connected ? (profiles.Selected() >= 0 ? settings.activeProfile : "Open app")
								  : view.endpoint == launchedEndpoint ? launchedName
																	  : AppName(view.endpoint);
		const ImU32 dot = state == RunState::Running   ? theme::Ok
						  : state == RunState::Paused  ? theme::Paused
						  : state == RunState::Failed  ? theme::Record
						  : state == RunState::Launching || state == RunState::WaitingForSession ? theme::Warn
						  : state == RunState::Disconnected ? theme::TextDim
															: 0;
		std::string tooltip = RunStateLabel(state);
		if (view.connected)
			tooltip += " \u00b7 " + view.endpoint;
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
		const auto choice =
			picker.Draw(settings, view, label, dot, tooltip, std::exchange(shell.openAppPicker, false), session);
		if (!choice.discoveryError.empty())
			notices.Push({"Application discovery failed", choice.discoveryError});
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
	// Recent pose edits (click one to undo back to before it) and notices whose toasts are gone.
	void DrawHistory(RuntimePanels& runtime)
	{
		const bool unseen = notices.Unseen();
		if (IconButton("History", Icon::History, "Recent changes and notices", ImGui::IsPopupOpen("History")))
		{
			ImGui::OpenPopup("History");
			notices.MarkSeen();
		}
		if (unseen)
		{
			// A dot while a notice arrived that the history has not shown yet.
			const auto corner = ImGui::GetItemRectMax();
			const float radius = ImGui::GetFrameHeight() * .12f;
			ImGui::GetWindowDrawList()->AddCircleFilled(
				{corner.x - radius * 1.6f, ImGui::GetItemRectMin().y + radius * 1.6f}, radius, theme::Warn);
		}
		const float scale = settings.scale;
		ImGui::SetNextWindowSize({300 * scale, 0});
		if (!ImGui::BeginPopup("History"))
			return;
		const auto now = std::chrono::steady_clock::now();
		const auto ago = [&](std::chrono::steady_clock::time_point time)
		{
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - time).count();
			char text[24];
			if (seconds < 5)
				return std::string("now");
			if (seconds < 60)
				std::snprintf(text, sizeof(text), "%lld s", static_cast<long long>(seconds));
			else if (seconds < 3600)
				std::snprintf(text, sizeof(text), "%lld min", static_cast<long long>(seconds / 60));
			else
				std::snprintf(text, sizeof(text), "%lld h", static_cast<long long>(seconds / 3600));
			return std::string(text);
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
		const auto& logged = notices.Log();
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
	void DrawToolbar(RuntimePanels& runtime)
	{
		const auto& snapshot = runtime.View();
		shell.session = DeriveRunState(snapshot, attachment != nullptr, error);
		if (std::holds_alternative<DeviceSel>(shell.selection))
			shell.device = shell.selection;
		if (const auto stepped = runtime.TakeSteppedFrame();
			stepped && (stepped->requested || std::holds_alternative<FrameSel>(shell.selection)))
		{
			shell.selection = FrameSel{stepped->frame, stepped->epoch};
			drawer = drawer || stepped->requested;
		}
		DrawAppPicker(runtime, snapshot);
		if (shell.session.state == RunState::Failed && !shell.session.error.empty())
		{
			ImGui::SameLine();
			if (IconButton("Run error", Icon::Warning, "Show the connection or launch error", false, nullptr,
						   theme::Record))
				showError = true;
		}
		if (!snapshot.recording)
			shell.timeline.recordingSince.reset();
		else if (!shell.timeline.recordingSince)
			shell.timeline.recordingSince = std::chrono::steady_clock::now();
		// Focus hides the timeline, so the Focus button carries a running recording's time.
		std::string elapsed;
		if (previewExpanded && shell.timeline.recordingSince)
		{
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
																				   *shell.timeline.recordingSince)
									 .count();
			char text[16];
			std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(seconds / 60),
						  static_cast<long long>(seconds % 60));
			elapsed = text;
		}
		const float right = ImGui::GetContentRegionMax().x;
		const auto& style = ImGui::GetStyle();
		const float button = ImGui::GetFrameHeight();
		const float focus =
			elapsed.empty() ? button : button + ImGui::CalcTextSize(elapsed.c_str()).x + style.FramePadding.x;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - button * 2 - focus - style.ItemSpacing.x * 2));
		DrawHistory(runtime);
		ImGui::SameLine();
		const std::string focusTip = std::string(previewExpanded ? "Exit focus (F11)" : "Focus: only the image (F11)") +
									 (elapsed.empty() ? "" : "\nRecording " + elapsed + "; Ctrl+R stops");
		if (IconButton("Focus", Icon::Focus, focusTip.c_str(), previewExpanded,
					   elapsed.empty() ? nullptr : elapsed.c_str(), elapsed.empty() ? 0 : theme::Record))
			previewExpanded = !previewExpanded;
		ImGui::SameLine();
		if (IconButton("Settings", Icon::Settings, "Preferences: scale, snapshots, keys and layout", showSettings))
			showSettings = true;
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
				else if (!std::holds_alternative<DeviceSel>(shell.selection))
					shell.selection = shell.device;
				else if (shell.library)
					shell.library = false;
				else
					drawer = false;
			}
		}
	}
	// The Library drawer lives in the timeline, so opening it shows the timeline.
	void ToggleLibrary()
	{
		shell.library = !shell.library;
		previewExpanded = previewExpanded && !shell.library;
		if (shell.library && std::exchange(settings.timelineFolded, false))
			dirty = true;
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
	// A device tab; the open tab clicked again folds or unfolds the inspector.
	void PickTab(Selection picked, bool overlay)
	{
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
	// A region edge drawn above the regions it separates: drag to resize, drag
	// past the minimum to fold, and click its tab to fold or unfold. Sizes are
	// base pixels; returns true when the region should fold or unfold.
	bool Divider(const char* id, ShellRect strip, bool vertical, float& value, float minimum, float maximum,
				 float defaultValue, bool folded, float foldBelow)
	{
		const float scale = settings.scale;
		const auto& io = ImGui::GetIO();
		ImGui::PushID(id);
		const auto region = [&](const char* part, ShellRect rect)
		{
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
			if (!dividerDrag || dividerDrag->id != key)
				dividerDrag = DividerDrag{key, folded ? foldBelow : value, 0};
			const float delta = (vertical ? io.MouseDelta.x : io.MouseDelta.y) / scale;
			dividerDrag->request -= delta;
			dividerDrag->travel += std::abs(delta);
			const float request = dividerDrag->request;
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
				settings.timelineResized = true;
			dirty = true;
		}
		// A click on the tab without a drag folds or unfolds.
		if (tabReleased && dividerDrag && dividerDrag->id == key && dividerDrag->travel * scale < 3)
			toggle = !toggle;
		if (!active && dividerDrag && dividerDrag->id == key)
			dividerDrag.reset();
		if (lineDoubleClicked || tabDoubleClicked)
		{
			value = defaultValue;
			toggle = folded;
			dirty = true;
		}
		const bool hot = lineHovered || tabHovered || active;
		if (hot)
			ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
		const ImVec2 center{(lineMin.x + lineMax.x) / 2, (lineMin.y + lineMax.y) / 2};
		lineDraw->AddLine(vertical ? ImVec2{center.x, lineMin.y} : ImVec2{lineMin.x, center.y},
						  vertical ? ImVec2{center.x, lineMax.y} : ImVec2{lineMax.x, center.y},
						  active ? theme::Accent : hot ? theme::Fade(theme::Accent, .6f) : theme::Line,
						  (hot ? 2.0f : 1.0f) * scale);
		tabDraw->AddRectFilled(tabMin, tabMax,
							   tabHovered || active ? theme::Mix(theme::SurfaceRaised, theme::Text, .14f)
													: theme::SurfaceRaised,
							   theme::Radius * scale);
		tabDraw->AddRect(tabMin, tabMax, hot ? theme::Fade(theme::Accent, .7f) : theme::Line, theme::Radius * scale);
		// The chevron points the way the region goes when clicked.
		const Icon chevron = vertical ? (folded ? Icon::ChevronLeft : Icon::ChevronRight)
									  : (folded ? Icon::ChevronUp : Icon::ChevronDown);
		DrawIcon(tabDraw, chevron, {(tabMin.x + tabMax.x) / 2, (tabMin.y + tabMax.y) / 2}, 12 * scale,
				 tabHovered || active ? theme::Text : theme::TextDim);
		if (tabHovered && !active)
			ImGui::SetTooltip("%s", vertical ? (folded ? "Show the inspector (Ctrl+I); drag to resize"
													   : "Hide the inspector (Ctrl+I); drag to resize")
											 : (folded ? "Show the timeline (Ctrl+T); drag to resize"
													   : "Hide the timeline (Ctrl+T); drag to resize"));
		else if (lineHovered && !active)
			ImGui::SetTooltip("Drag to resize; drag past the edge to hide; double-click resets");
		ImGui::PopID();
		return toggle;
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
		// Preview over timeline on the left; the inspector takes the full height on the
		// right with the device tabs across its top. While the inspector is folded, or
		// a narrow window makes it a drawer, the tabs become an icon rail at the edge.
		const bool compact = width / scale < 1100, overlay = width / scale < 800;
		const bool focus = previewExpanded;
		// Narrowing into the drawer layout must not cover the image with the drawer.
		if (overlay && !std::exchange(wasOverlay, true))
			drawer = false;
		wasOverlay = overlay;
		const bool inspectorFolded = settings.inspectorFolded && !overlay;
		const bool rail = !focus && (overlay || inspectorFolded);
		const float railWidth = rail ? RailWidth() : 0;
		const float inspector = focus || inspectorFolded
									? 0
									: std::min(std::min(settings.inspectorWidth, compact ? 320.0f : 520.0f) * scale,
											   std::max(0.0f, width - railWidth - 120 * scale));
		const float top = origin.y + 40 * scale, bodyHeight = std::max(1.0f, height - 40 * scale);
		const float left = std::max(1.0f, width - railWidth - (overlay ? 0 : inspector));
		const float gap = focus ? 0 : 6 * scale;
		float timelineSize = focus					? 0
							 : settings.timelineFolded ? TimelinePanel::FoldedHeight(scale)
													   : std::min(height * .5f, (!settings.timelineResized && height / scale < 560
																					 ? 56
																					 : settings.timelineHeight) *
																					scale);
		// The Library drawer needs a few rows; opening it grows a short timeline without saving that height.
		if (shell.library && !focus && !settings.timelineFolded)
			timelineSize = std::max(timelineSize, std::min(height * .5f, 220 * scale));
		const float previewHeight = std::max(1.0f, bodyHeight - gap - timelineSize);
		const float railX = origin.x + width - railWidth;
		shell.preview = {origin.x, top, left, previewHeight};
		shell.inspector = {overlay ? railX - inspector : origin.x + left, top, std::max(1.0f, inspector), bodyHeight};
		shell.inspectorVisible = !focus && !inspectorFolded && (!overlay || drawer);
		if (rail)
		{
			ImGui::SetCursorPos({railX, top});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Bg);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 4 * scale});
			ImGui::BeginChild("Device rail", {railWidth, bodyHeight}, ImGuiChildFlags_AlwaysUseWindowPadding,
							  ImGuiWindowFlags_NoScrollbar);
			ImGui::PopStyleVar();
			if (auto picked = DrawDeviceRail(shell.selection, runtime.View(), settings))
				PickTab(std::move(*picked), overlay);
			ImGui::EndChild();
			ImGui::PopStyleColor();
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
		if (!focus)
		{
			ImGui::SetCursorPos({origin.x, top + previewHeight + gap});
			ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
			ImGui::BeginChild("Timeline", {left, timelineSize}, 0,
							  settings.timelineFolded ? ImGuiWindowFlags_NoScrollbar : ImGuiWindowFlags_None);
			const bool library = shell.library;
			if (timeline.Draw(runtime, shell, settings.timelineFolded))
				ShowInspector();
			if (shell.library != library && shell.library && std::exchange(settings.timelineFolded, false))
				dirty = true;
			ImGui::EndChild();
			ImGui::PopStyleColor();
			DrawLibrary(runtime);
		}
		if (SelectionKey(shell.selection) != selected)
			drawer = true;
		if (shell.inspectorVisible)
			DrawInspectorPanel(runtime, rail);
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
			// Created after the regions so they draw and hover above them.
			if (!overlay && Divider("Inspector divider", {origin.x + left - 3 * scale, top, 6 * scale, bodyHeight}, true,
									settings.inspectorWidth, 280, compact ? 320 : 520, 340, inspectorFolded, 200))
				FoldInspector();
			if (Divider("Timeline divider", {origin.x, top + previewHeight, left, gap}, false, settings.timelineHeight, 56,
						height / scale * .5f, 132, settings.timelineFolded, 40))
				FoldTimeline();
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
	RuntimeFrameRun ReplayTarget(const RuntimePanels& runtime) const
	{
		const auto state = runtime.Snapshot();
		return {state.epoch, state.session, state.generation};
	}
	// The tab strip (unless the rail stands in for it) above the selected page.
	void DrawInspectorPanel(RuntimePanels& runtime, bool rail)
	{
		const float scale = settings.scale;
		auto area = shell.inspector;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
		if (!rail)
		{
			const float strip = DeviceTabsHeight() + 6 * scale;
			ImGui::SetCursorPos({area.x, area.y});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * scale, 4 * scale});
			ImGui::BeginChild("Device tabs", {area.width, strip}, ImGuiChildFlags_AlwaysUseWindowPadding,
							  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PopStyleVar();
			if (auto picked = DrawDeviceTabs(shell.selection, runtime.View(), settings))
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
	// The Library drawer over the timeline's left column, below its transport.
	void DrawLibrary(RuntimePanels& runtime)
	{
		const auto rect = timeline.DrawerRect();
		if (!shell.library || rect.width <= 0 || rect.height <= 0)
			return;
		const float scale = settings.scale;
		ImGui::SetCursorScreenPos({rect.x, rect.y});
		ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Surface);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * scale, 4 * scale});
		ImGui::BeginChild("Library", {rect.width, rect.height}, ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
#ifdef SIMXR_COMPANION_AUTOMATION
		const auto before = SelectionKey(shell.selection);
		if (auto run = library.DrawBrowser(shell.selection, runtime, rigs))
			QueueScript(runtime, std::move(*run));
		if (SelectionKey(shell.selection) != before)
			ShowInspector();
#else
		(void)runtime;
		ImGui::TextDisabled("Takes and scripts need a Luau-enabled build");
#endif
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}
#ifdef SIMXR_COMPANION_AUTOMATION
	void QueueScript(RuntimePanels& runtime, ScriptRun run)
	{
		runtime.PrepareReplay();
		replay = PendingReplay{{run.path}, run.options,
			std::chrono::steady_clock::now() + std::chrono::seconds(3), ReplayTarget(runtime)};
	}
#endif
	void DrawInspector(RuntimePanels& runtime)
	{
		if (std::holds_alternative<DeviceSel>(shell.selection))
			runtime.DrawDevicePage(shell);
		else if (std::holds_alternative<FrameSel>(shell.selection))
			runtime.DrawFramePage(shell);
		else if (const auto range = std::get_if<RangeSel>(&shell.selection))
			runtime.DrawRangePage(RangeSel{*range}, shell);
		else if (std::holds_alternative<AdvancedSel>(shell.selection))
		{
			PageTitle(Icon::StepForward, "Advanced stepping");
			if (PageClose())
				shell.selection = std::monostate{};
			else
				runtime.DrawAdvancedPage();
		}
		else if (std::holds_alternative<TakeSel>(shell.selection) || std::holds_alternative<ScriptSel>(shell.selection))
		{
#ifdef SIMXR_COMPANION_AUTOMATION
			if (auto run = library.Draw(shell.selection, runtime, rigs))
				QueueScript(runtime, std::move(*run));
#else
			ImGui::TextDisabled("Takes and scripts need a Luau-enabled build");
#endif
		}
		// A page's × (or its file deleted) returns to the device page it was opened from.
		if (std::holds_alternative<std::monostate>(shell.selection))
			shell.selection = shell.device;
	}
	void Draw(RuntimePanels& runtime)
	{
		PollLaunch(runtime);
		runtime.BeginFrame();
		timeline.Observe(runtime.View());
#ifdef SIMXR_COMPANION_AUTOMATION
		if (auto recording = runtime.TakeRecordingReplay())
		{
			runtime.PrepareReplay();
			replay = PendingReplay{std::move(*recording), {},
				std::chrono::steady_clock::now() + std::chrono::seconds(3), ReplayTarget(runtime)};
		}
		if (replay && replay->target != ReplayTarget(runtime))
		{
			replay.reset();
			runtime.FinishReplayPreparation();
			notices.Push({"Replay target changed", "Select the intended session and replay again"});
		}
		const auto rigRuntime = runtime.Snapshot();
		const RigTarget rigTarget{rigRuntime.endpoint, rigRuntime.session, rigRuntime.generation};
		if (replay)
		{
			if (runtime.ReplayReady())
			{
				if (replay->script)
					rigs.RunScript(rigTarget, replay->recording.path, *replay->script);
				else
				{
					rigs.ReplayRecording(rigTarget, replay->recording.path, replay->recording.seed);
					timeline.ReplayTake(replay->recording.path, rigRuntime, rigs.RunId());
				}
				replay.reset();
				runtime.FinishReplayPreparation();
			}
			else if (std::chrono::steady_clock::now() >= replay->deadline)
			{
				replay.reset();
				runtime.FinishReplayPreparation();
				notices.Push({"Replay unavailable", "Input release did not complete. Release inputs and try again"});
			}
		}
		runtime.SetReplayActive(rigs.IsActive());
		timeline.ObserveRig(rigRuntime, rigs.RunId(), rigs.Snapshot(), rigs.MatchesTarget(rigTarget));
		shell.timeline.playing = rigs.IsActive() && rigs.ReplayingTake();
		shell.timeline.playbackPaused = rigs.Snapshot().phase == automation::RigPhase::Paused;
#endif
		DrawShell(runtime);
#ifdef SIMXR_COMPANION_AUTOMATION
		if (std::exchange(shell.timeline.togglePlayback, false))
			shell.timeline.playbackPaused ? rigs.Resume() : rigs.Pause();
		if (std::exchange(shell.timeline.stopReplay, false))
			rigs.Cancel();
		// A replay or script that fails says so once; its page has the details.
		const auto rig = rigs.Snapshot();
		const bool failed = rig.phase == automation::RigPhase::Failed || !rigs.Error().empty();
		if (failed && !std::exchange(replayFailed, true))
		{
			const bool take = rigs.ReplayingTake();
			notices.Push({take ? "Replay failed" : "Script failed", rigs.Error().empty() ? rig.error : rigs.Error(),
						  "Details", [this, take, path = rigs.Script()]
						  {
							  shell.selection = take ? Selection{TakeSel{path}} : Selection{ScriptSel{path}};
							  ShowInspector();
						  }});
		}
		replayFailed = failed;
#endif
		if (auto saved = runtime.TakeSavedTake())
		{
			timeline.SavedRecording(*saved, runtime.View());
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
		runtime.EndFrame();
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
void WorkspacePanels::ShowLibrary(bool open)
{
	if (m_impl->shell.library != open)
		m_impl->ToggleLibrary();
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
