#include "app/DeviceRail.h"
#include "app/Theme.h"
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
};
constexpr std::array<Row, 9> Rows{{
	{"Player", Icon::Player, {}, PilotAction::TargetPlayer},
	{"Head", Icon::Head, ControlChannel::Head, PilotAction::TargetHead},
	{"Left", Icon::HandLeft, ControlChannel::LeftPose, PilotAction::TargetLeft},
	{"Right", Icon::HandRight, ControlChannel::RightPose, PilotAction::TargetRight},
	{"Body", Icon::Body, ControlChannel::Body, {}},
	{"Gaze", Icon::Eye, ControlChannel::Gaze, {}},
	{"Trackers", Icon::Tracker, ControlChannel::Trackers, {}},
	{"Headset", Icon::Headset, {}, {}},
	{"Scenarios", Icon::Scenarios, {}, {}},
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
} // namespace
float NarrowRailWidth()
{
	float label = 0;
	for (const auto& row : Rows)
		label = std::max(label, ImGui::CalcTextSize(row.label).x);
	const float unit = ImGui::GetFontSize() / 17;
	return label + 46 * unit + ImGui::GetStyle().WindowPadding.x * 2;
}
bool DrawDeviceRail(Selection& selection, const RuntimeView& view, const CompanionSettings& settings)
{
	bool picked = false;
	std::string scenarios;
	if (view.setup)
		if (const auto active = std::ranges::count(view.setup->scenarios, true))
			scenarios = std::to_string(active) + " on";
	// Rows shrink (down to one text line) rather than clip when the window is short.
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	const float height = std::clamp((ImGui::GetContentRegionAvail().y - spacing * (Rows.size() - 1)) / Rows.size(),
									ImGui::GetFontSize() + 4, 36 * ImGui::GetFontSize() / 17);
	for (std::size_t index = 0; index < Rows.size(); ++index)
	{
		const auto& row = Rows[index];
		bool selected = false;
		if (const auto device = std::get_if<DeviceSel>(&selection))
			selected = index == static_cast<std::size_t>(device->page);
		if (const auto item = std::get_if<SessionItemSel>(&selection))
			selected = index == 7 + static_cast<std::size_t>(item->item);
		RailStatus status;
		std::string key;
		if (row.key)
		{
			key = PilotBindingLabel(settings.pilotBindings[static_cast<std::size_t>(*row.key)]);
			status.key = key.c_str();
		}
		if (view.setup && view.connected)
		{
			std::tie(status.dot, status.dotTooltip) = DeviceDot(index, *view.setup);
			if (row.channel)
				status.badge = OwnerBadge(view.setup->owners[static_cast<std::size_t>(*row.channel)]);
			status.badgeColor = theme::Head;
			if (index == 8 && !scenarios.empty())
			{
				status.badge = scenarios.c_str();
				status.badgeColor = theme::Warn;
			}
		}
		if (RailItem(row.label, row.icon, selected, status, height))
		{
			if (index < 7)
				selection = DeviceSel{static_cast<DevicePage>(index)};
			else
				selection = SessionItemSel{static_cast<SessionItem>(index - 7)};
			picked = true;
		}
	}
	return picked;
}
} // namespace simxr::app
