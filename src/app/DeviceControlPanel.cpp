#include "app/DeviceControlPanel.h"
#include "app/Theme.h"
#include "Math.h"
#include "app/SpatialMath.h"
#include "app/widgets/Controls.h"
#include "app/widgets/InputPad.h"
#include "app/widgets/LiveField.h"
#include "simulation/BodyModel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <string_view>

#include <cstdio>
#include <exception>
#include <imgui.h>
#include <utility>

namespace simxr::app
{
DeviceControlPanel::DeviceControlPanel(PlayerRig& rig) : m_playerRig(rig) {}
std::optional<SimulationEdit> DeviceControlPanel::Draw(bool supported, const Selection* selection,
													   const std::function<void()>& placement)
{
	m_edit.reset();
	m_continuousEdit = false;
	if (m_observed && !m_pending && !ImGui::IsAnyItemActive())
		LoadObservedSettings();
	if (supported && !m_observed)
	{
		ImGui::TextDisabled("Waiting for runtime values\u2026");
		if (!m_observationError.empty())
			ImGui::SetItemTooltip("%s", m_observationError.c_str());
	}
	supported = supported && m_observed.has_value() && m_configurationLoaded;
	ImGui::PushItemWidth(-1);
	if (selection)
	{
		ImGui::BeginDisabled(!supported);
		const auto device = std::get_if<DeviceSel>(selection);
		const bool hand = device && (device->page == DevicePage::Left || device->page == DevicePage::Right);
		if (placement && !hand)
		{
			ImGui::EndDisabled();
			placement();
			ImGui::BeginDisabled(!supported);
		}
		if (device)
		{
			m_fieldScope = "device/" + std::to_string(static_cast<int>(device->page)) + "/";
			DrawTracking(device->page);
			switch (device->page)
			{
			case DevicePage::Player:
				DrawPlayer();
				break;
			case DevicePage::Head:
				DrawHead();
				DrawViews();
				break;
			case DevicePage::Left:
			case DevicePage::Right:
				m_controlHand = device->page == DevicePage::Right;
				DrawController(placement);
				break;
			case DevicePage::Body:
				DrawBody();
				break;
			case DevicePage::Gaze:
				DrawGaze();
				break;
			case DevicePage::Trackers:
				m_selectedTracker = static_cast<int>(std::min(device->slot, 3u));
				DrawTrackers();
				break;
			}
		}
		ImGui::EndDisabled();
	}
	ImGui::PopItemWidth();
	return std::exchange(m_edit, {});
}
bool DeviceControlPanel::Field(const char* key, bool changed, bool continuous, const FieldValue& value)
{
	const std::string field = m_fieldScope + key;
	const bool finished = continuous && ImGui::IsItemDeactivatedAfterEdit();
	changed |= finished;
	// A reset or wheel nudge is one complete edit.
	const bool acted = FieldActions(field.c_str(), value);
	changed |= acted;
	if (changed)
	{
		m_pendingFields.insert(field);
		m_continuousEdit = continuous && !finished && !acted;
		m_rejectedFields.erase(field);
	}
	DrawLiveField(m_pending && m_pendingFields.contains(field),
				  m_rejectedFields.contains(field) && ImGui::GetTime() < m_rejectedUntil ? m_deliveryError
																						 : std::string{});
	return changed;
}
bool DeviceControlPanel::InputField(const char* key, bool changed)
{
	const std::string field = m_fieldScope + key;
	if (changed)
	{
		m_pendingFields.insert(field);
		m_rejectedFields.erase(field);
	}
	DrawLiveField(false, m_rejectedFields.contains(field) && ImGui::GetTime() < m_rejectedUntil ? m_deliveryError
																							   : std::string{});
	return changed;
}
std::optional<SimulationEdit> DeviceControlPanel::DrawHeader(const Selection& selection)
{
	m_edit.reset();
	m_continuousEdit = false;
	const auto device = std::get_if<DeviceSel>(&selection);
	if (!device)
		return {};
	static constexpr const char* titles[] = {"Player", "Head",	   "Left hand", "Right hand",
											 "Body",   "Eye gaze", "Trackers"};
	static constexpr Icon icons[] = {Icon::Player, Icon::Head, Icon::HandLeft, Icon::HandRight,
									 Icon::Body,   Icon::Eye,  Icon::Tracker};
	const auto page = device->page;
	const bool ready = m_observed && m_configurationLoaded && m_playerRig.Ready();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	PageTitle(icons[static_cast<int>(page)], titles[static_cast<int>(page)]);
	if (page == DevicePage::Player)
	{
		const float width = ImGui::CalcTextSize("Reset all").x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(right - width);
		ImGui::BeginDisabled(!ready);
		if (ImGui::Button("Reset all"))
			ResetPage(page);
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Player, devices, body and trackers back to defaults. Settings stay.");
	}
	else
	{
		ImGui::SameLine(right - ImGui::GetFrameHeight());
		ImGui::BeginDisabled(!ready);
		const std::string tip =
			std::string("Restore defaults for ") + titles[static_cast<int>(page)] + "; other pages stay";
		if (IconButton("Reset page", Icon::ResetPose, tip.c_str()))
			ResetPage(page);
		ImGui::EndDisabled();
	}
	ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y / 2});
	return std::exchange(m_edit, {});
}
void DeviceControlPanel::ResetPage(DevicePage page)
{
	if (!m_observed || !m_playerRig.Ready())
		return;
	const auto allowed = [&](ControlChannel channel)
	{
		const auto& owner = m_observed->owners[static_cast<std::size_t>(channel)];
		return !owner || owner == m_manualSource;
	};
	std::vector<SimulationCommand> extra;
	const auto resetControls = [&](std::uint32_t hand)
	{
		const auto channel = hand ? ControlChannel::RightControls : ControlChannel::LeftControls;
		m_authoredControls[hand] = {};
		m_latches[hand] = {};
		if (!allowed(channel))
			return;
		extra.emplace_back(TakeControl{channel, false});
		extra.emplace_back(SetControllerControls{hand, ControllerControls{}});
	};
	switch (page)
	{
	case DevicePage::Player:
		m_playerRig.ResetAll();
		m_playerRig.height = m_observed->values.proportions.height;
		for (std::uint32_t hand = 0; hand < 2; ++hand)
			resetControls(hand);
		m_authoredTracking.gaze = {};
		if (allowed(ControlChannel::Gaze))
		{
			extra.emplace_back(TakeControl{ControlChannel::Gaze, false});
			extra.emplace_back(SetGaze{GazeInput{}});
		}
		break;
	case DevicePage::Head:
		m_playerRig.ResetHead();
		break;
	case DevicePage::Left:
	case DevicePage::Right:
		m_playerRig.ResetHand(page == DevicePage::Right);
		resetControls(page == DevicePage::Right);
		break;
	case DevicePage::Body:
		m_playerRig.ResetBody();
		break;
	case DevicePage::Gaze:
		m_authoredTracking.gaze = {};
		if (allowed(ControlChannel::Gaze))
			ApplyOwned(ControlChannel::Gaze, SetGaze{GazeInput{}});
		return;
	case DevicePage::Trackers:
		m_playerRig.ResetTracker(static_cast<std::size_t>(m_selectedTracker));
		break;
	}
	auto commands = PlayerEdit(true).commands;
	for (auto& command : commands)
		if (auto controller = std::get_if<SetControllerPose>(&command);
			controller && (page == DevicePage::Player ||
				(page == DevicePage::Left && controller->hand == 0) ||
				(page == DevicePage::Right && controller->hand == 1)))
			controller->value.connected = true;
	for (auto& command : extra)
		commands.push_back(std::move(command));
	m_continuousEdit = false;
	Apply(std::move(commands));
	if (auto edit = std::get_if<ManualTransaction>(&*m_edit))
		edit->releaseEdge = !extra.empty();
}
void DeviceControlPanel::ObserveDelivery(bool pending, const std::string& error)
{
	if (pending && error.empty())
		m_deliveryError.clear();
	if (!error.empty() && error != m_deliveryError)
	{
		m_deliveryError = error;
		m_rejectedFields = m_pendingFields;
		m_rejectedUntil = ImGui::GetTime() + 3;
	}
	m_pending = pending;
	if (!pending)
		m_pendingFields.clear();
}
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
	ImGui::BeginDisabled(!ready);
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
void DeviceControlPanel::Apply(std::vector<SimulationCommand> commands, std::string label)
{
	ManualTransaction transaction;
	transaction.continuous = m_continuousEdit;
	m_pending = true;
	if (m_applyFrame)
		transaction.frame = FrameId{m_applyFrame};
	transaction.latePolicy = m_reschedule ? CommandLatePolicy::RescheduleNext : CommandLatePolicy::Reject;
	transaction.commands = std::move(commands);
	transaction.label = std::move(label);
	m_edit = std::move(transaction);
}
void DeviceControlPanel::ApplyOwned(ControlChannel channel, SimulationCommand command)
{
	Apply({TakeControl{channel, m_replaceOwner}, std::move(command)});
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
	ImGui::Text("Input ownership");
	ImGui::Combo("##Input ownership", &m_selectedChannel, channels, IM_ARRAYSIZE(channels));
	DrawOwner(static_cast<ControlChannel>(m_selectedChannel));
	ImGui::Checkbox("Replace owner", &m_replaceOwner);
	if (ImGui::Button("Take control"))
		Apply({TakeControl{static_cast<ControlChannel>(m_selectedChannel), m_replaceOwner}});
	ImGui::SameLine();
	if (ImGui::Button("Release control"))
		Apply({ReleaseControl{static_cast<ControlChannel>(m_selectedChannel)}});
	if (ImGui::Button("Release all manual inputs"))
		m_edit = ReleaseManualInputs{};
	ImGui::TextUnformatted("Channel ownership");
}
void DeviceControlPanel::ClearHeldInputs()
{
	m_authoredControls = {};
	m_latches = {};
}
void DeviceControlPanel::ResetObservations()
{
	m_observed.reset();
	m_playerRig = {};
	m_rigPiloting = false;
	m_manualSource.reset();
	m_observationError.clear();
	m_configurationLoaded = false;
	m_applyFrame = 0;
	m_replaceOwner = false;
	m_edit.reset();
	m_pending = false;
	m_pendingFields.clear();
	m_rejectedFields.clear();
	m_deliveryError.clear();
}
void DeviceControlPanel::LoadObservedSettings()
{
	if (!m_observed)
		return;
	const auto& values = m_observed->values;
	if (!m_rigPiloting)
		m_playerRig.Observe(*m_observed);
	m_authoredTracking = values.tracking;
	m_authoredControls = values.input.controllers;
	m_authoredViews = values.views;
	m_authoredProportions = values.proportions;
	m_trackerRole = static_cast<int>(values.tracking.trackers[static_cast<std::size_t>(m_selectedTracker)].role);
	for (unsigned hand = 0; hand != 2; ++hand)
	{
		auto& profile = m_controllerProfiles[hand];
		std::snprintf(profile.data(), profile.size(), "%s", values.input.preferredProfiles[hand].c_str());
	}
	m_loadedFrame = m_observed->frame;
	m_configurationLoaded = true;
}
void DeviceControlPanel::ObserveState(const control::Message& state)
{
	try
	{
		const auto previous = m_observed ? m_observed->scenarios : ScenarioState{};
		m_observed = ReadObservedSetup(state);
		for (std::size_t index = 0; index < previous.size(); ++index)
			if (previous[index] && !m_observed->scenarios[index])
				ReloadRecoveredChannel(ScenarioInputChannel(static_cast<FailureScenario>(index)));
		m_observationError.clear();
		if (!m_configurationLoaded)
			LoadObservedSettings();
	}
	catch (const std::exception& error)
	{
		m_observed.reset();
		m_observationError = error.what();
		m_configurationLoaded = false;
	}
}
void DeviceControlPanel::ReloadRecoveredChannel(ControlChannel channel)
{
	// Failure observations include their effects. Once restored, refresh only
	// that device so an edit cannot latch the simulated failure into its values.
	const auto& values = m_observed->values;
	switch (channel)
	{
	case ControlChannel::Head:
		m_authoredTracking.head = values.tracking.head;
		break;
	case ControlChannel::LeftPose:
	case ControlChannel::RightPose:
	{
		const auto hand = channel == ControlChannel::LeftPose ? 0 : 1;
		m_authoredTracking.controllers[hand] = values.tracking.controllers[hand];
		m_authoredControls[hand].connected = values.input.controllers[hand].connected;
		break;
	}
	case ControlChannel::Body:
		m_authoredTracking.body = values.tracking.body;
		break;
	case ControlChannel::Gaze:
		m_authoredTracking.gaze = values.tracking.gaze;
		break;
	case ControlChannel::Trackers:
		m_authoredTracking.trackers = values.tracking.trackers;
		m_trackerRole = static_cast<int>(values.tracking.trackers[static_cast<std::size_t>(m_selectedTracker)].role);
		break;
	default:
		break;
	}
}
bool DeviceControlPanel::ScenarioActive(ControlChannel channel) const
{
	if (m_observed)
		for (std::size_t index = 0; index < m_observed->scenarios.size(); ++index)
			if (m_observed->scenarios[index] && ScenarioInputChannel(static_cast<FailureScenario>(index)) == channel)
				return true;
	return false;
}
void DeviceControlPanel::DrawTracking(DevicePage page)
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
													"Simulate the device disconnecting; restores when switched back"};
	const std::span<const char* const> labels = lost ? std::span<const char* const>(tracked) : connected;
	const std::span<const char* const> tips = lost ? std::span<const char* const>(trackedTips) : connectedTips;
	const int last = static_cast<int>(labels.size()) - 1;
	const int current = active(off) ? last : active(lost) ? 1 : 0;
	ImGui::PushID("Tracking");
	FieldLabel(lost ? "Tracking" : "Connection");
	const int choice = Segmented(labels, current, ImGui::GetContentRegionAvail().x, tips);
	ImGui::PopID();
	if (choice < 0 || choice == current || !m_observed)
		return;
	const auto channel = ScenarioInputChannel(*off);
	std::vector<SimulationCommand> commands{TakeControl{channel, m_replaceOwner}};
	if (lost)
		commands.emplace_back(SetScenario{*lost, choice == 1});
	commands.emplace_back(SetScenario{*off, choice == last});
	Apply(std::move(commands));
}
void DeviceControlPanel::DrawOwner(ControlChannel channel)
{
	if (!m_observed)
	{
		return;
	}
	// You are the implicit owner; only another driver gets a badge.
	const auto& owner = m_observed->owners[static_cast<std::size_t>(channel)];
	if (!owner || (owner->kind == CommandSourceKind::Manual && m_manualSource && *owner == *m_manualSource))
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
	ImGui::SetItemTooltip("Source %llu. Editing here takes over.", static_cast<unsigned long long>(owner->id));
}
void DeviceControlPanel::DrawHead()
{
	ImGui::BeginDisabled(ScenarioActive(ControlChannel::Head));
	ImGui::Spacing();
	if (ImGui::Button("Recenter view", {-1, 0}))
		ApplyOwned(ControlChannel::Session, SetScenario{FailureScenario::Recenter, true});
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Makes your current view the forward direction. Does not move devices.");
	DrawOwner(ControlChannel::Head);
	DrawExactPose(0);
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawExactPose(int device)
{
	const float scale = ImGui::GetFontSize() / 16;
	const auto flags = ImGui::GetContentRegionAvail().x >= 340 * scale ? ImGuiTreeNodeFlags_DefaultOpen : 0;
	if (!Section("Position & rotation", flags) || !m_playerRig.Ready())
		return;
	const auto channel = device == 0   ? ControlChannel::Head
						 : device == 1 ? ControlChannel::LeftPose
						 : device == 2 ? ControlChannel::RightPose
									   : ControlChannel::Trackers;
	const auto owner = m_observed ? m_observed->owners[static_cast<std::size_t>(channel)] : std::nullopt;
	ImGui::BeginDisabled(owner && owner != m_manualSource);
	// The numbers are relative to what the device follows (its anchor or joint), or to the world.
	XrPosef base = m_playerRig.Root();
	const char* follows = "Body";
	if (device == 1 || device == 2)
	{
		const auto anchor = m_playerRig.HandAnchor(static_cast<std::size_t>(device - 1));
		base = m_playerRig.AnchorPose(anchor);
		follows = anchor == Anchor::Head ? "Head" : anchor == Anchor::World ? "World" : "Body";
	}
	else if (device > 2)
	{
		const auto joint = m_playerRig.TrackerAt(static_cast<std::size_t>(device - 3)).joint;
		base = joint ? m_playerRig.JointPose(*joint) : IdentityPose();
		follows = joint ? BodyJointName(*joint) : "World";
	}
	if (!m_exactAnchor)
		base = IdentityPose();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	FieldLabel("Relative to");
	const std::string relative = std::string(m_exactAnchor ? follows : "World") + "##Relative";
	if (ImGui::SmallButton(relative.c_str()))
		m_exactAnchor = !m_exactAnchor;
	ImGui::SetItemTooltip("%s", m_exactAnchor ? "Click for world coordinates"
											  : "Click for coordinates relative to what the device follows");
	// Rotation as angles to edit, or as the raw quaternion to read and copy.
	static constexpr const char* forms[] = {"Angles", "Quaternion"};
	static constexpr const char* formTips[] = {"Yaw, pitch and roll in degrees",
											   "The orientation as x, y, z, w; read only"};
	bool& quaternion = m_quaternionView[static_cast<std::size_t>(device)];
	// Segments share the width equally, so size both for the longer label.
	const float formWidth = (ImGui::CalcTextSize("Quaternion").x + ImGui::GetStyle().FramePadding.x * 2) * 2;
	// Right-aligned beside the frame switch, or on its own row in a narrow inspector.
	ImGui::SameLine();
	if (ImGui::GetCursorPosX() + formWidth > right)
		ImGui::NewLine();
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - formWidth));
	ImGui::PushID("Rotation form");
	if (const int form = Segmented(forms, quaternion ? 1 : 0, formWidth, formTips); form >= 0)
		quaternion = form == 1;
	ImGui::PopID();
	const auto worldPose = [device](const PlayerRig& rig)
	{
		return device == 0	? rig.Head()
			   : device < 3 ? rig.Hand(static_cast<std::size_t>(device - 1))
							: rig.Tracker(static_cast<std::size_t>(device - 3));
	};
	auto pose = RelativePose(worldPose(m_playerRig), base);
	// Where Reset puts this device, in the same frame as the numbers, so each number resets alone.
	auto defaults = m_playerRig;
	if (device == 0)
		defaults.ResetHead();
	else if (device < 3)
		defaults.ResetHand(static_cast<std::size_t>(device - 1));
	else
		defaults.SnapTracker(static_cast<std::size_t>(device - 3));
	const auto fallback = RelativePose(worldPose(defaults), base);
	constexpr float degrees = 180 / std::numbers::pi_v<float>;
	const auto toDegrees = [](XrQuaternionf orientation)
	{
		const auto radians = YawPitchRoll(orientation);
		return std::array<float, 3>{radians.x * degrees, radians.y * degrees, radians.z * degrees};
	};
	auto angles = toDegrees(pose.orientation);
	const auto fallbackAngles = toDegrees(fallback.orientation);
	const float width = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
	// Resets only this device's pose; controls and connection stay.
	const auto reset = [this, device]
	{
		if (device == 0)
			m_playerRig.ResetHead();
		else if (device < 3)
			m_playerRig.ResetHand(static_cast<std::size_t>(device - 1));
		else
			m_playerRig.SnapTracker(static_cast<std::size_t>(device - 3));
		ApplyRig();
	};
	const auto& q = pose.orientation;
	char poseText[160], quaternionText[80];
	std::snprintf(poseText, sizeof(poseText), "{\"position\": [%.4f, %.4f, %.4f], \"orientation\": [%.5f, %.5f, %.5f, %.5f]}",
				  pose.position.x, pose.position.y, pose.position.z, q.x, q.y, q.z, q.w);
	std::snprintf(quaternionText, sizeof(quaternionText), "Quaternion x %.4f  y %.4f  z %.4f  w %.4f", q.x, q.y, q.z,
				  q.w);
	const auto value = [&](float& number, float fallbackNumber, float step, float limit, const char* format)
	{
		auto result = NumberValue(number, fallbackNumber, step, -limit, limit, format);
		result.group = "pose";
		result.resetGroup = reset;
		result.groupText = poseText;
		return result;
	};
	static constexpr const char* positionIds[] = {"##X", "##Y", "##Z"};
	static constexpr const char* positionFormats[] = {"X %.2f m", "Y %.2f m", "Z %.2f m"};
	float* positions[] = {&pose.position.x, &pose.position.y, &pose.position.z};
	const float fallbackPositions[] = {fallback.position.x, fallback.position.y, fallback.position.z};
	bool edited = false;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(width);
		edited |= Field(positionIds[axis],
						ImGui::DragFloat(positionIds[axis], positions[axis], .005f, 0, 0, positionFormats[axis]), true,
						value(*positions[axis], fallbackPositions[axis], .01f, 100, "%.4f"));
		ImGui::SetItemTooltip("X right, Y up, Z back.\nMiddle-click resets; Ctrl+wheel nudges.");
	}
	bool turned = false;
	if (quaternion)
	{
		float components[] = {q.x, q.y, q.z, q.w};
		static constexpr const char* componentIds[] = {"##Qx", "##Qy", "##Qz", "##Qw"};
		static constexpr const char* componentFormats[] = {"x %.4f", "y %.4f", "z %.4f", "w %.4f"};
		const float quarter = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
		for (int component = 0; component < 4; ++component)
		{
			if (component)
				ImGui::SameLine();
			ImGui::SetNextItemWidth(quarter);
			ImGui::InputScalar(componentIds[component], ImGuiDataType_Float, &components[component], nullptr, nullptr,
							   componentFormats[component], ImGuiInputTextFlags_ReadOnly);
			FieldValue copy;
			char text[24];
			std::snprintf(text, sizeof(text), "%.6f", components[component]);
			copy.text = text;
			copy.group = "pose";
			copy.resetGroup = reset;
			copy.groupText = poseText;
			FieldActions((m_fieldScope + componentIds[component]).c_str(), copy);
			ImGui::SetItemTooltip("Read only; Angles edits the rotation");
		}
	}
	else
	{
		static constexpr const char* angleIds[] = {"##Yaw", "##Pitch", "##Roll"};
		static constexpr const char* angleFormats[] = {"Yaw %.0f\u00b0", "Pitch %.0f\u00b0", "Roll %.0f\u00b0"};
		for (int axis = 0; axis < 3; ++axis)
		{
			if (axis)
				ImGui::SameLine();
			ImGui::SetNextItemWidth(width);
			const float limit = axis == 1 ? 89.9f : 180.0f;
			auto& angle = angles[static_cast<std::size_t>(axis)];
			turned |= Field(angleIds[axis],
							ImGui::DragFloat(angleIds[axis], &angle, .5f, -limit, limit, angleFormats[axis],
											 ImGuiSliderFlags_AlwaysClamp),
							true, value(angle, fallbackAngles[static_cast<std::size_t>(axis)], 1, limit, "%.1f"));
			ImGui::SetItemTooltip("%s\nMiddle-click resets; Ctrl+wheel nudges.", quaternionText);
		}
	}
	if (turned)
		pose.orientation = EulerAngles(angles[0] / degrees, angles[1] / degrees, angles[2] / degrees);
	if ((edited || turned) && !m_edit)
	{
		pose = Compose(base, pose);
		if (device == 0)
			m_playerRig.SetHeadWorld(pose);
		else if (device < 3)
			m_playerRig.SetHandWorld(static_cast<std::size_t>(device - 1), pose);
		else
			m_playerRig.SetTrackerWorld(static_cast<std::size_t>(device - 3), pose);
		Apply(PlayerEdit(true).commands);
	}
	ImGui::EndDisabled();
}
ManualTransaction DeviceControlPanel::PlayerEdit(bool includeWorld) const
{
	return m_observed ? m_playerRig.Transaction(*m_observed, m_manualSource, includeWorld) : ManualTransaction{};
}
void DeviceControlPanel::ApplyRig(std::vector<SimulationCommand> extra, std::string label)
{
	auto commands = PlayerEdit(true).commands;
	for (auto& command : extra)
		commands.push_back(std::move(command));
	m_continuousEdit = false;
	Apply(std::move(commands), std::move(label));
}
void DeviceControlPanel::DrawBodyPoses()
{
	static constexpr const char* tips[] = {"Arms down at the sides; hands and attached trackers follow",
										   "Arms straight out to the sides; hands and attached trackers follow",
										   "Arms 45° down; hands and attached trackers follow",
										   "Seated, hands on the thighs; hands and attached trackers follow"};
	const auto current = m_playerRig.CurrentBodyPose();
	ImGui::PushID("Body pose");
	const int pose = Segmented(BodyPoseNames, current ? static_cast<int>(*current) : -1,
							   ImGui::GetContentRegionAvail().x, tips);
	if (pose >= 0 && m_observed && m_playerRig.Ready())
	{
		m_playerRig.ApplyBodyPose(static_cast<BodyPose>(pose));
		ApplyRig({}, std::string("Body pose: ") + BodyPoseNames[pose]);
	}
	ImGui::PopID();
}
void DeviceControlPanel::DrawPlayer()
{
	DrawBodyPoses();
	FieldLabel("Walk speed");
	ImGui::SliderFloat("##Walk speed", &m_playerRig.walkSpeed, .1f, 5.0f, "%.1f m/s");
	// Walk speed only paces piloting; it is not sent to the app.
	FieldActions("Walk speed", NumberValue(m_playerRig.walkSpeed, 1, .1f, .1f, 5, "%.1f"));
	ImGui::SetItemTooltip("Piloting speed.\nMiddle-click for 1 m/s; Ctrl+wheel nudges.");
	const float defaultHeight = m_observed ? m_observed->values.proportions.height : 1.7f;
	FieldLabel("Height");
	bool changed = Field("##Player height",
						 ImGui::DragFloat("##Player height", &m_playerRig.height, .01f, .4f, 2.2f, "%.2f m"), true,
						 NumberValue(m_playerRig.height, defaultHeight, .01f, .4f, 2.2f));
	FieldLabel("Floor (m)");
	FieldValue floor;
	floor.reset = [&]
	{
		m_playerRig.position = {};
	};
	const auto& position = m_playerRig.position;
	floor.modified = position.x != 0 || position.y != 0 || position.z != 0;
	char floorText[64];
	std::snprintf(floorText, sizeof(floorText), "%.3f, %.3f, %.3f", position.x, position.y, position.z);
	floor.text = floorText;
	// One drag per axis, each named in its own box; the group is the one field.
	ImGui::BeginGroup();
	ImGui::PushID("Player position");
	const float gap = ImGui::GetStyle().ItemInnerSpacing.x, width = (ImGui::CalcItemWidth() - gap * 2) / 3;
	bool dragged = false;
	static constexpr const char* ids[] = {"##X", "##Y", "##Z"};
	static constexpr const char* formats[] = {"X  %.2f", "Y  %.2f", "Z  %.2f"};
	float* axes[] = {&m_playerRig.position.x, &m_playerRig.position.y, &m_playerRig.position.z};
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis)
			ImGui::SameLine(0, gap);
		ImGui::SetNextItemWidth(width);
		dragged |= ImGui::DragFloat(ids[axis], axes[axis], .01f, 0, 0, formats[axis]);
	}
	ImGui::PopID();
	ImGui::EndGroup();
	changed |= Field("##Player position", dragged, true, floor);
	if (changed && !m_edit)
	{
		auto edit = PlayerEdit();
		edit.continuous = m_continuousEdit;
		m_edit = std::move(edit);
	}
}
void DeviceControlPanel::DrawAnchor(std::size_t hand)
{
	int anchor = static_cast<int>(m_playerRig.HandAnchor(hand));
	static constexpr const char* names[] = {"Body", "Head", "World"};
	static constexpr const char* tips[] = {"Moves and turns with the player", "Follows the head",
										   "Stays put in the room"};
	ImGui::PushID("Anchor");
	FieldLabel("Follows");
	const int option = Segmented(names, anchor, ImGui::GetContentRegionAvail().x, tips);
	const bool changed = option >= 0 && option != anchor;
	if (changed)
		anchor = option;
	ImGui::PopID();
	if (changed)
		m_playerRig.SetHandAnchor(hand, static_cast<Anchor>(anchor));
}
void DeviceControlPanel::DrawHandPresets(std::size_t hand)
{
	static constexpr const char* names[] = {"Rest", "Forward", "Up"};
	static constexpr const char* tips[] = {"Low in front, holding the controller; this hand only",
										   "Reaching forward at eye level; this hand only",
										   "Pointing straight up; this hand only"};
	ImGui::PushID("Hand presets");
	const int preset = Segmented(names, -1, ImGui::GetContentRegionAvail().x, tips);
	if (preset >= 0 && m_observed && m_playerRig.Ready())
	{
		m_playerRig.Preset(hand, static_cast<HandPreset>(preset));
		ApplyRig({}, std::string(hand ? "Right hand: " : "Left hand: ") + names[preset]);
	}
	ImGui::PopID();
}
void DeviceControlPanel::DrawInputPad(std::uint32_t hand)
{
	auto& controller = m_authoredTracking.controllers[hand];
	auto& controls = m_authoredControls[hand];
	auto& latched = m_latches[hand];
	const auto previous = controls;
	const auto color = hand ? theme::Right : theme::Left;
	// Face button names follow the interaction profile.
	const std::string_view profile = m_controllerProfiles[hand].data();
	const bool touch = profile.find("oculus") != std::string_view::npos || profile.find("meta") != std::string_view::npos ||
					   profile.find("pico") != std::string_view::npos;
	const bool index = profile.find("valve/index") != std::string_view::npos;
	const char* primary = touch ? (hand ? "A##Primary" : "X##Primary") : index ? "A##Primary" : "1##Primary";
	const char* secondary = touch ? (hand ? "B##Secondary" : "Y##Secondary") : index ? "B##Secondary" : "2##Secondary";
	SectionLabel("Input");
	DrawOwner(hand ? ControlChannel::RightControls : ControlChannel::LeftControls);
	bool changed = false, continuous = false;
	bool stickClick = controls.thumbClick;
	const auto top = ImGui::GetCursorPosY();
	const bool stick = StickPad("Stick", controls.stick.x, controls.stick.y, stickClick, latched.stick, color);
	controls.thumbClick = stickClick;
	changed |= InputField("Stick", stick);
	continuous |= stick && ImGui::IsItemActive();
	const float stickHeight = ImGui::GetItemRectSize().y;
	ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
	// Trigger, grip and the face buttons, centred beside the stick.
	const float unit = ImGui::GetFontSize() / 17, spacing = ImGui::GetStyle().ItemSpacing.y;
	ImGui::SetCursorPosY(top + std::max(0.0f, (stickHeight - (24 * 2 + 34) * unit - spacing * 2) / 2));
	ImGui::BeginGroup();
	const float width = std::max(60.f, ImGui::GetContentRegionAvail().x);
	const bool trigger = TriggerBar("Trigger", "Trigger", controls.trigger, latched.trigger, color, width);
	changed |= InputField("Trigger", trigger);
	continuous |= trigger && ImGui::IsItemActive();
	const bool grip = TriggerBar("Grip", "Grip", controls.grip, latched.grip, color, width);
	changed |= InputField("Grip", grip);
	continuous |= grip && ImGui::IsItemActive();
	changed |= InputField("Primary", HoldButton("Primary", primary, controls.primary, latched.primary, color));
	ImGui::SameLine();
	changed |= InputField("Secondary", HoldButton("Secondary", secondary, controls.secondary, latched.secondary, color));
	ImGui::SameLine();
	changed |= InputField("Menu", HoldButton("Menu", "M##Menu", controls.menu, latched.menu, color));
	// Release-all sits at the row's right end, centred on the round buttons.
	const float buttonRow = ImGui::GetItemRectSize().y;
	ImGui::SameLine(width - ImGui::GetFrameHeight());
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (buttonRow - ImGui::GetFrameHeight()) / 2);
	const bool clear = IconButton("Clear hand", Icon::Close, "Release every input of this hand");
	ImGui::EndGroup();
	ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + stickHeight + spacing));
	if (clear)
	{
		controls = {};
		latched = {};
	}
	if (!clear && !changed)
		return;
	controls.connected = controller.connected;
	ApplyOwned(hand ? ControlChannel::RightControls : ControlChannel::LeftControls, SetControllerControls{hand, controls});
	auto& edit = std::get<ManualTransaction>(*m_edit);
	edit.continuous = continuous;
	edit.releaseEdge = clear || (previous.primary && !controls.primary) ||
					   (previous.secondary && !controls.secondary) || (previous.menu && !controls.menu) ||
					   (previous.thumbClick && !controls.thumbClick);
}
void DeviceControlPanel::DrawController(const std::function<void()>& placement)
{
	const auto hand = static_cast<std::uint32_t>(m_controlHand);
	DrawAnchor(hand);
	ImGui::BeginDisabled(ScenarioActive(hand ? ControlChannel::RightPose : ControlChannel::LeftPose));
	auto& controller = m_authoredTracking.controllers[hand];
	DrawInputPad(hand);
	if (placement)
		placement();
	SectionLabel("Pose");
	DrawHandPresets(hand);
	DrawExactPose(static_cast<int>(hand) + 1);
	if (Section("Connection and profile"))
	{
		if (Field("Connected", ImGui::Checkbox("Connected", &controller.connected), false,
				  SwitchValue(controller.connected, true)))
			ApplyOwned(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
					   SetControllerPose{hand, controller});
		ImGui::Text("Interaction profile path");
		auto& profile = m_controllerProfiles[hand];
		Field("##Interaction profile", ImGui::InputText("##Interaction profile", profile.data(), profile.size()),
			  false);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", profile.data());
		if (ImGui::IsItemDeactivatedAfterEdit())
			ApplyOwned(ControlChannel::Profiles, SetControllerProfile{hand, profile.data()});
	}
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawBody()
{
	SectionLabel("Body");
	DrawOwner(ControlChannel::Body);
	if (m_observed && !m_observed->bodyOverridesAvailable)
		ImGui::TextUnformatted("Body unavailable");
	const bool scenario = ScenarioActive(ControlChannel::Body);
	ImGui::BeginDisabled(scenario || !m_observed || !m_observed->bodyOverridesAvailable);
	DrawBodyPoses();
	auto& body = m_authoredTracking.body;
	bool edited = Field("Body active", ImGui::Checkbox("Body active", &body.active), false,
						SwitchValue(body.active, true));
	FieldLabel("Confidence");
	edited |= Field("##Confidence", ImGui::SliderFloat("##Confidence", &body.confidence, 0, 1), true,
					NumberValue(body.confidence, 1, .05f, 0, 1));
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Body, SetBodyControls{body});
	ImGui::EndDisabled();
	SectionLabel("Body proportions");
	DrawOwner(ControlChannel::Proportions);
	const BodyProportions defaults;
	FieldLabel("Height");
	bool proportionsEdited =
		Field("##Height", ImGui::SliderFloat("##Height", &m_authoredProportions.height, 0.5f, 2.5f, "%.2f m"), true,
			  NumberValue(m_authoredProportions.height, defaults.height, .01f, .5f, 2.5f));
	FieldLabel("Shoulder width");
	proportionsEdited |= Field(
		"##Shoulder width",
		ImGui::SliderFloat("##Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f, "%.2f m"), true,
		NumberValue(m_authoredProportions.shoulderWidth, defaults.shoulderWidth, .01f, .1f, .8f));
	FieldLabel("Hip width");
	proportionsEdited |= Field(
		"##Hip width", ImGui::SliderFloat("##Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f, "%.2f m"), true,
		NumberValue(m_authoredProportions.hipWidth, defaults.hipWidth, .01f, .1f, .8f));
	if (proportionsEdited && !m_edit)
		ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
}
void DeviceControlPanel::DrawGaze()
{
	SectionLabel("Eye gaze");
	DrawOwner(ControlChannel::Gaze);
	ImGui::BeginDisabled(ScenarioActive(ControlChannel::Gaze));
	auto& gaze = m_authoredTracking.gaze;
	const GazeInput defaults;
	FieldLabel("Yaw");
	bool edited = Field("##Yaw", ImGui::SliderAngle("##Yaw", &gaze.yaw, -90, 90), true,
						AngleValue(gaze.yaw, defaults.yaw, -90, 90));
	FieldLabel("Pitch");
	edited |= Field("##Pitch", ImGui::SliderAngle("##Pitch", &gaze.pitch, -90, 90), true,
					AngleValue(gaze.pitch, defaults.pitch, -90, 90));
	edited |= Field("Gaze available", ImGui::Checkbox("Gaze available", &gaze.available), false,
					SwitchValue(gaze.available, defaults.available));
	edited |= Field("Gaze tracked", ImGui::Checkbox("Gaze tracked", &gaze.tracked), false,
					SwitchValue(gaze.tracked, defaults.tracked));
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawTrackers()
{
	if (m_observed && !m_observed->trackerAttachmentsAvailable)
		ImGui::TextUnformatted("Trackers unavailable");
	const bool scenario = ScenarioActive(ControlChannel::Trackers);
	ImGui::BeginDisabled(scenario || !m_observed || !m_observed->trackerAttachmentsAvailable);
	DrawOwner(ControlChannel::Trackers);
	// One chip per tracker, named by its role.
	ImGui::PushID("Tracker chips");
	bool selected = false;
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	for (int slot = 0; slot < static_cast<int>(m_authoredTracking.trackers.size()); ++slot)
	{
		const auto& device = m_authoredTracking.trackers[static_cast<std::size_t>(slot)];
		const char* name = TrackerRoleName(device.role);
		const float chip = ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2;
		if (slot && ImGui::GetItemRectMax().x + 2 + chip <= right)
			ImGui::SameLine(0, 2);
		const bool current = slot == m_selectedTracker;
		ImGui::PushStyleColor(ImGuiCol_Button, current ? theme::Accent : theme::SurfaceRaised);
		ImGui::PushStyleColor(ImGuiCol_Text, device.connected ? theme::Text : theme::TextDim);
		ImGui::PushID(slot);
		if (ImGui::Button(name) && !current)
		{
			m_selectedTracker = slot;
			selected = true;
		}
		ImGui::PopID();
		ImGui::PopStyleColor(2);
		ImGui::SetItemTooltip("Tracker %d%s", slot + 1, device.connected ? "" : " (disconnected)");
	}
	ImGui::PopID();
	const auto slot = static_cast<std::size_t>(m_selectedTracker);
	auto& tracker = m_authoredTracking.trackers[slot];
	if (selected)
		m_trackerRole = static_cast<int>(tracker.role);
	const auto attachment = m_playerRig.TrackerAt(slot);
	FieldLabel("Attached to");
	const float snapWidth = ImGui::CalcTextSize("Snap to body").x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::SetNextItemWidth(
		std::max(80.0f, ImGui::GetContentRegionAvail().x - snapWidth - ImGui::GetStyle().ItemSpacing.x));
	if (ImGui::BeginCombo("##Attached to", attachment.joint ? BodyJointName(*attachment.joint) : "World"))
	{
		for (const auto joint : AttachableJoints())
			if (ImGui::Selectable(BodyJointName(joint), attachment.joint == joint) && m_playerRig.Ready())
			{
				m_playerRig.AttachTracker(slot, joint);
				ApplyRig();
			}
		if (ImGui::Selectable("World", !attachment.joint) && m_playerRig.Ready())
		{
			m_playerRig.AttachTracker(slot, std::nullopt);
			ApplyRig();
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("Joint offsets follow body animation. World attachments stay fixed.");
	ImGui::SameLine();
	if (ImGui::Button("Snap to body") && m_playerRig.Ready())
	{
		m_playerRig.SnapTracker(slot);
		ApplyRig();
	}
	ImGui::SetItemTooltip("Back on the %s joint, no offset", BodyJointName(BodyModel::TrackerJoint(tracker.role)));
	DrawExactPose(m_selectedTracker + 3);
	static constexpr const char* roles[] = {
		"Waist",		 "Chest",		   "Left foot",	 "Right foot",	"Unassigned", "Handheld object",
		"Left shoulder", "Right shoulder", "Left elbow", "Right elbow", "Left knee",  "Right knee",
		"Left wrist",	 "Right wrist",	   "Left ankle", "Right ankle", "Camera",	  "Keyboard"};
	bool edited = Field("Tracker connected", ImGui::Checkbox("Connected", &tracker.connected), false,
						SwitchValue(tracker.connected, true));
	ImGui::SameLine();
	edited |= Field("Tracker tracked", ImGui::Checkbox("Tracked", &tracker.tracked), false,
					SwitchValue(tracker.tracked, true));
	ImGui::SetItemTooltip("Identity %llu", static_cast<unsigned long long>(tracker.identity.value));
	if (Section("Role"))
	{
		const auto previousRole = tracker.role;
		const bool roleChanged =
			Field("##Role", ImGui::Combo("##Role", &m_trackerRole, roles, IM_ARRAYSIZE(roles)), false);
		const auto path = BodyModel::TrackerRolePath(static_cast<TrackerRole>(m_trackerRole));
		ImGui::SetItemTooltip("The app sees this tracker at %s",
							  path.empty() ? "no role path" : std::string(path).c_str());
		tracker.role = static_cast<TrackerRole>(m_trackerRole);
		if (roleChanged && attachment.joint == BodyModel::TrackerJoint(previousRole))
		{
			// A tracker on its old role's joint moves with the role.
			m_playerRig.AttachTracker(slot, BodyModel::TrackerJoint(tracker.role));
			m_playerRig.SetTrackerOffset(slot, IdentityPose());
			tracker.anchor = BodyModel::TrackerJoint(tracker.role);
			tracker.poseOverride.reset();
			tracker.jointOffset.reset();
		}
		edited |= roleChanged;
	}
	bool duplicateRole = false;
	for (std::size_t index = 0; index < m_authoredTracking.trackers.size(); ++index)
	{
		const auto& other = m_authoredTracking.trackers[index];
		if (index != slot && tracker.connected && other.connected && tracker.role != TrackerRole::Unassigned &&
			other.role == tracker.role)
		{
			ImGui::TextColored(theme::Vec(theme::Warn), "%s is also tracker %u", TrackerRoleName(other.role),
							   static_cast<unsigned>(index + 1));
			duplicateRole = true;
		}
	}
	if (edited && !duplicateRole && !m_edit)
		ApplyOwned(ControlChannel::Trackers, SetTracker{static_cast<std::uint32_t>(slot), tracker});
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawViews()
{
	SectionLabel("Display");
	DrawOwner(ControlChannel::Views);
	FieldLabel("IPD");
	const ViewSettings defaults;
	bool edited = Field("##IPD", ImGui::SliderFloat("##IPD", &m_authoredViews.ipd, 0.04f, 0.09f, "%.3f m"), true,
						NumberValue(m_authoredViews.ipd, defaults.ipd, .001f, .04f, .09f, "%.3f"));
	FieldLabel("Inset angle");
	float maximum = std::numbers::pi_v<float> / 2;
	for (const auto& fov : m_authoredViews.outerFov)
		maximum = std::min({maximum, (fov.angleRight - fov.angleLeft) / 2, (fov.angleUp - fov.angleDown) / 2});
	const float maximumDegrees = maximum * 180 / std::numbers::pi_v<float>;
	edited |= Field("##Inset half angle",
					ImGui::SliderAngle("##Inset half angle", &m_authoredViews.insetHalfAngle, 0.1f, maximumDegrees),
					true,
					AngleValue(m_authoredViews.insetHalfAngle, std::min(defaults.insetHalfAngle, maximum), .1f,
							   maximumDegrees));
	if (edited)
		ApplyOwned(ControlChannel::Views, SetViewSettings{m_authoredViews});
	ImGui::Text("View size: %u x %u; inset: %u x %u", m_authoredViews.outerWidth, m_authoredViews.outerHeight,
				m_authoredViews.insetWidth, m_authoredViews.insetHeight);
}
} // namespace simxr::app
