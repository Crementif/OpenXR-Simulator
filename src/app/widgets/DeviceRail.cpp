#include "app/widgets/DeviceRail.h"
#include "app/UiPch.h"
#include "app/services/PlayerRig.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"

#include <tuple>

namespace simxr::app
{
struct DeviceRow
{
	const char* label;
	Icon icon;
	std::optional<ControlChannel> channel;
	std::optional<PilotAction> key;
	ImU32 tint;
};

static constexpr std::array<DeviceRow, 7> s_rows{{
	{"Player", Icon::Player, {}, PilotAction::TargetPlayer, 0},
	{"Head", Icon::Head, ControlChannel::Head, PilotAction::TargetHead, 0},
	{"Left", Icon::HandLeft, ControlChannel::LeftPose, PilotAction::TargetLeft, theme::Left},
	{"Right", Icon::HandRight, ControlChannel::RightPose, PilotAction::TargetRight, theme::Right},
	{"Body", Icon::Body, ControlChannel::Body, {}, 0},
	{"Gaze", Icon::Eye, ControlChannel::Gaze, {}, 0},
	{"Trackers", Icon::Tracker, ControlChannel::Trackers, {}, 0},
}};

static bool IsTracked(const PoseSample& pose)
{
	return (pose.flags & TrackedPoseFlags) == TrackedPoseFlags;
}

static std::pair<ImU32, const char*> DeviceDot(DevicePage page, const ObservedSetup& setup)
{
	const auto& tracking = setup.values.tracking;
	const auto scenario = [&](FailureScenario failure) {
		return setup.scenarios[static_cast<std::size_t>(failure)];
	};
	switch (page)
	{
	case DevicePage::Head:
		if (scenario(FailureScenario::HeadUnavailable))
			return {theme::TextDim, "Off"};
		if (!(tracking.head.flags & ValidPoseFlags))
			return {theme::TextDim, "Unavailable"};
		if (scenario(FailureScenario::HeadUntracked))
			return {theme::Warn, "Not tracked"};
		return IsTracked(tracking.head) ? std::pair{theme::Ok, "Tracked"} : std::pair{theme::Warn, "Not tracked"};
	case DevicePage::Left:
	case DevicePage::Right:
	{
		const bool right = page == DevicePage::Right;
		const auto& controller = tracking.controllers[right ? 1 : 0];
		if (!controller.connected ||
			scenario(right ? FailureScenario::RightDisconnected : FailureScenario::LeftDisconnected))
			return {theme::TextDim, "Off"};
		return IsTracked(controller.pose) ? std::pair{theme::Ok, "Tracked"} : std::pair{theme::Warn, "Not tracked"};
	}
	case DevicePage::Body:
		if (!tracking.body.active || scenario(FailureScenario::BodyUnavailable))
			return {theme::TextDim, "Off"};
		if (scenario(FailureScenario::BodyUntracked))
			return {theme::Warn, "Not tracked"};
		if (std::ranges::any_of(tracking.body.controls,
								[](const BodyControlOverride& control) { return !control.tracked; }) ||
			std::ranges::any_of(tracking.body.joints,
								[](const std::optional<BodyJointOverride>& joint) { return joint && !joint->tracked; }))
			return {theme::Warn, "Some not tracked"};
		return {theme::Ok, "Tracked"};
	case DevicePage::Gaze:
		if (scenario(FailureScenario::GazeUnavailable))
			return {theme::TextDim, "Off"};
		if (!tracking.gaze.available)
			return {theme::TextDim, "Unavailable"};
		if (scenario(FailureScenario::GazeUntracked))
			return {theme::Warn, "Not tracked"};
		return tracking.gaze.tracked ? std::pair{theme::Ok, "Tracked"} : std::pair{theme::Warn, "Not tracked"};
	case DevicePage::Trackers:
	{
		if (scenario(FailureScenario::TrackersDisconnected))
			return {theme::TextDim, "Off"};
		const auto connected = std::ranges::count_if(tracking.trackers, &TrackerDevice::connected);
		const auto lost = std::ranges::count_if(
			tracking.trackers, [](const TrackerDevice& tracker) { return tracker.connected && !tracker.tracked; });
		if (!connected)
			return {theme::TextDim, "None connected"};
		return lost ? std::pair{theme::Warn, "Some not tracked"} : std::pair{theme::Ok, "Tracked"};
	}
	case DevicePage::Player:
		return {0, nullptr};
	}
	return {0, nullptr};
}

static const char* OwnerBadge(const std::optional<CommandSource>& owner)
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

struct DeviceTabStatus
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

static DeviceTabStatus ReadTabStatus(std::size_t index, const RuntimeView& view, const CompanionSettings& settings)
{
	DeviceTabStatus result;
	const auto& row = s_rows[index];
	if (row.key)
		result.key = PilotBindingLabel(settings.pilotBindings[static_cast<std::size_t>(*row.key)]);
	if (view.setup && view.connected)
	{
		const char* tooltip = nullptr;
		std::tie(result.status.dot, tooltip) = DeviceDot(static_cast<DevicePage>(index), *view.setup);
		result.tooltip = tooltip ? tooltip : "";
		if (index == static_cast<std::size_t>(DevicePage::Trackers) &&
			!view.setup->scenarios[static_cast<std::size_t>(FailureScenario::TrackersDisconnected)])
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

static bool IsSelected(std::size_t index, const Selection& selection)
{
	const auto device = std::get_if<DeviceSel>(&selection);
	return device && index == static_cast<std::size_t>(device->page);
}

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
	const float height = std::clamp((ImGui::GetContentRegionAvail().y - spacing * (s_rows.size() - 1)) / s_rows.size(),
									ImGui::GetFontSize() + 4, 36 * unit);
	for (std::size_t index = 0; index < s_rows.size(); ++index)
	{
		auto status = ReadTabStatus(index, view, settings);
		status.status.iconOnly = true;
		if (RailItem(s_rows[index].label, s_rows[index].icon, IsSelected(index, selection), status.View(), height))
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
	const float icon = 34 * unit;
	std::size_t current = s_rows.size();
	for (std::size_t index = 0; index < s_rows.size(); ++index)
		if (IsSelected(index, selection))
			current = index;
	float named = 0;
	if (current < s_rows.size())
		named = icon + ImGui::CalcTextSize(s_rows[current].label).x + 10 * unit;
	if (named && (width - named) / (s_rows.size() - 1) < icon)
		named = 0;
	const float other = named ? (width - named) / (s_rows.size() - 1) : width / s_rows.size();
	const float bottom = origin.y + height - unit;
	ImGui::GetWindowDrawList()->AddLine({origin.x, bottom}, {origin.x + width, bottom}, theme::Line, unit);
	float x = origin.x;
	for (std::size_t index = 0; index < s_rows.size(); ++index)
	{
		const bool selected = index == current;
		const float tab = selected && named ? named : other;
		ImGui::SetCursorScreenPos({x, origin.y});
		const auto status = ReadTabStatus(index, view, settings);
		if (DeviceTab(s_rows[index].label, s_rows[index].icon, selected, status.View(), {tab, height},
					  selected && named))
			picked = DeviceSel{static_cast<DevicePage>(index)};
		x += tab;
	}
	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy({width, height});
	return picked;
}
} // namespace simxr::app
