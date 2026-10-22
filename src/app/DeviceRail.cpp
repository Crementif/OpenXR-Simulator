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
};
constexpr std::array<Row, 10> Rows{{
	{"Player", Icon::Player, {}, PilotAction::TargetPlayer},
	{"Head", Icon::Head, ControlChannel::Head, PilotAction::TargetHead},
	{"Left", Icon::HandLeft, ControlChannel::LeftPose, PilotAction::TargetLeft},
	{"Right", Icon::HandRight, ControlChannel::RightPose, PilotAction::TargetRight},
	{"Body", Icon::Body, ControlChannel::Body, {}},
	{"Gaze", Icon::Eye, ControlChannel::Gaze, {}},
	{"Trackers", Icon::Tracker, ControlChannel::Trackers, {}},
	{"Headset", Icon::Headset, {}, {}},
	{"Scenarios", Icon::Scenarios, {}, {}},
	{"Library", Icon::Library, {}, {}},
}};
// Rows from this index on are under SETUP.
constexpr std::size_t SetupRow = 7;
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
float IconRailWidth()
{
	return 34 * ImGui::GetFontSize() / 17 + ImGui::GetStyle().WindowPadding.x * 2;
}
std::optional<Selection> DrawDeviceRail(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings, bool iconOnly, float reserve)
{
	std::optional<Selection> picked;
	std::string scenarios;
	if (view.setup)
		if (const auto active = std::ranges::count(view.setup->scenarios, true))
			scenarios = std::to_string(active) + " on";
	const float unit = ImGui::GetFontSize() / 17;
	// Section labels are small caps text; the icon-only rail uses a divider instead.
	const auto section = [&](const char* name)
	{
		const auto position = ImGui::GetCursorScreenPos();
		auto* draw = ImGui::GetWindowDrawList();
		if (iconOnly)
		{
			draw->AddLine({position.x + 6 * unit, position.y + 4 * unit},
						  {position.x + ImGui::GetContentRegionAvail().x - 6 * unit, position.y + 4 * unit}, theme::Line,
						  unit);
			ImGui::Dummy({0, 8 * unit});
			return;
		}
		ImGui::SetWindowFontScale(.78f);
		draw->AddText({position.x + 8 * unit, position.y + 4 * unit}, theme::TextDim, name);
		ImGui::Dummy({0, ImGui::GetFontSize() + 6 * unit});
		ImGui::SetWindowFontScale(1);
	};
	// Rows shrink (down to one text line) rather than clip when the window is short.
	const float spacing = ImGui::GetStyle().ItemSpacing.y;
	const float labels = iconOnly ? 2 * (8 * unit + spacing) : 2 * (ImGui::GetFontSize() * .78f + 6 * unit + spacing);
	const float height =
		std::clamp((ImGui::GetContentRegionAvail().y - reserve - labels - spacing * (Rows.size() - 1)) / Rows.size(),
				   ImGui::GetFontSize() + 4, 36 * unit);
	const bool library = std::holds_alternative<LibrarySel>(selection) || std::holds_alternative<TakeSel>(selection) ||
						 std::holds_alternative<ScriptSel>(selection);
	for (std::size_t index = 0; index < Rows.size(); ++index)
	{
		if (index == 0 || index == SetupRow)
			section(index == 0 ? "DEVICES" : "SETUP");
		const auto& row = Rows[index];
		bool selected = index == 9 && library;
		if (const auto device = std::get_if<DeviceSel>(&selection))
			selected = index == static_cast<std::size_t>(device->page);
		if (const auto item = std::get_if<SessionItemSel>(&selection))
			selected = index == SetupRow + static_cast<std::size_t>(item->item);
		RailStatus status;
		status.iconOnly = iconOnly;
		std::string key, trackerStatus;
		if (row.key)
		{
			key = PilotBindingLabel(settings.pilotBindings[static_cast<std::size_t>(*row.key)]);
			status.key = key.c_str();
		}
		else if (index == 9)
		{
			status.key = "Ctrl+L";
			status.pilotKey = false;
		}
		if (view.setup && view.connected)
		{
			std::tie(status.dot, status.dotTooltip) = DeviceDot(index, *view.setup);
			if (index == 6)
			{
				trackerStatus = status.dotTooltip;
				for (const auto& tracker : view.setup->values.tracking.trackers)
					if (tracker.connected)
					{
						trackerStatus += "\n";
						trackerStatus += TrackerRoleName(tracker.role);
						if (!tracker.tracked)
							trackerStatus += " (not tracked)";
					}
				status.dotTooltip = trackerStatus.c_str();
			}
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
			if (index < SetupRow)
				picked = DeviceSel{static_cast<DevicePage>(index)};
			else if (index < 9)
				picked = SessionItemSel{static_cast<SessionItem>(index - SetupRow)};
			else
				picked = LibrarySel{};
		}
	}
	return picked;
}
} // namespace simxr::app
