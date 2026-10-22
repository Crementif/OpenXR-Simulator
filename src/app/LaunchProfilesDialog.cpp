#include "app/LaunchProfilesDialog.h"
#include "simulation/HeadsetProfiles.h"

#include <cstdio>
#include <imgui.h>
#include <stdexcept>
#include <string_view>
#include <vector>

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
void LaunchProfilesDialog::Edit(const CompanionSettings& settings, int index)
{
	m_selected = index;
	LaunchProfile profile;
	if (index >= 0)
		profile = settings.profiles.at(static_cast<std::size_t>(index));
	Copy(m_name, profile.name);
	Copy(m_executable, profile.executable);
	Copy(m_workingDirectory, profile.workingDirectory);
	Copy(m_manifest, profile.runtimeManifest);
	Copy(m_rig, profile.rig);
	Copy(m_headset, profile.headsetProfile);
	Copy(m_arguments, Join(profile.arguments));
	std::vector<std::string> values;
	for (const auto& [key, value] : profile.environment)
		values.push_back(key + "=" + value);
	Copy(m_environment, Join(values));
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
	for (const auto& line : Lines(m_environment.data()))
	{
		const auto equals = line.find('=');
		if (equals == std::string::npos)
			throw std::invalid_argument("Environment lines must be KEY=value");
		profile.environment.emplace_back(line.substr(0, equals), line.substr(equals + 1));
	}
	return profile;
}
void LaunchProfilesDialog::Store(CompanionSettings& settings)
{
	auto candidate = settings;
	auto profile = Profile();
	if (m_selected < 0)
		candidate.profiles.push_back(profile);
	else
		candidate.profiles.at(static_cast<std::size_t>(m_selected)) = profile;
	candidate.activeProfile = profile.name;
	ValidateSettings(candidate);
	settings = std::move(candidate);
	if (m_selected < 0)
		m_selected = static_cast<int>(settings.profiles.size()) - 1;
}
bool LaunchProfilesDialog::Draw(CompanionSettings& settings, const LaunchSnapshot& process, bool& open, bool& changed,
								std::string& error)
{
	if (open)
		ImGui::OpenPopup("Launch profiles");
	ImGui::SetNextWindowSize({620, 500}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints({400, 260}, ImGui::GetMainViewport()->WorkSize);
	if (!ImGui::BeginPopupModal("Launch profiles", &open))
		return false;
	if (ImGui::BeginCombo("Profile", m_selected >= 0 ? settings.profiles[static_cast<std::size_t>(m_selected)].name.c_str()
													 : "New profile"))
	{
		for (std::size_t index = 0; index < settings.profiles.size(); ++index)
			if (ImGui::Selectable(settings.profiles[index].name.c_str(), m_selected == static_cast<int>(index)))
				Edit(settings, static_cast<int>(index));
		ImGui::EndCombo();
	}
	if (ImGui::Button("New profile"))
		Edit(settings, -1);
	if (m_selected >= 0)
	{
		ImGui::SameLine();
		if (ImGui::Button("Delete profile"))
		{
			settings.profiles.erase(settings.profiles.begin() + m_selected);
			settings.activeProfile.clear();
			Edit(settings, -1);
			changed = true;
		}
	}
	ImGui::InputText("Name", m_name.data(), m_name.size());
	ImGui::InputText("Executable", m_executable.data(), m_executable.size());
	ImGui::TextUnformatted("Arguments: one literal argument per line; no shell expansion");
	ImGui::InputTextMultiline("##arguments", m_arguments.data(), m_arguments.size(), {-1, 90});
	ImGui::InputText("Working directory", m_workingDirectory.data(), m_workingDirectory.size());
	ImGui::InputText("OpenXR runtime JSON", m_manifest.data(), m_manifest.size());
	ImGui::TextUnformatted("Child environment: KEY=value per line (including WINEPREFIX / Proton values)");
	ImGui::InputTextMultiline("##environment", m_environment.data(), m_environment.size(), {-1, 90});
	if (ImGui::BeginCombo("Headset profile", m_headset[0] ? m_headset.data() : "Runtime default"))
	{
		if (ImGui::Selectable("Runtime default", !m_headset[0]))
			m_headset.fill(0);
		for (const auto& profile : HeadsetProfiles())
			if (ImGui::Selectable(profile.id, std::string_view(profile.id) == m_headset.data()))
				Copy(m_headset, profile.id);
		ImGui::EndCombo();
	}
	ImGui::InputText("Luau rig after launch", m_rig.data(), m_rig.size());
	ImGui::TextWrapped("The headset profile applies to the launched runtime. A rig runs only after the new "
					   "child endpoint exposes one running session; an existing connection is never reused.");
	if (ImGui::Button("Save profile"))
		try
		{
			Store(settings);
			changed = true;
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
		}
	ImGui::SameLine();
	ImGui::BeginDisabled(process.state == LaunchState::Starting || process.state == LaunchState::Running);
	const bool launch = ImGui::Button("Launch");
	ImGui::EndDisabled();
	if (process.process)
		ImGui::Text("Process: %llu", static_cast<unsigned long long>(process.process));
	if (!process.detail.empty())
		ImGui::TextWrapped("%s", process.detail.c_str());
	ImGui::TextWrapped("The runtime manifest and environment apply only to the child. After launch, use Find "
					   "runtimes to attach. Closing the companion leaves the launched application running.");
	ImGui::EndPopup();
	return launch;
}
} // namespace simxr::app
