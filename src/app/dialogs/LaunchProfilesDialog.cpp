#include "app/dialogs/LaunchProfilesDialog.h"
#include "Pch.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include "simulation/HeadsetProfiles.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_filesystem.h>
#include <exception>
#include <imgui.h>
#include <mutex>
#include <set>

namespace simxr::app
{
static std::vector<std::string> Lines(const char* text)
{
	std::vector<std::string> result;
	std::string_view remaining(text);
	if (remaining.empty())
		return result;
	for (;;)
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
static std::string Join(const std::vector<std::string>& lines)
{
	std::string result;
	for (std::size_t index = 0; index < lines.size(); ++index)
	{
		if (index)
			result += '\n';
		result += lines[index];
	}
	return result;
}
static void Copy(std::vector<char>& buffer, const std::string& text)
{
	buffer.resize(std::max(buffer.size(), text.size() + 1024));
	std::copy(text.begin(), text.end(), buffer.begin());
	buffer[text.size()] = 0;
}
static void Problem(const std::string& problem)
{
	if (!problem.empty())
		ImGui::TextColored(theme::Vec(theme::Record), "%s", problem.c_str());
}
static void Label(const char* text)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(text);
}
static bool ProfileChoice(const char* name, bool selected)
{
	const auto position = ImGui::GetCursorScreenPos();
	const bool chosen = ImGui::Selectable("##Profile", selected, 0, {0, ImGui::GetTextLineHeight()});
	ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), name);
	ImGui::SetItemTooltip("%s", name);
	return chosen;
}
static int ResizeText(ImGuiInputTextCallbackData* data)
{
	auto& buffer = *static_cast<std::vector<char>*>(data->UserData);
	buffer.resize(static_cast<std::size_t>(data->BufSize));
	data->Buf = buffer.data();
	return 0;
}
static std::string CheckPath(const char* path, PathKind kind)
{
	try
	{
		return PathProblem(path, kind);
	}
	catch (const std::exception& error)
	{
		return error.what();
	}
}

// One native file or folder dialog at a time. SDL may answer on another
// thread, so the answer goes through a shared, locked slot.
struct LaunchProfilesDialog::FileResult
{
	std::mutex lock;
	bool done = false, failed = false;
	std::string path, error;
	static void SDLCALL Picked(void* user, const char* const* files, int) noexcept
	{
		const std::unique_ptr<std::shared_ptr<FileResult>> owner(static_cast<std::shared_ptr<FileResult>*>(user));
		std::lock_guard lock((*owner)->lock);
		(*owner)->done = true;
		try
		{
			if (!files)
			{
				(*owner)->failed = true;
				(*owner)->error = SDL_GetError();
			}
			else if (files[0])
				(*owner)->path = files[0];
		}
		catch (...)
		{
			(*owner)->failed = true;
		}
	}
};

LaunchProfilesDialog::LaunchProfilesDialog()
	: m_name(128), m_executable(2048), m_workingDirectory(2048), m_manifest(2048), m_rig(2048),
	  m_privateProtonPrefix(2048), m_headset(256), m_arguments(16384)
{
}
void LaunchProfilesDialog::BrowsePath(Browse browse, bool folder, const char* start)
{
	m_browsing = browse;
	m_browseProblem.clear();
	m_file = std::make_shared<FileResult>();
	auto owner = std::make_unique<std::shared_ptr<FileResult>>(m_file);
	const char* location = *start ? start : nullptr;
	if (folder)
		SDL_ShowOpenFolderDialog(FileResult::Picked, owner.release(), nullptr, location, false);
	else
		SDL_ShowOpenFileDialog(FileResult::Picked, owner.release(), nullptr, nullptr, 0, location, false);
}
bool LaunchProfilesDialog::TakePath()
{
	if (!m_file)
		return false;
	const auto file = m_file;
	std::lock_guard lock(file->lock);
	if (!file->done)
		return false;
	m_file.reset();
	const auto browse = std::exchange(m_browsing, std::nullopt);
	if (!browse)
		return false;
	if (file->failed)
		m_browseProblem = file->error.empty() ? "Cannot open file dialog" : file->error;
	if (file->failed || file->path.empty())
		return false;
	auto& target = *browse == Browse::Executable			? m_executable
				   : *browse == Browse::WorkingDirectory	? m_workingDirectory
				   : *browse == Browse::PrivateProtonPrefix ? m_privateProtonPrefix
															: m_manifest;
	Copy(target, file->path);
	if (*browse == Browse::Executable)
		ApplyExecutableDefaults();
	return true;
}
void LaunchProfilesDialog::ApplyExecutableDefaults()
{
	const auto path = Utf8Path(m_executable.data());
	if (!m_name[0])
		Copy(m_name, PathText(path.stem()));
	if (!m_workingDirectory[0])
		Copy(m_workingDirectory, PathText(path.parent_path()));
}
void LaunchProfilesDialog::Load(const LaunchProfile& profile)
{
	++m_form;
	m_browsing.reset();
	m_problem.clear();
	m_browseProblem.clear();
	Copy(m_name, profile.name);
	Copy(m_executable, profile.executable);
	Copy(m_workingDirectory, profile.workingDirectory);
	if (profile.useBundledRuntime)
		m_runtimeChoice = RuntimeChoice::Bundled;
	else if (profile.runtimeManifest.empty())
		m_runtimeChoice = RuntimeChoice::System;
	else
		m_runtimeChoice = RuntimeChoice::Custom;
	Copy(m_manifest, m_runtimeChoice == RuntimeChoice::Custom ? profile.runtimeManifest : std::string{});
	Copy(m_privateProtonPrefix, profile.privateProtonPrefix);
	Copy(m_rig, profile.rig);
	Copy(m_headset, profile.headsetProfile);
	m_originalArguments = profile.arguments;
	Copy(m_arguments, Join(profile.arguments));
	m_environment.clear();
	for (const auto& [key, value] : profile.environment)
	{
		auto& variable = m_environment.emplace_back();
		Copy(variable.key, key);
		Copy(variable.value, value);
	}
}
void LaunchProfilesDialog::Edit(const CompanionSettings& settings, int index)
{
	m_selected = index >= 0 && static_cast<std::size_t>(index) < settings.profiles.size() ? index : -1;
	m_storedName = m_selected >= 0 ? settings.profiles[static_cast<std::size_t>(m_selected)].name : std::string{};
	if (m_selected >= 0)
		Load(settings.profiles[static_cast<std::size_t>(m_selected)]);
	else
	{
		LaunchProfile profile;
		profile.useBundledRuntime = true;
		Load(profile);
	}
}
void LaunchProfilesDialog::RefreshSelection(const CompanionSettings& settings)
{
	if (m_selected < 0)
		return;
	for (std::size_t index = 0; index < settings.profiles.size(); ++index)
		if (settings.profiles[index].name == m_storedName)
		{
			m_selected = static_cast<int>(index);
			return;
		}
	m_selected = -1;
	m_storedName.clear();
	Load(Profile());
}
bool LaunchProfilesDialog::Create(CompanionSettings& settings, const LaunchProfile& launch)
{
	m_selected = -1;
	m_storedName.clear();
	auto profile = launch;
	if (profile.name.empty())
		profile.name = "App";
	// A unique name, so the new profile can be stored right away.
	const auto base = profile.name;
	for (int suffix = 2; std::ranges::any_of(settings.profiles,
											 [&](const LaunchProfile& other)
											 {
												 return other.name == profile.name;
											 });
		 ++suffix)
		profile.name = base + " " + std::to_string(suffix);
	Load(profile);
	return Store(settings);
}
LaunchProfile LaunchProfilesDialog::Profile() const
{
	LaunchProfile profile;
	profile.name = m_name.data();
	profile.executable = m_executable.data();
	// Newline-containing or empty arguments must survive edits to unrelated fields.
	profile.arguments =
		m_arguments.data() == Join(m_originalArguments) ? m_originalArguments : Lines(m_arguments.data());
	profile.workingDirectory = m_workingDirectory.data();
	if (m_runtimeChoice == RuntimeChoice::Bundled)
		profile.useBundledRuntime = true;
	else if (m_runtimeChoice == RuntimeChoice::Custom)
		profile.runtimeManifest = m_manifest.data();
	profile.privateProtonPrefix = m_privateProtonPrefix.data();
	profile.headsetProfile = m_headset.data();
	profile.rig = m_rig.data();
	for (const auto& variable : m_environment)
		if (variable.key[0] || variable.value[0])
			profile.environment.emplace_back(variable.key.data(), variable.value.data());
	return profile;
}
std::string LaunchProfilesDialog::NameProblem(const CompanionSettings& settings) const
{
	if (!m_name[0])
		return "Required";
	for (std::size_t index = 0; index < settings.profiles.size(); ++index)
		if (static_cast<int>(index) != m_selected && settings.profiles[index].name == m_name.data())
			return "Name already used";
	return {};
}
std::string LaunchProfilesDialog::EnvironmentProblem() const
{
	std::set<std::string_view> keys;
	for (const auto& variable : m_environment)
	{
		const std::string_view key = variable.key.data();
		if (key.empty() && variable.value[0])
			return "A variable has no name";
		if (key.find('=') != std::string_view::npos)
			return "Names cannot contain =";
		if (!key.empty() && !keys.insert(key).second)
			return std::string(key) + " is set twice";
	}
	return {};
}
bool LaunchProfilesDialog::Store(CompanionSettings& settings)
{
	RefreshSelection(settings);
	m_problem.clear();
	if (!NameProblem(settings).empty() || !EnvironmentProblem().empty())
		return false;
	if (m_runtimeChoice == RuntimeChoice::Custom && !m_manifest[0])
		return false;
	auto candidate = settings;
	const auto profile = Profile();
	if (m_selected < 0)
		candidate.profiles.push_back(profile);
	else
		candidate.profiles.at(static_cast<std::size_t>(m_selected)) = profile;
	candidate.activeProfile = profile.name;
	try
	{
		ValidateSettings(candidate);
	}
	catch (const std::exception& error)
	{
		m_problem = error.what();
		return false;
	}
	settings = std::move(candidate);
	if (m_selected < 0)
		m_selected = static_cast<int>(settings.profiles.size()) - 1;
	m_storedName = profile.name;
	return true;
}
bool LaunchProfilesDialog::PathField(const char* label, std::vector<char>& buffer, PathKind kind, Browse browse)
{
	ImGui::PushID(label);
	Label(label);
	const auto& style = ImGui::GetStyle();
	const char* browseLabel = "Browse…";
	ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(browseLabel).x + style.FramePadding.x * 2 + style.ItemSpacing.x));
	bool edited = ImGui::InputText("##Path", buffer.data(), buffer.size(), ImGuiInputTextFlags_CallbackResize,
										 ResizeText, &buffer);
	if (browse == Browse::Executable && ImGui::IsItemDeactivatedAfterEdit())
	{
		ApplyExecutableDefaults();
		edited = true;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(m_file != nullptr);
	if (ImGui::Button(browseLabel))
		BrowsePath(browse, kind == PathKind::Folder, buffer.data());
	ImGui::EndDisabled();
	if (buffer[0])
		Problem(CheckPath(buffer.data(), kind));
	ImGui::PopID();
	return edited;
}
void LaunchProfilesDialog::DrawList(CompanionSettings& settings, bool& changed)
{
	const auto& style = ImGui::GetStyle();
	const bool narrow = ImGui::GetContentRegionAvail().x <
						ImGui::CalcTextSize("New?Delete").x + style.FramePadding.x * 6 + style.ItemSpacing.x * 2;
	const float footer = ImGui::GetFrameHeightWithSpacing() * (narrow ? 3 : 1);
	ImGui::BeginChild("Profile names", {0, -footer});
	for (std::size_t index = 0; index < settings.profiles.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		if (ProfileChoice(settings.profiles[index].name.c_str(), m_selected == static_cast<int>(index)))
			Edit(settings, static_cast<int>(index));
		ImGui::PopID();
	}
	if (m_selected < 0)
		ProfileChoice(m_name[0] ? m_name.data() : "New profile", true);
	ImGui::EndChild();
	if (ImGui::Button("New"))
		Edit(settings, -1);
	ImGui::SetItemTooltip("Start a new launch profile");
	if (!narrow)
		ImGui::SameLine();
	if (ImGui::SmallButton("?"))
		ImGui::OpenPopup("Launch help");
	ImGui::SetItemTooltip("How launching works");
	if (ImGui::BeginPopup("Launch help"))
	{
		ImGui::TextUnformatted("Launching an app");
		ImGui::TextDisabled("Runtime selection is per app unless a private Proton prefix is selected.");
		ImGui::TextDisabled(
			"An explicit private Proton prefix keeps its selected runtime for all apps in that prefix.");
		ImGui::TextDisabled("Arguments are passed as they are, without a shell.");
		ImGui::TextDisabled("A script runs only after the new app exposes one running session.");
		ImGui::TextDisabled("Closing the companion leaves the launched app running.");
		ImGui::EndPopup();
	}
	if (m_selected >= 0)
	{
		if (!narrow)
			ImGui::SameLine();
		if (ImGui::Button("Delete"))
			ImGui::OpenPopup("Delete profile?");
	}
	if (ImGui::BeginPopup("Delete profile?"))
	{
		ImGui::Text("Delete %s?", m_storedName.c_str());
		if (ImGui::Button("Delete") && m_selected >= 0)
		{
			const auto removed = settings.profiles[static_cast<std::size_t>(m_selected)].name;
			settings.profiles.erase(settings.profiles.begin() + m_selected);
			if (settings.activeProfile == removed)
				settings.activeProfile.clear();
			Edit(settings, settings.profiles.empty() ? -1 : 0);
			changed = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
bool LaunchProfilesDialog::DrawForm(CompanionSettings& settings, const LaunchSnapshot& process, bool& changed,
									const std::optional<std::array<HeadsetEyeSize, 2>>& eyes, bool canRestart)
{
	bool edited = TakePath();
	ImGui::PushItemWidth(-1);
	edited |= PathField("Executable", m_executable, PathKind::Executable, Browse::Executable);
	Label("Name");
	edited |= ImGui::InputText("##Name", m_name.data(), m_name.size(), ImGuiInputTextFlags_CallbackResize, ResizeText,
							   &m_name);
	const auto nameProblem = NameProblem(settings);
	if (m_name[0])
		Problem(nameProblem);
	if (Section("Launch options"))
	{
		Label("Arguments");
		ImGui::SameLine();
		ImGui::TextDisabled("one per line");
		edited |= ImGui::InputTextMultiline("##Arguments", m_arguments.data(), m_arguments.size(),
											   {-1, ImGui::GetFontSize() * 4}, ImGuiInputTextFlags_CallbackResize,
											   ResizeText, &m_arguments);
		edited |= PathField("Working folder", m_workingDirectory, PathKind::Folder, Browse::WorkingDirectory);
	}

	if (eyes)
	{
		const auto width = std::max((*eyes)[0].width, (*eyes)[1].width);
		const auto height = std::max((*eyes)[0].height, (*eyes)[1].height);
		const auto find = [&](std::string_view key)
		{
			return std::ranges::find_if(m_environment,
										[&](const Variable& value)
										{
											return key == value.key.data();
										});
		};
		const auto additions = (find("SIMXR_WIDTH") == m_environment.end() ? 1u : 0u) +
							   (find("SIMXR_HEIGHT") == m_environment.end() ? 1u : 0u);
		const bool supported = width >= 16 && height >= 16 && width <= 8192 && height <= 8192;
		ImGui::Text("Headset render size: %u × %u", width, height);
		ImGui::BeginDisabled(!supported || m_environment.size() + additions > MaxLaunchEnvironment);
		if (ImGui::Button("Use headset size"))
		{
			for (const auto& [key, value] : {std::pair{"SIMXR_WIDTH", width}, std::pair{"SIMXR_HEIGHT", height}})
			{
				auto entry = find(key);
				if (entry == m_environment.end())
				{
					m_environment.emplace_back();
					entry = std::prev(m_environment.end());
					Copy(entry->key, key);
				}
				Copy(entry->value, std::to_string(value));
			}
			edited = true;
		}
		ImGui::SetItemTooltip("Saves the larger recommended eye dimensions for this profile's next launch. "
							  "Both eyes use the same allocation size.");
		ImGui::EndDisabled();
		if (!supported)
			ImGui::TextWrapped("The headset recommendation exceeds the supported launch dimensions (16–8192).");
	}

	if (Section("Environment"))
	{
		if (!m_environment.empty() &&
			ImGui::BeginTable("Environment", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH))
		{
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2);
			ImGui::TableSetupColumn("Remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
			std::optional<std::size_t> removed;
			for (std::size_t index = 0; index < m_environment.size(); ++index)
			{
				auto& variable = m_environment[index];
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-1);
				edited |= ImGui::InputTextWithHint("##Key", "NAME", variable.key.data(), variable.key.size(),
												   ImGuiInputTextFlags_CallbackResize, ResizeText, &variable.key);
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-1);
				edited |= ImGui::InputTextWithHint("##Value", "value", variable.value.data(), variable.value.size(),
												   ImGuiInputTextFlags_CallbackResize, ResizeText, &variable.value);
				ImGui::TableNextColumn();
				if (IconButton("Remove variable", Icon::Close, "The next launch will no longer set this variable"))
					removed = index;
				ImGui::PopID();
			}
			ImGui::EndTable();
			if (removed)
			{
				m_environment.erase(m_environment.begin() + static_cast<std::ptrdiff_t>(*removed));
				edited = true;
			}
		}
		ImGui::BeginDisabled(m_environment.size() >= MaxLaunchEnvironment);
		if (ImGui::SmallButton("Add variable"))
			m_environment.emplace_back();
		ImGui::EndDisabled();
	}
	const auto environmentProblem = EnvironmentProblem();
	Problem(environmentProblem);

	if (Section("Runtime and script"))
	{
		Label("OpenXR runtime");
		const char* runtime = m_runtimeChoice == RuntimeChoice::Bundled ? "OpenXR Simulator"
											 : m_runtimeChoice == RuntimeChoice::System ? "System or environment" : "Custom";
		if (ImGui::BeginCombo("##OpenXR runtime", runtime))
		{
			for (const auto& [choice, label] : {std::pair{RuntimeChoice::Bundled, "OpenXR Simulator"},
												 std::pair{RuntimeChoice::System, "System or environment"},
												 std::pair{RuntimeChoice::Custom, "Custom"}})
				if (ImGui::Selectable(label, choice == m_runtimeChoice))
				{
					m_runtimeChoice = choice;
					edited = true;
				}
			ImGui::EndCombo();
		}
		if (m_runtimeChoice == RuntimeChoice::Custom)
			edited |= PathField("OpenXR runtime JSON", m_manifest, PathKind::File, Browse::Manifest);
		edited |= PathField("Private Proton prefix (optional)", m_privateProtonPrefix, PathKind::Folder,
							Browse::PrivateProtonPrefix);
		ImGui::TextWrapped("For Proton, select the pfx folder prepared by prepare-proton-prefix.py. "
						   "Launch registers this runtime for that private prefix.");
		if (ImGui::Button("Steam launch options"))
			ImGui::OpenPopup("Steam launch options");
		const auto anchor = ImGui::GetItemRectMin();
		const auto* viewport = ImGui::GetMainViewport();
		const float popupWidth = std::max(1.0f, std::min(ImGui::GetFontSize() * 24, viewport->WorkSize.x - 16));
		const float popupMinX = viewport->WorkPos.x + 8;
		const float popupMaxX = std::max(popupMinX, viewport->WorkPos.x + viewport->WorkSize.x - popupWidth - 8);
		const float popupX = std::clamp(anchor.x, popupMinX, popupMaxX);
		ImGui::SetNextWindowPos({popupX, anchor.y - 4}, ImGuiCond_Appearing, {0, 1});
		ImGui::SetNextWindowSize({popupWidth, 0}, ImGuiCond_Appearing);
		if (ImGui::BeginPopup("Steam launch options"))
		{
#ifdef __linux__
			ImGui::TextWrapped("For native Linux games, paste this in Steam under Properties → Launch Options.");
			try
			{
				const auto* base = SDL_GetBasePath();
				if (!base)
					throw std::runtime_error("Cannot locate the OpenXR Simulator installation");
				const auto profile = ResolveLaunchProfile(Profile(), Utf8Path(base));
				const auto manifest = Utf8Path(profile.runtimeManifest);
				if (!profile.privateProtonPrefix.empty())
					throw std::runtime_error("Private Proton profiles use the Windows launcher in their prefix.");
				if (const auto problem = CheckPath(profile.runtimeManifest.c_str(), PathKind::File); !problem.empty())
					throw std::runtime_error("Select an existing runtime manifest first.");
				const auto option = LinuxSteamRuntimeOption(manifest);
				ImGui::TextWrapped("%s", option.c_str());
				if (ImGui::Button("Copy options") && !SDL_SetClipboardText(option.c_str()))
					m_problem = SDL_GetError();
				ImGui::SetItemTooltip("Copies only runtime selection. Configure other Steam launch options separately.");
			}
			catch (const std::exception& error)
			{
				ImGui::TextWrapped("%s", error.what());
			}
#else
			ImGui::TextWrapped("With Steam running, select the game executable in a launch profile and launch it here. "
							   "Some games require Steam-managed startup.");
#endif
			ImGui::TextWrapped("For Proton, prepare a dedicated prefix with prepare-proton-prefix.py and run the "
						   "Windows simulator in it. Select that prefix above; shared prefixes are not adopted.");
			ImGui::EndPopup();
		}
		Label("Headset profile");
		if (ImGui::BeginCombo("##Headset profile", m_headset[0] ? m_headset.data() : "Runtime default"))
		{
			if (ImGui::Selectable("Runtime default", !m_headset[0]))
			{
				std::fill(m_headset.begin(), m_headset.end(), 0);
				edited = true;
			}
			for (const auto& profile : HeadsetProfiles())
				if (ImGui::Selectable(profile.id, std::string_view(profile.id) == m_headset.data()))
				{
					Copy(m_headset, profile.id);
					edited = true;
				}
			ImGui::EndCombo();
		}
		Label("Script after launch");
		edited |= ImGui::InputText("##Script after launch", m_rig.data(), m_rig.size(),
								   ImGuiInputTextFlags_CallbackResize, ResizeText, &m_rig);
	}
	ImGui::PopItemWidth();
	if (edited)
		changed |= Store(settings);
	std::string profileProblem;
	try
	{
		ValidateLaunchProfile(Profile());
	}
	catch (const std::exception& error)
	{
		profileProblem = error.what();
	}
	Problem(profileProblem);
	if (m_problem != profileProblem)
		Problem(m_problem);
	Problem(m_browseProblem);

	ImGui::Separator();
	const bool valid = nameProblem.empty() && environmentProblem.empty() && profileProblem.empty() &&
					   CheckPath(m_executable.data(), PathKind::Executable).empty() &&
					   (!m_workingDirectory[0] || CheckPath(m_workingDirectory.data(), PathKind::Folder).empty()) &&
					   (m_runtimeChoice != RuntimeChoice::Custom || CheckPath(m_manifest.data(), PathKind::File).empty()) &&
					   (!m_privateProtonPrefix[0] || CheckPath(m_privateProtonPrefix.data(), PathKind::Folder).empty());
	const bool running = process.state == LaunchState::Starting || process.state == LaunchState::Running;
	ImGui::BeginDisabled(!valid || running);
	const bool launch = ImGui::Button("Launch");
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(running ? "An app launched from here is still running"
						  : valid ? "Launch this profile and connect to it"
								  : "Choose an executable and name, then fix any marked fields");
	const bool saved = m_selected >= 0 && static_cast<std::size_t>(m_selected) < settings.profiles.size() &&
					   settings.profiles[static_cast<std::size_t>(m_selected)] == Profile();
	if (ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize("Copy launch command").x +
										  ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x)
		ImGui::SameLine();
	ImGui::BeginDisabled(!valid || !saved);
	if (ImGui::Button("Copy launch command"))
		m_copyCommand = true;
	ImGui::EndDisabled();
#ifdef _WIN32
	ImGui::SetItemTooltip("Copies a PowerShell command that opens a new simulator window and launches this saved "
						  "profile. Later profile changes affect the command.");
#else
	ImGui::SetItemTooltip("Copies a shell command that opens a new simulator window and launches this saved "
						  "profile. Later profile changes affect the command.");
#endif
	if (process.restartPending)
	{
		if (ImGui::Button("Cancel restart"))
			m_cancelRestart = true;
		ImGui::SetItemTooltip(
			"Cancels the replacement launch. A close request already sent to the game cannot be undone.");
	}
	else if (eyes && running)
	{
		const auto width = std::max((*eyes)[0].width, (*eyes)[1].width);
		const auto height = std::max((*eyes)[0].height, (*eyes)[1].height);
		ImGui::BeginDisabled(!valid || !canRestart || width < 16 || width > 8192 || height < 16 || height > 8192);
		if (ImGui::Button("Restart with headset settings"))
		{
			auto replacement = Profile();
			for (const auto& [key, value] : {std::pair{"SIMXR_WIDTH", width}, std::pair{"SIMXR_HEIGHT", height}})
			{
				auto entry = std::ranges::find_if(replacement.environment,
												  [&](const auto& variable)
												  {
													  return variable.first == key;
												  });
				if (entry == replacement.environment.end())
					replacement.environment.emplace_back(key, std::to_string(value));
				else
					entry->second = std::to_string(value);
			}
			m_restartReview = LaunchRestartRequest{std::move(replacement), process.process};
			ImGui::OpenPopup("Restart application?");
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(
			"Restart the connected app launched by this companion, using this profile and the detected eye size.");
	}
	if (ImGui::BeginPopupModal("Restart application?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (m_restartReview)
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 32);
			ImGui::TextWrapped(
				"This closes %s (PID %llu). Unsaved game progress and the current XR session will be lost.",
				process.profile.c_str(), static_cast<unsigned long long>(m_restartReview->process));
			ImGui::TextWrapped("Replacement profile: %s", m_restartReview->profile.name.c_str());
			ImGui::TextWrapped("Executable: %s", m_restartReview->profile.executable.c_str());
			for (const auto& [key, value] : m_restartReview->profile.environment)
				if (key == "SIMXR_WIDTH" || key == "SIMXR_HEIGHT")
					ImGui::Text("%s: %s", key == "SIMXR_WIDTH" ? "Eye width" : "Eye height", value.c_str());
			ImGui::TextWrapped("The headset disconnects first. The replacement starts only after the game exits. "
							   "Connect the headset again in the new session.");
			ImGui::PopTextWrapPos();
			ImGui::BeginDisabled(!canRestart || process.state != LaunchState::Running ||
								 process.process != m_restartReview->process || process.restartPending);
			if (ImGui::Button("Restart"))
			{
				m_restartRequest = std::move(m_restartReview);
				m_restartReview.reset();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Keep running"))
			{
				m_restartReview.reset();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}
	if (process.process || !process.detail.empty())
	{
		ImGui::AlignTextToFramePadding();
		if (process.process)
			ImGui::TextDisabled("PID %llu", static_cast<unsigned long long>(process.process));
		if (!process.detail.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("%s", process.detail.c_str());
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("%s", process.detail.c_str());
		}
	}
	return launch;
}
std::optional<LaunchRestartRequest> LaunchProfilesDialog::TakeRestartRequest()
{
	return std::exchange(m_restartRequest, {});
}
bool LaunchProfilesDialog::TakeRestartCancellation()
{
	return std::exchange(m_cancelRestart, false);
}
bool LaunchProfilesDialog::TakeCopyCommandRequest()
{
	return std::exchange(m_copyCommand, false);
}
bool LaunchProfilesDialog::Draw(CompanionSettings& settings, const LaunchSnapshot& process, bool& open, bool& changed,
								const std::optional<std::array<HeadsetEyeSize, 2>>& eyes, bool canRestart)
{
	RefreshSelection(settings);
	if (!open)
		m_browsing.reset();
	if (open && !ImGui::IsPopupOpen("Launch profiles"))
		ImGui::OpenPopup("Launch profiles");
	const float unit = ImGui::GetFontSize();
	const auto* viewport = ImGui::GetMainViewport();
	const auto available = viewport->WorkSize;
	ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, {.5f, .5f});
	ImGui::SetNextWindowSize({std::min(unit * 46, available.x), std::min(unit * 32, available.y)},
							 ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints({std::min(unit * 30, available.x), std::min(unit * 28, available.y)},
										available);
	if (!ImGui::BeginPopupModal("Launch profiles", &open, ImGuiWindowFlags_NoMove))
		return false;
	ImGui::BeginChild("Profile list", {std::min(unit * 11, ImGui::GetContentRegionAvail().x * .35f), 0},
					  ImGuiChildFlags_Borders);
	DrawList(settings, changed);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("Profile form", {0, 0});
	ImGui::PushID(std::to_string(m_form).c_str());
	const bool launch = DrawForm(settings, process, changed, eyes, canRestart);
	ImGui::PopID();
	ImGui::EndChild();
	ImGui::EndPopup();
	return launch;
}
} // namespace simxr::app
