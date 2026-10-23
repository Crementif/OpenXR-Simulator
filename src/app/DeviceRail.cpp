#include "app/DeviceRail.h"
#include "app/Theme.h"
#include "app/PlayerRig.h"
#include "app/widgets/Controls.h"

#include <algorithm>
#include <array>
#include <string>

namespace simxr::app
{
namespace
{
struct Row
{
	const char* label;
	Icon icon;
	std::optional<ControlChannel> channel;
	std::optional<PilotAction> key;
	// Hands keep their colour so Left and Right read apart as icons.
	ImU32 tint;
};
constexpr std::array<Row, 7> Rows{{
	{"Player", Icon::Player, {}, PilotAction::TargetPlayer, 0},
	{"Head", Icon::Head, ControlChannel::Head, PilotAction::TargetHead, 0},
	{"Left", Icon::HandLeft, ControlChannel::LeftPose, PilotAction::TargetLeft, theme::Left},
	{"Right", Icon::HandRight, ControlChannel::RightPose, PilotAction::TargetRight, theme::Right},
	{"Body", Icon::Body, ControlChannel::Body, {}, 0},
	{"Gaze", Icon::Eye, ControlChannel::Gaze, {}, 0},
	{"Trackers", Icon::Tracker, ControlChannel::Trackers, {}, 0},
}};
bool Tracked(const PoseSample& pose)
{
	return (pose.flags & TrackedPoseFlags) == TrackedPoseFlags;
}
// Dot colour and its tooltip for one row, from the applied device state.
std::pair<ImU32, const char*> DeviceDot(std::size_t row, const ObservedSetup& setup)
{
	const auto& tracking = setup.values.tracking;
	switch (row)
	{
	case 1:
		return Tracked(tracking.head) ? std::pair{theme::Ok, "Tracked"} : std::pair{theme::Warn, "Not tracked"};
	case 2:
	case 3:
	{
		const auto& controller = tracking.controllers[row - 2];
		if (!controller.connected)
			return {theme::TextDim, "Off"};
		return Tracked(controller.pose) ? std::pair{theme::Ok, "Tracked"} : std::pair{theme::Warn, "Not tracked"};
	}
	case 4:
		return tracking.body.active ? std::pair{theme::Ok, "Active"} : std::pair{theme::TextDim, "Off"};
	case 5:
		if (!tracking.gaze.available)
			return {theme::TextDim, "Unavailable"};
		return tracking.gaze.tracked ? std::pair{theme::Ok, "Tracked"} : std::pair{theme::Warn, "Not tracked"};
	case 6:
	{
		const auto connected = std::ranges::count_if(tracking.trackers, &TrackerDevice::connected);
		const auto lost = std::ranges::count_if(tracking.trackers,
												[](const TrackerDevice& tracker)
												{
													return tracker.connected && !tracker.tracked;
												});
		if (!connected)
			return {theme::TextDim, "None connected"};
		return lost ? std::pair{theme::Warn, "Some not tracked"} : std::pair{theme::Ok, "Tracked"};
	}
	default:
		return {0, nullptr};
	}
}
const char* OwnerBadge(const std::optional<CommandSource>& owner)
{
	if (!owner)
		return nullptr;
	switch (owner->kind)
	{
	case CommandSourceKind::Rig:
		return "Script";
	case CommandSourceKind::Mcp:
		return "MCP";
	case CommandSourceKind::Sequence:
		return "Replay";
	case CommandSourceKind::Manual:
		break;
	}
	return nullptr;
}
// A tab's dot, owner badge and key, with the text they point to.
struct TabStatus
{
	RailStatus status;
	std::string key, tooltip;
	RailStatus View() const
	{
		auto result = status;
		result.key = key.empty() ? nullptr : key.c_str();
		result.dotTooltip = tooltip.empty() ? nullptr : tooltip.c_str();
		return result;
	}
};
TabStatus Status(std::size_t index, const RuntimeView& view, const CompanionSettings& settings)
{
	TabStatus result;
	const auto& row = Rows[index];
	if (row.key)
		result.key = PilotBindingLabel(settings.pilotBindings[static_cast<std::size_t>(*row.key)]);
	if (view.setup && view.connected)
	{
		const char* tooltip = nullptr;
		std::tie(result.status.dot, tooltip) = DeviceDot(index, *view.setup);
		result.tooltip = tooltip ? tooltip : "";
		if (index == 6)
			for (const auto& tracker : view.setup->values.tracking.trackers)
				if (tracker.connected)
				{
					result.tooltip += "\n";
					result.tooltip += TrackerRoleName(tracker.role);
					if (!tracker.tracked)
						result.tooltip += " (not tracked)";
				}
		if (row.channel)
			result.status.badge = OwnerBadge(view.setup->owners[static_cast<std::size_t>(*row.channel)]);
		result.status.badgeColor = theme::Head;
	}
	result.status.tint = row.tint;
	return result;
}
bool Selected(std::size_t index, const Selection& selection)
{
	const auto device = std::get_if<DeviceSel>(&selection);
	return device && index == static_cast<std::size_t>(device->page);
}
} // namespace
float RailWidth()
{
	return 34 * ImGui::GetFontSize() / 17 + ImGui::GetStyle().WindowPadding.x * 2;
}
float DeviceTabsHeight()
{
	return 40 * ImGui::GetFontSize() / 17;
}
std::optional<Selection> DrawDeviceRail(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings)
{
	std::optional<Selection> picked;
	const float unit = ImGui::GetFontSize() / 17, spacing = ImGui::GetStyle().ItemSpacing.y;
	// Rows shrink (down to one text line) rather than clip when the window is short.
	const float height = std::clamp((ImGui::GetContentRegionAvail().y - spacing * (Rows.size() - 1)) / Rows.size(),
									ImGui::GetFontSize() + 4, 36 * unit);
	for (std::size_t index = 0; index < Rows.size(); ++index)
	{
		auto status = Status(index, view, settings);
		status.status.iconOnly = true;
		if (RailItem(Rows[index].label, Rows[index].icon, Selected(index, selection), status.View(), height))
			picked = DeviceSel{static_cast<DevicePage>(index)};
	}
	return picked;
}
std::optional<Selection> DrawDeviceTabs(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings)
{
	std::optional<Selection> picked;
	const float unit = ImGui::GetFontSize() / 17, height = DeviceTabsHeight();
	const float width = ImGui::GetContentRegionAvail().x;
	const auto origin = ImGui::GetCursorScreenPos();
	// The selected tab is wide enough for its name; the others share the rest.
	const float icon = 34 * unit;
	std::size_t current = Rows.size();
	for (std::size_t index = 0; index < Rows.size(); ++index)
		if (Selected(index, selection))
			current = index;
	float named = 0;
	if (current < Rows.size())
		named = icon + ImGui::CalcTextSize(Rows[current].label).x + 10 * unit;
	if (named && (width - named) / (Rows.size() - 1) < icon)
		named = 0;
	const float other = named ? (width - named) / (Rows.size() - 1) : width / Rows.size();
	const float bottom = origin.y + height - unit;
	ImGui::GetWindowDrawList()->AddLine({origin.x, bottom}, {origin.x + width, bottom}, theme::Line, unit);
	float x = origin.x;
	for (std::size_t index = 0; index < Rows.size(); ++index)
	{
		const bool selected = index == current;
		const float tab = selected && named ? named : other;
		ImGui::SetCursorScreenPos({x, origin.y});
		const auto status = Status(index, view, settings);
		if (DeviceTab(Rows[index].label, Rows[index].icon, selected, status.View(), {tab, height}, selected && named))
			picked = DeviceSel{static_cast<DevicePage>(index)};
		x += tab;
	}
	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy({width, height});
	return picked;
}
} // namespace simxr::app
