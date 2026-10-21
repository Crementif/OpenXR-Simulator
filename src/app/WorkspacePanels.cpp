#include "app/WorkspacePanels.h"
#include "app/ProcessLauncher.h"
#include "app/RuntimePanels.h"
#include "simulation/HeadsetProfiles.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/RigPanel.h"
#endif

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
	bool showConnection = false;
	WorkspaceMode mode = WorkspaceMode::Simulator;
	std::filesystem::path path;
	CompanionSettings settings;
	ProcessLauncher launcher;
#ifdef SIMXR_COMPANION_AUTOMATION
	RigPanel rigs;
#endif
	std::string error, pendingLayout;
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
		if (!endpoint.empty())
			settings.endpoint = endpoint;
		for (std::size_t index = 0; index < settings.profiles.size(); ++index)
			if (settings.profiles[index].name == settings.activeProfile)
				EditProfile(static_cast<int>(index));
		for (const auto& layout : settings.layouts)
			if (layout.name == settings.activeLayout)
				pendingLayout = layout.ini;
		if (!pendingLayout.empty())
			mode = WorkspaceMode::Custom;
		Copy(layoutName, settings.activeLayout.empty() ? "My layout" : settings.activeLayout);
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
	void StoreLayout()
	{
		const std::string name(layoutName.data());
		if (name.empty())
			throw std::invalid_argument("Give the layout a name");
		std::size_t size = 0;
		const auto ini = ImGui::SaveIniSettingsToMemory(&size);
		auto candidate = settings;
		auto found = std::find_if(candidate.layouts.begin(), candidate.layouts.end(),
								  [&](const auto& value)
								  {
									  return value.name == name;
								  });
		if (found == candidate.layouts.end())
			candidate.layouts.push_back({name, std::string(ini, size)});
		else
			found->ini.assign(ini, size);
		candidate.activeLayout = name;
		ValidateSettings(candidate);
		settings = std::move(candidate);
		dirty = true;
		Save();
	}
	void BeginFrame(RuntimePanels& runtime)
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
		if (!pendingLayout.empty())
		{
			ImGui::LoadIniSettingsFromMemory(pendingLayout.data(), pendingLayout.size());
			pendingLayout.clear();
		}
		DrawMainMenu(runtime);
		DrawToolbar(runtime);
	}
	void SelectMode(WorkspaceMode selected)
	{
		mode = selected;
		previewExpanded = false;
	}
	void LaunchSelected()
	{
		try
		{
			const auto profile = Profile();
#ifdef SIMXR_COMPANION_AUTOMATION
			rigs.PrepareLaunch(profile);
#else
			if (!profile.rig.empty())
				throw std::runtime_error("This build does not support launch rigs");
#endif
			launcher.Launch(profile);
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
#ifdef SIMXR_COMPANION_AUTOMATION
			rigs.PrepareLaunch({});
#endif
		}
	}
	void DrawToolbar(RuntimePanels& runtime)
	{
		const auto snapshot = runtime.Snapshot();
		auto* viewport = ImGui::GetMainViewport();
		const float row = ImGui::GetFrameHeightWithSpacing();
		if (!ImGui::BeginViewportSideBar("##Workspace toolbar", viewport, ImGuiDir_Up,
										 row * 2 + ImGui::GetStyle().WindowPadding.y * 2,
										 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
		{
			ImGui::End();
			return;
		}
		for (const auto view : {WorkspaceMode::Simulator, WorkspaceMode::Debug, WorkspaceMode::Automation})
		{
			const auto label = view == WorkspaceMode::Simulator ? "Simulate"
							   : view == WorkspaceMode::Debug	? "Inspect"
																: "Record & replay";
			const bool active = view == mode;
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if (ImGui::Button(label))
				SelectMode(view);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"%s", view == WorkspaceMode::Simulator ? "Pilot the head and hands; configure simulated devices."
						  : view == WorkspaceMode::Debug ? "Inspect captured frames, submitted images and application "
														   "state. Switching tools does not pause the app."
														 : "Record simulated input, save sequences and replay them.");
			if (active)
				ImGui::PopStyleColor();
			ImGui::SameLine();
		}
		if (ImGui::Button(previewExpanded ? "Show tools" : "Expand view"))
			previewExpanded = !previewExpanded;
		ImGui::SameLine();
		if (ImGui::Button("Release inputs"))
			runtime.ReleaseInputs();
		ImGui::SameLine();
		const bool paused = snapshot.connected && snapshot.sessionAvailable && snapshot.manual;
		const bool inspecting = !snapshot.following && snapshot.selectedFrame;
		const bool disconnectedImage = !snapshot.connected && snapshot.selectedFrame;
		const ImVec4 accent = !snapshot.connected ? ImVec4{0.62f, 0.66f, 0.72f, 1}
							  : paused			  ? ImVec4{0.96f, 0.69f, 0.32f, 1}
							  : inspecting		  ? ImVec4{0.48f, 0.72f, 0.95f, 1}
												  : ImVec4{0.35f, 0.80f, 0.65f, 1};
		ImGui::TextColored(accent, "%s",
						   !snapshot.connected ? (disconnectedImage ? "Disconnected / image retained" : "Disconnected")
						   : !snapshot.sessionAvailable ? "Waiting for application"
						   : snapshot.operationPending	? "Updating..."
						   : paused						? "Application paused"
						   : inspecting					? "Inspecting / app running"
														: "Application running");
		if (paused || inspecting || disconnectedImage)
		{
			const auto position = viewport->Pos, size = viewport->Size;
			const float inset = 2 * settings.scale;
			ImGui::GetForegroundDrawList(viewport)->AddRect(
				{position.x + inset, position.y + inset}, {position.x + size.x - inset, position.y + size.y - inset},
				ImGui::ColorConvertFloat4ToU32(accent), 0, 0, 3 * settings.scale);
		}
		ImGui::SetNextItemWidth(std::max(100.0f, std::min(220.0f * settings.scale, viewport->WorkSize.x * 0.30f)));
		if (ImGui::BeginCombo("##Launch profile",
							  selectedProfile >= 0
								  ? settings.profiles[static_cast<std::size_t>(selectedProfile)].name.c_str()
								  : "Choose application"))
		{
			for (std::size_t index = 0; index < settings.profiles.size(); ++index)
				if (ImGui::Selectable(settings.profiles[index].name.c_str(),
									  selectedProfile == static_cast<int>(index)))
					EditProfile(static_cast<int>(index));
			if (ImGui::Selectable("Add or edit applications..."))
			{
				showLaunch = true;
				previewExpanded = false;
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		const auto process = launcher.Snapshot();
		ImGui::BeginDisabled(process.state == LaunchState::Starting || process.state == LaunchState::Running);
		if (ImGui::Button(selectedProfile >= 0 ? "Launch" : "Set up"))
		{
			previewExpanded = false;
			if (selectedProfile >= 0)
				LaunchSelected();
			else
				showLaunch = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(snapshot.connected ? "Connection" : "Connect"))
		{
			showConnection = !showConnection;
			previewExpanded = false;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!snapshot.sessionAvailable || snapshot.operationPending);
		if (ImGui::Button(snapshot.manual ? "Continue" : "Pause"))
		{
			if (snapshot.manual)
				runtime.ResumeLive();
			else
				runtime.Command(control::CommandKind::Pause);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", !snapshot.sessionAvailable ? "Connect to a running XR application first"
									: snapshot.operationPending
										? "Waiting for the runtime to finish the previous operation"
										: "F6 pauses. F8 continues the application and follows live output.");
		ImGui::SameLine();
		ImGui::BeginDisabled(!snapshot.sessionAvailable || !snapshot.manual || snapshot.operationPending);
		if (ImGui::Button("Next frame"))
			runtime.Command(control::CommandKind::Step);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Pause the application, then advance one XR frame (F7)");
		ImGui::End();
		runtime.SetPreviewExpanded(previewExpanded);
	}
	void DrawDockspace()
	{
		const auto dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
													   previewExpanded ? ImGuiDockNodeFlags_KeepAliveOnly
																	   : ImGuiDockNodeFlags_AutoHideTabBar);
		if (previewExpanded)
			return;
		const auto* node = ImGui::DockBuilderGetNode(dock);
		if (!reset && node && node->IsSplitNode())
			return;
		reset = false;
		ImGui::DockBuilderRemoveNode(dock);
		ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
		const auto size = ImGui::GetMainViewport()->WorkSize;
		ImGui::DockBuilderSetNodeSize(dock, size);
		ImGuiID preview, controls;
		const float controlWidth = std::clamp(360.0f * settings.scale / std::max(size.x, 1.0f), 0.18f, 0.48f);
		ImGui::DockBuilderSplitNode(dock, ImGuiDir_Right, controlWidth, &controls, &preview);
		ImGui::DockBuilderDockWindow("GPU preview", preview);
		// Every tool has a stable home, even while another tool is selected.
		for (const auto name : {"Device controls", "Rigs", "Recording", "Frame controls", "Captures",
								"Historical state", "Action and binding inspector", "Runtime connection",
								"Workspace settings", "Launch profiles", "Workspace error"})
			ImGui::DockBuilderDockWindow(name, controls);
		ImGui::DockBuilderFinish(dock);
	}
	void DrawMainMenu(RuntimePanels& runtime)
	{
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("Simulator"))
			{
				if (ImGui::MenuItem("Launch profiles", nullptr, &showLaunch))
					previewExpanded = false;
				if (ImGui::MenuItem("Workspace settings", nullptr, &showSettings))
					previewExpanded = false;
				ImGui::Separator();
				if (ImGui::MenuItem("Pause", "F6"))
					runtime.Command(control::CommandKind::Pause);
				if (ImGui::MenuItem("Step", "F7"))
					runtime.Command(control::CommandKind::Step);
				if (ImGui::MenuItem("Continue", "F8"))
					runtime.ResumeLive();
				if (ImGui::MenuItem("Cancel pending operation", "Shift+F7"))
					runtime.CancelPending();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Layouts"))
			{
				for (const auto& layout : settings.layouts)
					if (ImGui::MenuItem(layout.name.c_str(), nullptr, settings.activeLayout == layout.name))
					{
						pendingLayout = layout.ini;
						settings.activeLayout = layout.name;
						mode = WorkspaceMode::Custom;
						dirty = true;
						Save();
					}
				ImGui::Separator();
				if (ImGui::MenuItem("Save named layout…"))
					showSettings = true;
				if (ImGui::MenuItem("Reset to default"))
				{
					reset = true;
					mode = WorkspaceMode::Simulator;
					settings.activeLayout.clear();
					dirty = true;
					Save();
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
		if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive())
		{
			if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
				runtime.Command(control::CommandKind::Pause);
			if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
			{
				if (ImGui::GetIO().KeyShift)
					runtime.CancelPending();
				else
					runtime.Command(control::CommandKind::Step);
			}
			if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
				runtime.ResumeLive();
		}
	}
	void Draw(RuntimePanels& runtime)
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		if (const auto recording = runtime.TakeRecordingReplay())
			rigs.ReplayRecording(runtime, recording->path, recording->seed);
		if (enabled)
			rigs.ObserveLaunch(launcher.Snapshot());
		rigs.Draw(runtime, !previewExpanded && (mode == WorkspaceMode::Automation || mode == WorkspaceMode::Custom));
#endif
		if (previewExpanded)
			return;
		if (showConnection)
			runtime.DrawConnection(&showConnection);
		if (!error.empty())
		{
			ImGui::Begin("Workspace error");
			ImGui::TextWrapped("%s", error.c_str());
			if (ImGui::Button("Dismiss"))
				error.clear();
			ImGui::End();
		}
		if (showSettings)
		{
			ImGui::SetNextWindowSize({480, 380}, ImGuiCond_FirstUseEver);
			ImGui::Begin("Workspace settings", &showSettings);
			ImGui::TextWrapped("Settings: %s", path.empty() ? "unavailable" : path.string().c_str());
			if (ImGui::SliderFloat("UI scale", &settings.scale, 0.75f, 2.5f, "%.2f"))
				dirty = true;
			if (ImGui::Button("Save preferences"))
			{
				settings.endpoint = runtime.Endpoint();
				dirty = true;
				Save();
			}
			ImGui::SeparatorText("Named layouts");
			ImGui::InputText("Layout name", layoutName.data(), layoutName.size());
			if (ImGui::Button("Save current layout"))
				try
				{
					StoreLayout();
				}
				catch (const std::exception& failure)
				{
					error = failure.what();
				}
			for (std::size_t index = 0; index < settings.layouts.size(); ++index)
			{
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::Button("Load"))
				{
					pendingLayout = settings.layouts[index].ini;
					settings.activeLayout = settings.layouts[index].name;
					mode = WorkspaceMode::Custom;
					dirty = true;
					Save();
				}
				ImGui::SameLine();
				ImGui::TextUnformatted(settings.layouts[index].name.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Delete"))
				{
					if (settings.activeLayout == settings.layouts[index].name)
						settings.activeLayout.clear();
					settings.layouts.erase(settings.layouts.begin() + static_cast<std::ptrdiff_t>(index));
					dirty = true;
					Save();
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::TextWrapped("F6 Pause · F7 Next frame · Shift+F7 Cancel · F8 Continue. Shortcuts are disabled while "
							   "editing fields.");
			ImGui::End();
		}
		if (showLaunch)
		{
			ImGui::SetNextWindowSize({620, 650}, ImGuiCond_FirstUseEver);
			ImGui::Begin("Launch profiles", &showLaunch);
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
			ImGui::End();
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
void WorkspacePanels::DrawDockspace()
{
	m_impl->DrawDockspace();
}
WorkspaceMode WorkspacePanels::Mode() const
{
	return m_impl->mode;
}
void WorkspacePanels::SetMode(WorkspaceMode mode)
{
	m_impl->SelectMode(mode);
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
