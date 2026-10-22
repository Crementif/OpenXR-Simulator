#include "app/WorkspacePanels.h"
#include "app/LaunchAttachment.h"
#include "app/ProcessLauncher.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimeEndpoints.h"
#include "app/RuntimePanels.h"
#include "app/TimelinePanel.h"
#include "simulation/HeadsetProfiles.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/RigPanel.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <map>
#include <stdexcept>
#include <string_view>

namespace simxr::app
{
namespace
{
std::vector<std::string> Lines(const char* text)
{
	std::vector<std::string> result;
	std::string_view remaining(text);
	while (!remaining.empty())
	{
		const auto end = remaining.find('\n');
		result.emplace_back(remaining.substr(0, end));
		if (!result.back().empty() && result.back().back() == '\r')
			result.back().pop_back();
		if (end == std::string_view::npos)
			break;
		remaining.remove_prefix(end + 1);
	}
	return result;
}
std::string Join(const std::vector<std::string>& lines)
{
	std::string result;
	for (const auto& line : lines)
	{
		if (!result.empty())
			result += '\n';
		result += line;
	}
	return result;
}
template <std::size_t N> void Copy(std::array<char, N>& buffer, const std::string& text)
{
	std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
}
} // namespace
struct WorkspacePanels::Impl
{
	bool enabled = false, showLaunch = false, showSettings = false, reset = false, dirty = false;
	bool previewExpanded = false;
	int bindingCapture = -1, bindingCaptureFrame = 0;

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
	std::string error, iniPath;
	std::array<char, 128> layoutName{}, profileName{};
	std::array<char, 2048> executable{}, workingDirectory{}, manifest{}, rig{};
	std::array<char, 256> headset{};
	std::array<char, 16384> arguments{}, environment{};
	int selectedProfile = -1;
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
				EditProfile(static_cast<int>(index));
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
	void EditProfile(int index)
	{
		selectedProfile = index;
		LaunchProfile profile;
		if (index >= 0)
			profile = settings.profiles.at(static_cast<std::size_t>(index));
		Copy(profileName, profile.name);
		Copy(executable, profile.executable);
		Copy(workingDirectory, profile.workingDirectory);
		Copy(manifest, profile.runtimeManifest);
		Copy(rig, profile.rig);
		Copy(headset, profile.headsetProfile);
		Copy(arguments, Join(profile.arguments));
		std::vector<std::string> values;
		for (const auto& [key, value] : profile.environment)
			values.push_back(key + "=" + value);
		Copy(environment, Join(values));
	}
	LaunchProfile Profile() const
	{
		LaunchProfile profile;
		profile.name = profileName.data();
		profile.executable = executable.data();
		profile.arguments = Lines(arguments.data());
		profile.workingDirectory = workingDirectory.data();
		profile.runtimeManifest = manifest.data();
		profile.headsetProfile = headset.data();
		profile.rig = rig.data();
		for (const auto& line : Lines(environment.data()))
		{
			const auto equals = line.find('=');
			if (equals == std::string::npos)
				throw std::invalid_argument("Environment lines must be KEY=value");
			profile.environment.emplace_back(line.substr(0, equals), line.substr(equals + 1));
		}
		return profile;
	}
	void StoreProfile()
	{
		auto candidate = settings;
		auto profile = Profile();
		if (selectedProfile < 0)
			candidate.profiles.push_back(profile);
		else
			candidate.profiles.at(static_cast<std::size_t>(selectedProfile)) = profile;
		candidate.activeProfile = profile.name;
		ValidateSettings(candidate);
		settings = std::move(candidate);
		if (selectedProfile < 0)
			selectedProfile = static_cast<int>(settings.profiles.size()) - 1;
		dirty = true;
		Save();
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
			const auto profile = Profile();
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
		const auto snapshot = runtime.Snapshot();
		shell.session = DeriveRunState(snapshot, attachment != nullptr, error);
		shell.time.following = snapshot.following;
		shell.time.cursor = snapshot.following ? FrameIdentity{snapshot.session, snapshot.generation, snapshot.latestFrame}
			: snapshot.selectedIdentity;
		shell.time.epoch = snapshot.following ? snapshot.epoch : snapshot.selectedEpoch;
		if (std::holds_alternative<DeviceSel>(shell.selection) || std::holds_alternative<SessionItemSel>(shell.selection))
			shell.device = shell.selection;
		if (const auto stepped = runtime.TakeSteppedFrame();
			stepped && (stepped->requested || std::holds_alternative<FrameSel>(shell.selection)))
		{
			shell.selection = stepped->frame;
			drawer = drawer || stepped->requested;
		}
		ImGui::SetNextItemWidth(180 * settings.scale);
		if (ImGui::BeginCombo("##App picker", selectedProfile >= 0 ? settings.activeProfile.c_str() : "App"))
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
					EditProfile(static_cast<int>(index));
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
				shell.selection = FrameSel{shell.time.cursor, shell.time.epoch};
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
		runtime.SetPreviewExpanded(previewExpanded);
		runtime.Draw(shell);
#ifdef SIMXR_COMPANION_AUTOMATION
		if (shell.inspectorVisible && std::holds_alternative<LibrarySel>(shell.selection))
		{
			ImGui::SetCursorPos({shell.inspector.x, shell.inspector.y});
			ImGui::BeginChild("Inspector", {shell.inspector.width, shell.inspector.height});
			rigs.Draw(runtime, true);
			ImGui::EndChild();
		}
#endif
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
							 : !shell.time.following		   ? IM_COL32(76, 141, 255, 255)
							 : state == RunState::Disconnected ? IM_COL32(139, 147, 161, 255)
															   : 0;
		if (border)
			ImGui::GetForegroundDrawList()->AddRect(
				viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y}, border, 0, 0,
				2 * scale);
		ImGui::End();
	}
	void DrawKeymap()
	{
		if (ImGui::BeginTabBar("Preference pages"))
		{
			if (ImGui::BeginTabItem("Keyboard & mouse"))
			{
				if (ImGui::Button("Reset keymap"))
				{
					settings.pilotBindings = DefaultPilotBindings();
					dirty = true;
					bindingCapture = -1;
				}
				if (ImGui::BeginTable("Bindings", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, {0, 320}))
				{
					ImGui::TableSetupColumn("Action");
					ImGui::TableSetupColumn("Binding");
					ImGui::TableHeadersRow();
					for (std::size_t index = 0; index < settings.pilotBindings.size(); ++index)
					{
						auto& binding = settings.pilotBindings[index];
						ImGui::PushID(static_cast<int>(index));
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(PilotActionNames[index]);
						ImGui::TableNextColumn();
						std::string label =
							bindingCapture == static_cast<int>(index) ? "Press a key…" : PilotBindingLabel(binding);
						if (ImGui::Button(label.c_str(), {-1, 0}))
						{
							bindingCapture = static_cast<int>(index);
							bindingCaptureFrame = ImGui::GetFrameCount();
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
				if (bindingCapture >= 0 && ImGui::GetFrameCount() > bindingCaptureFrame + 1)
				{
					if (ImGui::IsKeyPressed(ImGuiKey_Escape))
						bindingCapture = -1;
					else
						for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
						{
							if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown)
								continue;
							if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
								continue;
							const auto& io = ImGui::GetIO();
							auto& binding = settings.pilotBindings[static_cast<std::size_t>(bindingCapture)];
							binding.key = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
							binding.modifiers = (io.KeyCtrl ? 1 : 0) | (io.KeyShift ? 2 : 0) | (io.KeyAlt ? 4 : 0) |
												(io.KeySuper ? 8 : 0);
							bindingCapture = -1;
							dirty = true;
							break;
						}
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Gamepad"))
			{
				auto& bindings = settings.gamepadBindings;
				dirty |= ImGui::SliderFloat("Deadzone", &bindings.deadzone, 0, .5f, "%.2f");
				const auto mapping = [&](const char* label, std::string& name, bool axis)
				{
					if (ImGui::BeginCombo(label, name.c_str()))
					{
						for (int index = 0;
							 index < (axis ? int(SDL_GAMEPAD_AXIS_COUNT) : int(SDL_GAMEPAD_BUTTON_COUNT)); ++index)
						{
							const char* value =
								axis ? SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(index))
									 : SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(index));
							if (ImGui::Selectable(value, name == value))
							{
								name = value;
								dirty = true;
							}
						}
						ImGui::EndCombo();
					}
				};
				if (ImGui::BeginTable("Gamepad mapping", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, {0, 300}))
				{
					ImGui::TableSetupColumn("Input");
					ImGui::TableSetupColumn("Left");
					ImGui::TableSetupColumn("Right");
					ImGui::TableHeadersRow();
					int row = 0;
					for (const auto& [label, entries] : {std::pair{"Stick X", &bindings.stickX},
														 {"Stick Y", &bindings.stickY},
														 {"Trigger", &bindings.trigger},
														 {"Grip", &bindings.grip},
														 {"Primary", &bindings.primary},
														 {"Secondary", &bindings.secondary},
														 {"Menu", &bindings.menu},
														 {"Stick click", &bindings.stickClick}})
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(label);
						ImGui::PushID(row);
						for (int hand = 0; hand < 2; ++hand)
						{
							ImGui::TableNextColumn();
							ImGui::PushID(hand);
							ImGui::SetNextItemWidth(-1);
							mapping("##Mapping", (*entries)[hand], row < 3);
							ImGui::PopID();
						}
						ImGui::PopID();
						++row;
					}
					ImGui::EndTable();
				}
				mapping("Hold for gyro", bindings.gyroHold, false);
				if (ImGui::Button("Reset gamepad"))
				{
					bindings = {};
					dirty = true;
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	void Draw(RuntimePanels& runtime)
	{
		PollLaunch(runtime);
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
		if (showSettings)
			ImGui::OpenPopup("Workspace settings");
		ImGui::SetNextWindowSize({620, 500}, ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints({400, 260}, ImGui::GetMainViewport()->WorkSize);
		if (ImGui::BeginPopupModal("Workspace settings", &showSettings))
		{
			if (ImGui::SliderFloat("UI scale", &settings.scale, 0.75f, 2.5f, "%.2f"))
				dirty = true;
			int budget = static_cast<int>(settings.snapshotBudgetMiB);
			if (ImGui::SliderInt("Snapshots (MiB)", &budget, SnapshotBudget::MinMiB, SnapshotBudget::MaxMiB))
			{
				settings.snapshotBudgetMiB = static_cast<std::uint32_t>(budget);
				dirty = true;
			}
			DrawKeymap();
			if (ImGui::Button("Close"))
			{
				showSettings = false;
				bindingCapture = -1;
				settings.endpoint = runtime.Endpoint();
				Save();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (showLaunch)
			ImGui::OpenPopup("Launch profiles");
		ImGui::SetNextWindowSize({620, 500}, ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints({400, 260}, ImGui::GetMainViewport()->WorkSize);
		if (ImGui::BeginPopupModal("Launch profiles", &showLaunch))
		{
			ImGui::SetNextWindowSize({620, 650}, ImGuiCond_FirstUseEver);
			if (ImGui::BeginCombo("Profile",
								  selectedProfile >= 0
									  ? settings.profiles[static_cast<std::size_t>(selectedProfile)].name.c_str()
									  : "New profile"))
			{
				for (std::size_t index = 0; index < settings.profiles.size(); ++index)
					if (ImGui::Selectable(settings.profiles[index].name.c_str(),
										  selectedProfile == static_cast<int>(index)))
						EditProfile(static_cast<int>(index));
				ImGui::EndCombo();
			}
			if (ImGui::Button("New profile"))
				EditProfile(-1);
			if (selectedProfile >= 0)
			{
				ImGui::SameLine();
				if (ImGui::Button("Delete profile"))
				{
					settings.profiles.erase(settings.profiles.begin() + selectedProfile);
					settings.activeProfile.clear();
					EditProfile(-1);
					dirty = true;
					Save();
				}
			}
			ImGui::InputText("Name", profileName.data(), profileName.size());
			ImGui::InputText("Executable", executable.data(), executable.size());
			ImGui::TextUnformatted("Arguments: one literal argument per line; no shell expansion");
			ImGui::InputTextMultiline("##arguments", arguments.data(), arguments.size(), {-1, 90});
			ImGui::InputText("Working directory", workingDirectory.data(), workingDirectory.size());
			ImGui::InputText("OpenXR runtime JSON", manifest.data(), manifest.size());
			ImGui::TextUnformatted("Child environment: KEY=value per line (including WINEPREFIX / Proton values)");
			ImGui::InputTextMultiline("##environment", environment.data(), environment.size(), {-1, 90});
			if (ImGui::BeginCombo("Headset profile", headset[0] ? headset.data() : "Runtime default"))
			{
				if (ImGui::Selectable("Runtime default", !headset[0]))
					headset.fill(0);
				for (const auto& profile : HeadsetProfiles())
					if (ImGui::Selectable(profile.id, std::string_view(profile.id) == headset.data()))
						Copy(headset, profile.id);
				ImGui::EndCombo();
			}
			ImGui::InputText("Luau rig after launch", rig.data(), rig.size());
			ImGui::TextWrapped("The headset profile applies to the launched runtime. A rig runs only after the new "
							   "child endpoint exposes one running session; an existing connection is never reused.");
			if (ImGui::Button("Save profile"))
				try
				{
					StoreProfile();
				}
				catch (const std::exception& failure)
				{
					error = failure.what();
				}
			ImGui::SameLine();
			const auto process = launcher.Snapshot();
			ImGui::BeginDisabled(process.state == LaunchState::Starting || process.state == LaunchState::Running);
			if (ImGui::Button("Launch"))
				LaunchSelected();
			ImGui::EndDisabled();
			if (process.process)
				ImGui::Text("Process: %llu", static_cast<unsigned long long>(process.process));
			if (!process.detail.empty())
				ImGui::TextWrapped("%s", process.detail.c_str());
			ImGui::TextWrapped("The runtime manifest and environment apply only to the child. After launch, use Find "
							   "runtimes to attach. Closing the companion leaves the launched application running.");
			ImGui::EndPopup();
		}
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
