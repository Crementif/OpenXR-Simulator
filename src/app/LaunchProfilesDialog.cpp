#include "app/LaunchProfilesDialog.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"
#include "simulation/HeadsetProfiles.h"

#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <mutex>
#include <set>
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
void Problem(const std::string& problem)
{
	if (!problem.empty())
		ImGui::TextColored(theme::Vec(theme::Record), "%s", problem.c_str());
}
void Label(const char* text)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(text);
}
} // namespace

// One native file or folder dialog at a time. SDL may answer on another
// thread, so the answer goes through a shared, locked slot.
class LaunchProfilesDialog::FileRequest
{
  public:
	void Open(bool folder, const std::string& start)
	{
		m_shared = std::make_shared<Shared>();
		auto* owner = new std::shared_ptr<Shared>(m_shared);
		const char* location = start.empty() ? nullptr : start.c_str();
		if (folder)
			SDL_ShowOpenFolderDialog(Picked, owner, nullptr, location, false);
		else
			SDL_ShowOpenFileDialog(Picked, owner, nullptr, nullptr, 0, location, false);
	}
	bool Pending() const
	{
		return m_shared != nullptr;
	}
	// The chosen path once the dialog closed; empty when it was cancelled.
	std::optional<std::string> Take()
	{
		if (!m_shared)
			return {};
		std::lock_guard lock(m_shared->lock);
		if (!m_shared->done)
			return {};
		auto path = std::move(m_shared->path);
		m_shared.reset();
		return path;
	}

  private:
	struct Shared
	{
		std::mutex lock;
		bool done = false;
		std::string path;
	};
	static void SDLCALL Picked(void* user, const char* const* files, int)
	{
		const std::unique_ptr<std::shared_ptr<Shared>> owner(static_cast<std::shared_ptr<Shared>*>(user));
		std::lock_guard lock((*owner)->lock);
		(*owner)->done = true;
		if (files && files[0])
			(*owner)->path = files[0];
	}
	std::shared_ptr<Shared> m_shared;
};

LaunchProfilesDialog::LaunchProfilesDialog() : m_file(std::make_unique<FileRequest>())
{
}
LaunchProfilesDialog::~LaunchProfilesDialog() = default;
void LaunchProfilesDialog::Load(const LaunchProfile& profile)
{
	Copy(m_name, profile.name);
	Copy(m_executable, profile.executable);
	Copy(m_workingDirectory, profile.workingDirectory);
	Copy(m_manifest, profile.runtimeManifest);
	Copy(m_rig, profile.rig);
	Copy(m_headset, profile.headsetProfile);
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
	m_selected = index;
	Load(index >= 0 ? settings.profiles.at(static_cast<std::size_t>(index)) : LaunchProfile{});
}
bool LaunchProfilesDialog::Create(CompanionSettings& settings, const LaunchProfile& launch)
{
	m_selected = -1;
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
	profile.arguments = Lines(m_arguments.data());
	profile.workingDirectory = m_workingDirectory.data();
	profile.runtimeManifest = m_manifest.data();
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
	if (!NameProblem(settings).empty() || !EnvironmentProblem().empty())
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
	catch (const std::exception&)
	{
		return false;
	}
	settings = std::move(candidate);
	if (m_selected < 0)
		m_selected = static_cast<int>(settings.profiles.size()) - 1;
	return true;
}
bool LaunchProfilesDialog::PathField(const char* label, std::array<char, 2048>& buffer, PathKind kind, Browse browse,
									 bool optional)
{
	ImGui::PushID(label);
	Label(label);
	const auto& style = ImGui::GetStyle();
	const char* browseLabel = "Browse…";
	ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(browseLabel).x + style.FramePadding.x * 2 + style.ItemSpacing.x));
	ImGui::InputText("##Path", buffer.data(), buffer.size());
	bool edited = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::SameLine();
	ImGui::BeginDisabled(m_file->Pending());
	if (ImGui::Button(browseLabel))
	{
		m_browsing = browse;
		m_file->Open(kind == PathKind::Folder, buffer.data());
	}
	ImGui::EndDisabled();
	if (!optional || buffer[0])
		Problem(PathProblem(buffer.data(), kind));
	ImGui::PopID();
	return edited;
}
void LaunchProfilesDialog::DrawList(CompanionSettings& settings, bool& changed)
{
	const float footer = ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("Profile names", {0, -footer});
	for (std::size_t index = 0; index < settings.profiles.size(); ++index)
		if (ImGui::Selectable(settings.profiles[index].name.c_str(), m_selected == static_cast<int>(index)))
			Edit(settings, static_cast<int>(index));
	if (m_selected < 0)
		ImGui::Selectable(m_name[0] ? m_name.data() : "New profile", true);
	ImGui::EndChild();
	if (ImGui::Button("New"))
		Edit(settings, -1);
	ImGui::SetItemTooltip("Start a new launch profile");
	ImGui::SameLine();
	if (ImGui::SmallButton("?"))
		ImGui::OpenPopup("Launch help");
	ImGui::SetItemTooltip("How launching works");
	if (ImGui::BeginPopup("Launch help"))
	{
		ImGui::TextUnformatted("Launching an app");
		ImGui::TextDisabled("The runtime JSON and environment apply only to the launched app.");
		ImGui::TextDisabled("Arguments are passed as they are, without a shell.");
		ImGui::TextDisabled("A script runs only after the new app exposes one running session.");
		ImGui::TextDisabled("Closing the companion leaves the launched app running.");
		ImGui::EndPopup();
	}
	if (m_selected >= 0)
	{
		ImGui::SameLine();
		if (ImGui::Button("Delete"))
			ImGui::OpenPopup("Delete profile?");
	}
	if (ImGui::BeginPopup("Delete profile?"))
	{
		ImGui::Text("Delete %s?", m_name.data());
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
bool LaunchProfilesDialog::DrawForm(CompanionSettings& settings, const LaunchSnapshot& process, bool& changed)
{
	bool edited = false;
	if (auto path = m_file->Take(); path && !path->empty() && m_browsing)
	{
		auto& target = *m_browsing == Browse::Executable		 ? m_executable
					   : *m_browsing == Browse::WorkingDirectory ? m_workingDirectory
																 : m_manifest;
		Copy(target, *path);
		edited = true;
	}
	ImGui::PushItemWidth(-1);
	Label("Name");
	ImGui::InputText("##Name", m_name.data(), m_name.size());
	edited |= ImGui::IsItemDeactivatedAfterEdit();
	const auto nameProblem = NameProblem(settings);
	Problem(nameProblem);
	edited |= PathField("Executable", m_executable, PathKind::Executable, Browse::Executable, false);
	Label("Arguments");
	ImGui::SameLine();
	ImGui::TextDisabled("one per line");
	ImGui::InputTextMultiline("##Arguments", m_arguments.data(), m_arguments.size(), {-1, ImGui::GetFontSize() * 4});
	edited |= ImGui::IsItemDeactivatedAfterEdit();
	edited |= PathField("Working folder", m_workingDirectory, PathKind::Folder, Browse::WorkingDirectory, true);

	Label("Environment");
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
			ImGui::InputTextWithHint("##Key", "NAME", variable.key.data(), variable.key.size());
			edited |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##Value", "value", variable.value.data(), variable.value.size());
			edited |= ImGui::IsItemDeactivatedAfterEdit();
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
	const auto environmentProblem = EnvironmentProblem();
	Problem(environmentProblem);

	if (ImGui::CollapsingHeader("Runtime and script"))
	{
		edited |= PathField("OpenXR runtime JSON", m_manifest, PathKind::File, Browse::Manifest, true);
		Label("Headset profile");
		if (ImGui::BeginCombo("##Headset profile", m_headset[0] ? m_headset.data() : "Runtime default"))
		{
			if (ImGui::Selectable("Runtime default", !m_headset[0]))
			{
				m_headset.fill(0);
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
		ImGui::InputText("##Script after launch", m_rig.data(), m_rig.size());
		edited |= ImGui::IsItemDeactivatedAfterEdit();
	}
	ImGui::PopItemWidth();
	if (edited)
		changed |= Store(settings);

	ImGui::Separator();
	const bool valid = nameProblem.empty() && environmentProblem.empty() &&
					   PathProblem(m_executable.data(), PathKind::Executable).empty() &&
					   (!m_workingDirectory[0] || PathProblem(m_workingDirectory.data(), PathKind::Folder).empty()) &&
					   (!m_manifest[0] || PathProblem(m_manifest.data(), PathKind::File).empty());
	const bool running = process.state == LaunchState::Starting || process.state == LaunchState::Running;
	ImGui::BeginDisabled(!valid || running);
	const bool launch = ImGui::Button("Test launch");
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(running ? "An app launched from here is still running"
							: valid ? "Launch this profile and connect to it"
									: "Fix the marked fields first");
	if (process.process || !process.detail.empty())
	{
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		if (process.process)
			ImGui::TextDisabled("PID %llu", static_cast<unsigned long long>(process.process));
		if (!process.detail.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", process.detail.c_str());
			ImGui::SetItemTooltip("%s", process.detail.c_str());
		}
	}
	return launch;
}
bool LaunchProfilesDialog::Draw(CompanionSettings& settings, const LaunchSnapshot& process, bool& open, bool& changed)
{
	if (open && !ImGui::IsPopupOpen("Launch profiles"))
		ImGui::OpenPopup("Launch profiles");
	const float unit = ImGui::GetFontSize();
	ImGui::SetNextWindowSize({unit * 46, unit * 32}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints({unit * 30, unit * 18}, ImGui::GetMainViewport()->WorkSize);
	if (!ImGui::BeginPopupModal("Launch profiles", &open))
		return false;
	ImGui::BeginChild("Profile list", {unit * 11, 0}, ImGuiChildFlags_Borders);
	DrawList(settings, changed);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("Profile form", {0, 0});
	const bool launch = DrawForm(settings, process, changed);
	ImGui::EndChild();
	ImGui::EndPopup();
	return launch;
}
} // namespace simxr::app
