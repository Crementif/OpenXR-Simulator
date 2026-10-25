#include "Pch.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include <imgui.h>

namespace simxr::app
{
std::optional<SimulationEdit> DeviceControlPanel::DrawAdvanced()
{
	m_edit.reset();
	m_continuousEdit = false;
	DrawOwnership();
	return std::exchange(m_edit, {});
}
std::optional<SimulationEdit> DeviceControlPanel::DrawSessionMenu()
{
	m_edit.reset();
	m_continuousEdit = false;
	const bool ready = m_observed && m_configurationLoaded;
	ImGui::BeginDisabled(!ready || !CanControl(ControlChannel::Session));
	struct Toggle
	{
		FailureScenario scenario;
		const char* label;
		const char* tooltip;
	};
	static constexpr Toggle toggles[]{
		{FailureScenario::Unfocused, "Unfocused", "The app loses input focus, as when a system menu opens"},
		{FailureScenario::Hidden, "Hidden", "The app is not visible, as when the headset is taken off"}};
	for (const auto& toggle : toggles)
	{
		const bool on = ready && m_observed->scenarios[static_cast<std::size_t>(toggle.scenario)];
		if (ImGui::MenuItem(toggle.label, nullptr, on))
			ApplyOwned(ControlChannel::Session, SetScenario{toggle.scenario, !on});
		ImGui::SetItemTooltip("%s", toggle.tooltip);
	}
	if (ImGui::BeginMenu("Session loss"))
	{
		ImGui::TextDisabled("The app must recreate its session or relaunch");
		if (ImGui::MenuItem("Request session loss"))
			ApplyOwned(ControlChannel::Session, SetScenario{FailureScenario::SessionLoss, true});
		ImGui::EndMenu();
	}
	ImGui::EndDisabled();
	return std::exchange(m_edit, {});
}
void DeviceControlPanel::DrawOwnership()
{
	ImGui::Text("Target frame");
	ImGui::SetNextItemWidth(180);
	ImGui::InputScalar("##Target frame", ImGuiDataType_U64, &m_applyFrame);
	ImGui::Checkbox("Reschedule late edits", &m_reschedule);
	static constexpr const char* channels[] = {"Head",			 "Left pose",	"Right pose", "Left controls",
											   "Right controls", "Body",		"Gaze",		  "Trackers",
											   "Views",			 "Proportions", "Profiles",	  "Session scenarios"};
	ImGui::Text("Input control");
	ImGui::Combo("##Input ownership", &m_selectedChannel, channels, IM_ARRAYSIZE(channels));
	DrawOwner(static_cast<ControlChannel>(m_selectedChannel), false);
	ImGui::Checkbox("Replace owner", &m_replaceOwner);
	if (ImGui::Button("Take control"))
		Apply({TakeControl{static_cast<ControlChannel>(m_selectedChannel), m_replaceOwner}});
	ImGui::SameLine();
	if (ImGui::Button("Release control"))
		Apply({ReleaseControl{static_cast<ControlChannel>(m_selectedChannel)}});
	if (ImGui::Button("Release all manual inputs"))
		m_edit = ReleaseManualInputs{};
	ImGui::TextUnformatted("Input sources");
}
void DeviceControlPanel::DrawTracking(DevicePage page, float titleWidth)
{
	// A device's failure scenarios as one switch: lost tracking, or the device gone.
	std::optional<FailureScenario> lost, off;
	switch (page)
	{
	case DevicePage::Head:
		lost = FailureScenario::HeadUntracked;
		off = FailureScenario::HeadUnavailable;
		break;
	case DevicePage::Left:
		off = FailureScenario::LeftDisconnected;
		break;
	case DevicePage::Right:
		off = FailureScenario::RightDisconnected;
		break;
	case DevicePage::Body:
		lost = FailureScenario::BodyUntracked;
		off = FailureScenario::BodyUnavailable;
		break;
	case DevicePage::Gaze:
		lost = FailureScenario::GazeUntracked;
		off = FailureScenario::GazeUnavailable;
		break;
	case DevicePage::Trackers:
		off = FailureScenario::TrackersDisconnected;
		break;
	case DevicePage::Player:
		return;
	}
	const auto active = [&](std::optional<FailureScenario> scenario)
	{
		return scenario && m_observed && m_observed->scenarios[static_cast<std::size_t>(*scenario)];
	};
	static constexpr const char* tracked[] = {"Tracked", "Lost", "Off"};
	static constexpr const char* trackedTips[] = {"Normal tracking",
												  "Simulate lost tracking: the app sees an untracked pose",
												  "Simulate the device being unavailable to the app"};
	static constexpr const char* connected[] = {"Connected", "Off"};
	static constexpr const char* connectedTips[] = {"Normal connection",
													"Disconnect the device. Switch back to reconnect."};
	const std::span<const char* const> labels = lost ? std::span<const char* const>(tracked) : connected;
	const std::span<const char* const> tips = lost ? std::span<const char* const>(trackedTips) : connectedTips;
	const int last = static_cast<int>(labels.size()) - 1;
	const int current = active(off) ? last : active(lost) ? 1 : 0;
	const auto channel = ScenarioInputChannel(*off);
	float labelWidth = 0;
	for (const auto* label : labels)
		labelWidth = std::max(labelWidth, ImGui::CalcTextSize(label).x);
	const auto& style = ImGui::GetStyle();
	const float width = (labelWidth + style.FramePadding.x * 2) * labels.size();
	const float available = ImGui::GetContentRegionAvail().x;
	if (titleWidth + width + ImGui::GetFrameHeight() + style.ItemSpacing.x * 2 <= available)
		ImGui::SameLine(ImGui::GetCursorPosX() + available - ImGui::GetFrameHeight() - style.ItemSpacing.x - width);
	ImGui::BeginDisabled(!m_configurationLoaded || !CanControl(channel));
	ImGui::PushID("Tracking");
	int choice = -1;
	if (width <= available)
		choice = Segmented(labels, current, width, tips);
	else
	{
		ImGui::SetNextItemWidth(available);
		int selected = current;
		if (ImGui::Combo("##State", &selected, labels.data(), static_cast<int>(labels.size())))
			choice = selected;
		ImGui::SetItemTooltip("%s", tips[current]);
	}
	ImGui::PopID();
	ImGui::EndDisabled();
	if (choice < 0 || choice == current || !CanControl(channel))
		return;
	std::vector<SimulationCommand> commands{TakeControl{channel, false}};
	if (lost)
		commands.emplace_back(SetScenario{*lost, choice == 1});
	commands.emplace_back(SetScenario{*off, choice == last});
	Apply(std::move(commands));
}
void DeviceControlPanel::DrawOwner(ControlChannel channel, bool takeover)
{
	if (!m_observed)
	{
		return;
	}
	// You are the implicit owner; only another driver gets a badge.
	const auto& owner = m_observed->owners[static_cast<std::size_t>(channel)];
	if (!owner || owner == m_manualSource)
		return;
	const char* name = "Unknown source";
	switch (owner->kind)
	{
	case CommandSourceKind::Manual:
		name = "Another companion";
		break;
	case CommandSourceKind::Mcp:
		name = "MCP client";
		break;
	case CommandSourceKind::Rig:
		name = "Script";
		break;
	case CommandSourceKind::Sequence:
		name = "Replay";
		break;
	}
	ImGui::TextColored(theme::Vec(theme::Head), "Driven by %s", name);
	ImGui::SetItemTooltip("Input source %llu controls this input.", static_cast<unsigned long long>(owner->id));
	if (takeover)
	{
		ImGui::SameLine();
		ImGui::PushID(static_cast<int>(channel));
		if (ImGui::SmallButton("Take over"))
			Apply({TakeControl{channel, true}});
		ImGui::PopID();
	}
}
} // namespace simxr::app
