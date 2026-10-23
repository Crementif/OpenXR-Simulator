#include "app/widgets/AppPicker.h"
#include "Pch.h"
#include "app/RuntimeView.h"
#include "app/services/CompanionSettings.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"

#include <cfloat>
#include <ctime>
#include <exception>
#include <imgui.h>
#include <initializer_list>

namespace simxr::app
{
static std::string Clock(std::chrono::system_clock::time_point time)
{
	const auto seconds = std::chrono::system_clock::to_time_t(time);
	const auto age = std::chrono::system_clock::now() - time;
	const auto* local = std::localtime(&seconds);
	char text[32]{};
	if (!local || !std::strftime(text, sizeof(text), age < std::chrono::hours(20) ? "%H:%M" : "%b %d %H:%M", local))
		return "unknown";
	return text;
}
static std::string Age(std::int64_t lastUsed)
{
	const auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	const auto seconds =
		lastUsed >= now ? std::uint64_t{0} : static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(lastUsed);
	if (seconds < 60)
		return "now";
	if (seconds < 3600)
		return std::to_string(seconds / 60) + " min ago";
	if (seconds < 86400)
		return std::to_string(seconds / 3600) + " h ago";
	return std::to_string(seconds / 86400) + " d ago";
}
static std::string CommandLine(const LaunchProfile& launch)
{
	std::string text = launch.executable;
	for (const auto& argument : launch.arguments)
		text += " " + argument;
	if (!launch.workingDirectory.empty())
		text += "\nin " + launch.workingDirectory;
	return text;
}
static void ClippedText(const char* text, float width, ImU32 color)
{
	const auto position = ImGui::GetCursorScreenPos();
	const float height = ImGui::GetTextLineHeight();
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect(position, {position.x + width, position.y + height}, true);
	draw->AddText(position, color, text);
	draw->PopClipRect();
	ImGui::Dummy({width, height});
}
// Keeps the detail beside the actions when both fit, otherwise above them.
static void AppDetail(const std::string& detail, std::initializer_list<const char*> labels)
{
	const auto& style = ImGui::GetStyle();
	float actionsWidth = 0;
	for (const auto* label : labels)
		actionsWidth += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
	const float available = ImGui::GetContentRegionAvail().x;
	const bool sameLine = available >= actionsWidth + ImGui::GetFontSize() * 4;
	ClippedText(detail.c_str(), sameLine ? available - actionsWidth : available, theme::TextDim);
	ImGui::SetItemTooltip("%s", detail.c_str());
	if (sameLine)
		ImGui::SameLine();
}
std::string AppPicker::Refresh()
{
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
								ImU32 dot, const std::string& tooltip, bool open, const std::function<void()>& session,
								float maximumWidth)
{
	AppPickerChoice choice;
	const float height = ImGui::GetFrameHeight(), unit = ImGui::GetFontSize() / 17;
	const auto text = ImGui::CalcTextSize(label.c_str());
	const float extra = (dot ? 14 * unit : 0) + 18 * unit;
	const float fullWidth = height + text.x + 8 * unit + extra;
	const ImVec2 size{maximumWidth > 0 ? std::min(fullWidth, maximumWidth) : fullWidth, height};
	const float dotX = size.x - extra, chevronX = size.x - 18 * unit;
	const auto minimum = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton("##App picker", size, ImGuiButtonFlags_EnableNav);
	const bool hovered = ImGui::IsItemHovered(), shown = ImGui::IsPopupOpen("App picker");
	auto* draw = ImGui::GetWindowDrawList();
	if (hovered || shown)
		draw->AddRectFilled(minimum, {minimum.x + size.x, minimum.y + size.y},
							theme::Mix(theme::Bg, theme::Text, shown ? .1f : .06f), theme::Radius * unit);
	DrawIcon(draw, Icon::App, {minimum.x + height / 2, minimum.y + height / 2}, height * .6f, theme::TextDim);
	draw->PushClipRect({minimum.x + height, minimum.y},
					   {minimum.x + std::max(height, dotX - 4 * unit), minimum.y + height}, true);
	draw->AddText({minimum.x + height, minimum.y + (height - text.y) / 2}, theme::Text, label.c_str());
	draw->PopClipRect();
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
	const auto* viewport = ImGui::GetWindowViewport();
	const float availableWidth = std::max(1.0f, viewport->WorkSize.x - ImGui::GetStyle().DisplaySafeAreaPadding.x * 2);
	const float popupWidth = std::min(ImGui::GetFontSize() * 40, availableWidth);
	ImGui::SetNextWindowSizeConstraints({std::min(ImGui::GetFontSize() * 24, popupWidth), 0}, {popupWidth, FLT_MAX});
	if (!ImGui::BeginPopup("App picker"))
		return choice;
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
		ClippedText(app.name.c_str(), ImGui::GetContentRegionAvail().x, theme::Text);
		ImGui::SetItemTooltip("%s\n%s%s%s", app.name.c_str(), app.executable.c_str(),
							  app.executable.empty() ? "" : "\n", app.endpoint.c_str());
		std::string detail = app.process ? "PID " + std::to_string(app.process) : std::string{};
		if (app.started)
			detail += " · started " + Clock(*app.started);
		AppDetail(detail, {connected ? "Connected" : "Connect"});
		if (connected)
			ImGui::TextColored(theme::Vec(theme::Ok), "Connected");
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
		const auto& name = recent.launch.name.empty() ? recent.launch.executable : recent.launch.name;
		ClippedText(name.c_str(), ImGui::GetContentRegionAvail().x, theme::Text);
		ImGui::SetItemTooltip("%s\n%s", name.c_str(), CommandLine(recent.launch).c_str());
		AppDetail(Age(recent.lastUsed), {"Launch", "Save as profile"});
		const float actionsRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		const auto problem = PathProblem(recent.launch.executable, PathKind::Executable);
		ImGui::BeginDisabled(!problem.empty());
		if (ImGui::SmallButton("Launch"))
		{
			choice.launchRecent = recent.launch;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		const float launchRight = ImGui::GetItemRectMax().x;
		if (!problem.empty() &&
			ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s: %s", problem.c_str(), recent.launch.executable.c_str());
		const float saveWidth = ImGui::CalcTextSize("Save as profile").x + ImGui::GetStyle().FramePadding.x * 2;
		if (launchRight + ImGui::GetStyle().ItemSpacing.x + saveWidth <= actionsRight)
			ImGui::SameLine();
		if (ImGui::SmallButton("Save as profile"))
		{
			choice.saveRecent = index;
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopID();
	}

	SectionLabel("Profiles");
	ImGui::PushID("Profiles");
	for (std::size_t index = 0; index < settings.profiles.size(); ++index)
	{
		const auto& profile = settings.profiles[index];
		ImGui::PushID(static_cast<int>(index));
		const auto position = ImGui::GetCursorScreenPos();
		if (ImGui::Selectable("##Launch profile", false, 0, {0, ImGui::GetTextLineHeight()}))
			choice.launchProfile = static_cast<int>(index);
		else
			ImGui::SetItemTooltip("Launch %s", CommandLine(profile).c_str());
		ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), profile.name.c_str());
		ImGui::PopID();
	}
	ImGui::PopID();
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
