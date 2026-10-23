#include "Pch.h"
#include "app/AppPicker.h"
#include "app/RuntimeView.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"

#include <ctime>
#include <imgui.h>

namespace simxr::app
{
namespace
{
std::string Clock(std::chrono::system_clock::time_point time)
{
	const auto seconds = std::chrono::system_clock::to_time_t(time);
	const auto age = std::chrono::system_clock::now() - time;
	char text[32];
	std::strftime(text, sizeof(text), age < std::chrono::hours(20) ? "%H:%M" : "%b %d %H:%M",
				  std::localtime(&seconds));
	return text;
}
std::string Age(std::int64_t lastUsed)
{
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
						 std::chrono::system_clock::now().time_since_epoch())
						 .count();
	const auto seconds = std::max<std::int64_t>(0, now - lastUsed);
	if (seconds < 60)
		return "now";
	if (seconds < 3600)
		return std::to_string(seconds / 60) + " min ago";
	if (seconds < 86400)
		return std::to_string(seconds / 3600) + " h ago";
	return std::to_string(seconds / 86400) + " d ago";
}
std::string CommandLine(const LaunchProfile& launch)
{
	std::string text = launch.executable;
	for (const auto& argument : launch.arguments)
		text += " " + argument;
	if (!launch.workingDirectory.empty())
		text += "\nin " + launch.workingDirectory;
	return text;
}
// Right-aligns the next buttons, given their labels.
void AlignRight(std::initializer_list<const char*> labels)
{
	const auto& style = ImGui::GetStyle();
	float width = 0;
	for (const auto* label : labels)
		width += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - width + style.ItemSpacing.x));
}
} // namespace
std::string AppPicker::Refresh()
{
	m_nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	try
	{
		m_running = DiscoverRunningApps();
		m_discoveryFailed = false;
		return {};
	}
	catch (const std::exception& error)
	{
		m_running.clear();
		m_discoveryFailed = true;
		return error.what();
	}
}
AppPickerChoice AppPicker::Draw(const CompanionSettings& settings, const RuntimeView& view, const std::string& label,
								ImU32 dot, const std::string& tooltip, bool open, const std::function<void()>& session)
{
	AppPickerChoice choice;
	const float height = ImGui::GetFrameHeight(), unit = ImGui::GetFontSize() / 17;
	const auto text = ImGui::CalcTextSize(label.c_str());
	const float dotX = height + text.x + 8 * unit, chevronX = dotX + (dot ? 14 * unit : 0);
	const ImVec2 size{chevronX + 18 * unit, height};
	const auto minimum = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton("##App picker", size, ImGuiButtonFlags_EnableNav);
	const bool hovered = ImGui::IsItemHovered(), shown = ImGui::IsPopupOpen("App picker");
	auto* draw = ImGui::GetWindowDrawList();
	if (hovered || shown)
		draw->AddRectFilled(minimum, {minimum.x + size.x, minimum.y + size.y},
							theme::Mix(theme::Bg, theme::Text, shown ? .1f : .06f), theme::Radius * unit);
	DrawIcon(draw, Icon::App, {minimum.x + height / 2, minimum.y + height / 2}, height * .6f, theme::TextDim);
	draw->AddText({minimum.x + height, minimum.y + (height - text.y) / 2}, theme::Text, label.c_str());
	if (dot)
		draw->AddCircleFilled({minimum.x + dotX + 4 * unit, minimum.y + height / 2}, 4 * unit, dot);
	DrawIcon(draw, Icon::ChevronDown, {minimum.x + chevronX + 7 * unit, minimum.y + height / 2}, 12 * unit,
			 theme::TextDim);
	if (!tooltip.empty())
		ImGui::SetItemTooltip("%s", tooltip.c_str());
	if (pressed || open)
	{
		choice.discoveryError = Refresh();
		ImGui::OpenPopup("App picker");
	}
	ImGui::SetNextWindowPos({minimum.x, minimum.y + height + 2 * unit});
	ImGui::SetNextWindowSizeConstraints({ImGui::GetFontSize() * 24, 0}, {ImGui::GetFontSize() * 40, FLT_MAX});
	if (!ImGui::BeginPopup("App picker"))
		return choice;
	if (std::chrono::steady_clock::now() >= m_nextScan)
		choice.discoveryError = Refresh();
	if (view.connected && session)
	{
		SectionLabel("Session");
		session();
	}

	SectionLabel("Running");
	ImGui::SameLine();
	if (IconButton("Refresh", Icon::Refresh, "Look for running apps again"))
		choice.discoveryError = Refresh();
	if (m_running.empty())
		ImGui::TextDisabled("%s", m_discoveryFailed ? "Running apps unavailable" : "No running apps found");
	for (const auto& app : m_running)
	{
		ImGui::PushID(app.endpoint.c_str());
		const bool connected = view.connected && app.endpoint == view.endpoint;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(app.name.c_str());
		ImGui::SetItemTooltip("%s%s%s", app.executable.c_str(), app.executable.empty() ? "" : "\n", app.endpoint.c_str());
		ImGui::SameLine();
		std::string detail = app.process ? "PID " + std::to_string(app.process) : std::string{};
		if (app.started)
			detail += " · started " + Clock(*app.started);
		ImGui::TextDisabled("%s", detail.c_str());
		AlignRight({"Connect"});
		if (connected)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(theme::Vec(theme::Ok), "Connected");
		}
		else if (ImGui::SmallButton("Connect"))
		{
			choice.connect = app;
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopID();
	}

	SectionLabel("Recent");
	if (settings.recent.empty())
		ImGui::TextDisabled("Launched and connected apps appear here");
	for (std::size_t index = 0; index < settings.recent.size(); ++index)
	{
		const auto& recent = settings.recent[index];
		ImGui::PushID(static_cast<int>(index));
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(recent.launch.name.empty() ? recent.launch.executable.c_str() : recent.launch.name.c_str());
		ImGui::SetItemTooltip("%s", CommandLine(recent.launch).c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", Age(recent.lastUsed).c_str());
		AlignRight({"Launch", "Save as profile"});
		const auto problem = PathProblem(recent.launch.executable, PathKind::Executable);
		ImGui::BeginDisabled(!problem.empty());
		if (ImGui::SmallButton("Launch"))
		{
			choice.launchRecent = recent.launch;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		if (!problem.empty())
			ImGui::SetItemTooltip("%s: %s", problem.c_str(), recent.launch.executable.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Save as profile"))
		{
			choice.saveRecent = index;
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopID();
	}

	SectionLabel("Profiles");
	for (std::size_t index = 0; index < settings.profiles.size(); ++index)
		if (ImGui::Selectable(settings.profiles[index].name.c_str()))
			choice.launchProfile = static_cast<int>(index);
		else
			ImGui::SetItemTooltip("Launch %s", CommandLine(settings.profiles[index]).c_str());
	if (ImGui::Selectable("Add…"))
		choice.addProfile = true;
	if (!settings.profiles.empty() && ImGui::Selectable("Edit profiles"))
		choice.editProfiles = true;
	ImGui::Separator();
	if (ImGui::Selectable("Connect endpoint"))
		choice.connectEndpoint = true;
	if (view.connected && ImGui::Selectable("Disconnect"))
		choice.disconnect = true;
	ImGui::EndPopup();
	return choice;
}
} // namespace simxr::app
