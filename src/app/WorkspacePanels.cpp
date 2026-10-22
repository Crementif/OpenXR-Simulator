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

#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <stdexcept>

namespace simxr::app
{
struct WorkspacePanels::Impl
{
	bool enabled = false, showLaunch = false, showSettings = false, reset = false, dirty = false;
	bool previewExpanded = false;

	bool showConnection = false;
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
		ImGui::SetNextItemWidth(180 * settings.scale);
		if (ImGui::BeginCombo("##App picker", profiles.Selected() >= 0 ? settings.activeProfile.c_str() : "App"))
		{
			ImGui::SeparatorText("Running now");
			for (const auto& endpoint : DiscoverRuntimeEndpoints())
			{
				ImGui::PushID(endpoint.c_str());
				if (ImGui::Selectable(std::filesystem::path(endpoint).filename().string().c_str(),
									  endpoint == snapshot.endpoint))
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
			if (ImGui::Selectable("Edit profiles"))
				showLaunch = true;
			if (ImGui::Selectable("Connect endpoint"))
				showConnection = true;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		const auto chipStart = ImGui::GetCursorScreenPos();
		const auto label = RunStateLabel(shell.session.state);
		const auto labelSize = ImGui::CalcTextSize(label);
		const auto chipSize = ImVec2{labelSize.x + 24 * settings.scale, ImGui::GetFrameHeight()};
		const auto color =
			ImGui::GetColorU32(shell.session.state == RunState::Running ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
		ImGui::GetWindowDrawList()->AddRectFilled(chipStart, {chipStart.x + chipSize.x, chipStart.y + chipSize.y},
												  ImGui::GetColorU32(ImGuiCol_FrameBg), 12 * settings.scale);
		ImGui::GetWindowDrawList()->AddCircleFilled({chipStart.x + 8 * settings.scale, chipStart.y + chipSize.y / 2},
													3 * settings.scale, color);
		ImGui::GetWindowDrawList()->AddText(
			{chipStart.x + 16 * settings.scale, chipStart.y + (chipSize.y - labelSize.y) / 2},
			ImGui::GetColorU32(ImGuiCol_Text), label);
		ImGui::Dummy(chipSize);
		ImGui::SameLine();
		if (ImGui::Button("Library"))
		{
			shell.selection = LibrarySel{};
			drawer = true;
		}
		ImGui::SameLine();
		if (ImGui::Button(snapshot.recording ? "Stop recording" : "Rec"))
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
		if (ImGui::Button("..."))
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
#ifdef SIMXR_COMPANION_AUTOMATION
		if (rigs.HasStatus())
		{
			ImGui::SameLine();
			rigs.DrawTransport();
		}
#endif
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
		static constexpr const char* railLabels[] = {"Player", "Head",	   "Left",	  "Right",	  "Body",
													 "Gaze",   "Trackers", "Headset", "Scenarios"};
		// Pilot target keys shown beside the device rows when the rail has room for them.
		static constexpr PilotAction railKeys[] = {PilotAction::TargetPlayer, PilotAction::TargetHead,
												   PilotAction::TargetLeft, PilotAction::TargetRight};
		float labelWidth = 0;
		for (const auto* label : railLabels)
			labelWidth = std::max(labelWidth, ImGui::CalcTextSize(label).x);
		const float narrowRail = labelWidth + (ImGui::GetStyle().WindowPadding.x + 4 * scale) * 2;
		const bool narrow = compact || settings.railCollapsed;
		const float rail = previewExpanded ? 0 : narrow ? narrowRail : 176 * scale;
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
			ImGui::BeginChild("Device rail", {rail, bodyHeight}, 0, ImGuiWindowFlags_NoScrollbar);
			for (int index = 0; index < 9; ++index)
			{
				ImGui::PushID(index);
				bool selected = false;
				if (const auto device = std::get_if<DeviceSel>(&shell.selection))
					selected = index == static_cast<int>(device->page);
				if (const auto item = std::get_if<SessionItemSel>(&shell.selection))
					selected = index == 7 + static_cast<int>(item->item);
				if (ImGui::Selectable(railLabels[index], selected, 0, {0, 28 * scale}))
				{
					if (index < 7)
						shell.selection = DeviceSel{static_cast<DevicePage>(index)};
					else
						shell.selection = SessionItemSel{static_cast<SessionItem>(index - 7)};
					drawer = true;
				}
				if (index < 4)
				{
					const auto key = PilotBindingLabel(settings.pilotBindings[static_cast<std::size_t>(railKeys[index])]);
					if (!narrow)
					{
						const auto size = ImGui::CalcTextSize(key.c_str());
						const auto row = ImGui::GetItemRectMax();
						ImGui::GetWindowDrawList()->AddText(
							{row.x - size.x - 6 * scale, row.y - (28 * scale + size.y) / 2},
							ImGui::GetColorU32(ImGuiCol_TextDisabled), key.c_str());
					}
					ImGui::SetItemTooltip("%s (pilot with %s)", railLabels[index], key.c_str());
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
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
			ImGui::BeginChild("Inspector", {shell.inspector.width, shell.inspector.height});
			DrawInspector(runtime);
			ImGui::EndChild();
		}
		for (auto& notice : runtime.TakeNotices())
			notices.Push(std::move(notice));
		const auto window = ImGui::GetWindowPos();
		notices.Draw({window.x + shell.preview.x, window.y + shell.preview.y, shell.preview.width, shell.preview.height});
		if (!previewExpanded)
		{
			ImGui::SetCursorPos({origin.x, top + bodyHeight + 6 * scale});
			ImGui::BeginChild("Timeline", {width, timelineSize});
			if (timeline.Draw(runtime, shell))
			{
				drawer = true;
			}
			ImGui::EndChild();
		}
		const auto state = shell.session.state;
		const ImU32 border = state == RunState::Paused		   ? IM_COL32(240, 164, 58, 255)
							 : !runtime.View().time.following ? IM_COL32(76, 141, 255, 255)
							 : state == RunState::Disconnected ? IM_COL32(139, 147, 161, 255)
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
		if (const auto recording = runtime.TakeRecordingReplay())
			rigs.ReplayRecording(runtime, recording->path, recording->seed);
		rigs.Draw(runtime, false);
		runtime.SetReplayActive(rigs.IsActive());
		timeline.ObserveRig(runtime.Snapshot(), rigs.RunId(), rigs.Snapshot(), rigs.MatchesTarget(runtime.Snapshot()));
#endif
		DrawShell(runtime);
		if (showConnection)
			runtime.DrawConnection(&showConnection);
		if (!error.empty())
		{
			ImGui::OpenPopup("Error details");
			if (ImGui::BeginPopupModal("Error details"))
			{
				ImGui::TextWrapped("%s", error.c_str());
				if (ImGui::Button("Dismiss"))
				{
					error.clear();
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
