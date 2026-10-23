#include "app/DeviceControlPanel.h"
#include "app/Theme.h"
#include "Math.h"
#include "app/SpatialMath.h"
#include "app/widgets/Controls.h"
#include "app/widgets/InputPad.h"
#include "app/widgets/LiveField.h"
#include "simulation/BodyModel.h"
#include <algorithm>
#include <cmath>
#include <numbers>
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
			switch (device->page)
			{
			case DevicePage::Player:
				DrawPlayer();
				break;
			case DevicePage::Head:
				DrawHead();
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
		if (const auto session = std::get_if<SessionItemSel>(selection))
		{
			m_fieldScope = "session/" + std::to_string(static_cast<int>(session->item)) + "/";
			if (session->item == SessionItem::Headset)
				DrawViews();
			else
				DrawScenarios();
		}
		ImGui::EndDisabled();
	}
	else if (ImGui::BeginTabBar("Devices"))
	{
		if (ImGui::BeginTabItem("Head"))
		{
			ImGui::BeginDisabled(!supported);
			ImGui::BeginChild("Head details");
			ImGui::PushItemWidth(-1);
			DrawHead();
			ImGui::PopItemWidth();
			ImGui::EndChild();
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}
		for (int hand = 0; hand < 2; ++hand)
		{
			if (ImGui::BeginTabItem(hand ? "Right hand" : "Left hand"))
			{
				m_controlHand = hand;
				ImGui::PushID(hand);
				ImGui::BeginDisabled(!supported);
				ImGui::BeginChild("Hand details");
				ImGui::PushItemWidth(-1);
				DrawController({});
				ImGui::PopItemWidth();
				ImGui::EndChild();
				ImGui::EndDisabled();
				ImGui::PopID();
				ImGui::EndTabItem();
			}
		}
		if (ImGui::BeginTabItem("Setup"))
		{
			ImGui::TextUnformatted("Configure");
			ImGui::Combo("##Setup section", &m_setupSection, "Headset & views\0Body\0Eye gaze\0Trackers\0Scenarios\0");
			ImGui::BeginDisabled(!supported);
			ImGui::BeginChild("Configuration details");
			ImGui::PushItemWidth(-1);
			switch (m_setupSection)
			{
			case 0:
				DrawViews();
				break;
			case 1:
				DrawBody();
				break;
			case 2:
				DrawGaze();
				break;
			case 3:
				DrawTrackers();
				break;
			case 4:
				DrawScenarios();
				break;
			}
			ImGui::PopItemWidth();
			ImGui::EndChild();
			ImGui::EndDisabled();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::PopItemWidth();
	return std::exchange(m_edit, {});
}
bool DeviceControlPanel::Field(const char* key, bool changed, bool continuous, const std::function<void()>& reset,
							   bool slider)
{
	const std::string field = m_fieldScope + key;
	const bool finished = continuous && ImGui::IsItemDeactivatedAfterEdit();
	changed |= finished;
	bool restored = false;
	if (reset)
	{
		restored = slider && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
		if (ImGui::BeginPopupContextItem(field.c_str()))
		{
			restored |= ImGui::MenuItem("Reset to default");
			ImGui::EndPopup();
		}
		if (restored)
		{
			reset();
			changed = true;
		}
	}
	if (changed)
	{
		m_pendingFields.insert(field);
		m_continuousEdit = continuous && !finished && !restored;
		m_rejectedFields.erase(field);
	}
	DrawLiveField(m_pending && m_pendingFields.contains(field),
				  m_rejectedFields.contains(field) && ImGui::GetTime() < m_rejectedUntil ? m_deliveryError
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
	const auto page = device->page;
	const bool ready = m_observed && m_configurationLoaded && m_playerRig.Ready();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(titles[static_cast<int>(page)]);
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
		if (IconButton("Reset page", Icon::Refresh, tip.c_str()))
			ResetPage(page);
		ImGui::EndDisabled();
	}
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
void DeviceControlPanel::Apply(std::vector<SimulationCommand> commands)
{
	ManualTransaction transaction;
	transaction.continuous = m_continuousEdit;
	m_pending = true;
	if (m_applyFrame)
		transaction.frame = FrameId{m_applyFrame};
	transaction.latePolicy = m_reschedule ? CommandLatePolicy::RescheduleNext : CommandLatePolicy::Reject;
	transaction.commands = std::move(commands);
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
bool DeviceControlPanel::DrawScenarioNotice(ControlChannel channel)
{
	if (m_observed)
		for (std::size_t index = 0; index < m_observed->scenarios.size(); ++index)
			if (m_observed->scenarios[index] && ScenarioInputChannel(static_cast<FailureScenario>(index)) == channel)
			{
				ImGui::TextUnformatted("Scenario active");
				return true;
			}
	return false;
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
	ImGui::BeginDisabled(DrawScenarioNotice(ControlChannel::Head));
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
	if (!ImGui::CollapsingHeader("Position & rotation", flags) || !m_playerRig.Ready())
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
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Relative to");
	ImGui::SameLine();
	const std::string relative = std::string(m_exactAnchor ? follows : "World") + "##Relative";
	if (ImGui::SmallButton(relative.c_str()))
		m_exactAnchor = !m_exactAnchor;
	ImGui::SetItemTooltip("%s", m_exactAnchor ? "Click for world coordinates"
											  : "Click for coordinates relative to what the device follows");
	const auto world = device == 0	? m_playerRig.Head()
					   : device < 3 ? m_playerRig.Hand(static_cast<std::size_t>(device - 1))
									: m_playerRig.Tracker(static_cast<std::size_t>(device - 3));
	auto pose = RelativePose(world, base);
	constexpr float degrees = 180 / std::numbers::pi_v<float>;
	const auto radiansAngles = YawPitchRoll(pose.orientation);
	std::array<float, 3> angles{radiansAngles.x * degrees, radiansAngles.y * degrees, radiansAngles.z * degrees};
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
	static constexpr const char* positionIds[] = {"##X", "##Y", "##Z"};
	static constexpr const char* positionFormats[] = {"X %.2f m", "Y %.2f m", "Z %.2f m"};
	float* positions[] = {&pose.position.x, &pose.position.y, &pose.position.z};
	bool edited = false;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(width);
		edited |= Field(positionIds[axis],
						ImGui::DragFloat(positionIds[axis], positions[axis], .005f, 0, 0, positionFormats[axis]), true,
						reset);
		ImGui::SetItemTooltip("X right, Y up, Z back. Right-click to reset.");
	}
	static constexpr const char* angleIds[] = {"##Yaw", "##Pitch", "##Roll"};
	static constexpr const char* angleFormats[] = {"Yaw %.0f°", "Pitch %.0f°", "Roll %.0f°"};
	bool turned = false;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(width);
		const float limit = axis == 1 ? 89.9f : 180.0f;
		turned |= Field(angleIds[axis],
						ImGui::DragFloat(angleIds[axis], &angles[static_cast<std::size_t>(axis)], .5f, -limit, limit,
										 angleFormats[axis], ImGuiSliderFlags_AlwaysClamp),
						true, reset);
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
void DeviceControlPanel::ApplyRig(std::vector<SimulationCommand> extra)
{
	auto commands = PlayerEdit(true).commands;
	for (auto& command : extra)
		commands.push_back(std::move(command));
	m_continuousEdit = false;
	Apply(std::move(commands));
}
void DeviceControlPanel::DrawBodyPoses()
{
	static constexpr const char* tips[] = {"Arms down at the sides", "Arms straight out to the sides",
										   "Arms 45° down", "Seated, hands on the thighs"};
	const auto current = m_playerRig.CurrentBodyPose();
	ImGui::PushID("Body pose");
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Pose");
	for (std::size_t pose = 0; pose < BodyPoseNames.size(); ++pose)
	{
		ImGui::SameLine(0, pose ? 2.0f : -1.0f);
		const bool selected = current && static_cast<std::size_t>(*current) == pose;
		if (selected)
			ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent);
		const bool clicked = ImGui::Button(BodyPoseNames[pose]);
		if (selected)
			ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s. Hands and attached trackers follow.", tips[pose]);
		if (clicked && m_observed && m_playerRig.Ready())
		{
			m_playerRig.ApplyBodyPose(static_cast<BodyPose>(pose));
			ApplyRig();
		}
	}
	ImGui::PopID();
}
void DeviceControlPanel::DrawPlayer()
{
	DrawBodyPoses();
	ImGui::TextUnformatted("Walk speed");
	ImGui::SliderFloat("##Walk speed", &m_playerRig.walkSpeed, .1f, 5.0f, "%.1f m/s");
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		m_playerRig.walkSpeed = 1;
	ImGui::SetItemTooltip("Piloting speed. Double-click for 1 m/s.");
	const float defaultHeight = m_observed ? m_observed->values.proportions.height : 1.7f;
	ImGui::TextUnformatted("Height");
	bool changed = Field("##Player height",
						 ImGui::DragFloat("##Player height", &m_playerRig.height, .01f, .4f, 2.2f, "%.2f m"), true,
						 [&]
						 {
							 m_playerRig.height = defaultHeight;
						 });
	ImGui::TextUnformatted("Floor position");
	changed |= Field("##Player position",
					 ImGui::DragFloat3("##Player position", &m_playerRig.position.x, .01f, 0, 0, "%.2f m"), true,
					 [&]
					 {
						 m_playerRig.position = {};
					 });
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
	bool changed = false;
	ImGui::PushID("Anchor");
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Follows");
	for (int option = 0; option < 3; ++option)
	{
		ImGui::SameLine(0, option ? 2.0f : -1.0f);
		const bool selected = option == anchor;
		if (selected)
			ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent);
		if (ImGui::Button(names[option]) && !selected)
		{
			anchor = option;
			changed = true;
		}
		if (selected)
			ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", tips[option]);
	}
	ImGui::PopID();
	if (changed)
		m_playerRig.SetHandAnchor(hand, static_cast<Anchor>(anchor));
}
void DeviceControlPanel::DrawHandPresets(std::size_t hand)
{
	static constexpr const char* names[] = {"Rest", "Forward", "Up"};
	static constexpr const char* tips[] = {"Low in front, holding the controller", "Reaching forward at eye level",
										   "Pointing straight up"};
	ImGui::PushID("Hand presets");
	for (int index = 0; index < 3; ++index)
	{
		if (index)
			ImGui::SameLine();
		if (ImGui::Button(names[index]) && m_observed && m_playerRig.Ready())
		{
			m_playerRig.Preset(hand, static_cast<HandPreset>(index));
			ApplyRig();
		}
		ImGui::SetItemTooltip("%s. This hand only.", tips[index]);
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
	ImGui::SeparatorText("Input");
	DrawOwner(hand ? ControlChannel::RightControls : ControlChannel::LeftControls);
	bool changed = false, continuous = false;
	bool stickClick = controls.thumbClick;
	const bool stick = StickPad("Stick", controls.stick.x, controls.stick.y, stickClick, latched.stick, color);
	controls.thumbClick = stickClick;
	changed |= Field("Stick", stick, true);
	continuous |= stick && ImGui::IsItemActive();
	ImGui::SameLine();
	ImGui::BeginGroup();
	const float width = std::max(60.f, ImGui::GetContentRegionAvail().x);
	const bool trigger = TriggerBar("Trigger", "Trigger", controls.trigger, latched.trigger, color, width);
	changed |= Field("Trigger", trigger, true);
	continuous |= trigger && ImGui::IsItemActive();
	const bool grip = TriggerBar("Grip", "Grip", controls.grip, latched.grip, color, width);
	changed |= Field("Grip", grip, true);
	continuous |= grip && ImGui::IsItemActive();
	changed |= Field("Primary", HoldButton("Primary", primary, controls.primary, latched.primary, color));
	ImGui::SameLine();
	changed |= Field("Secondary", HoldButton("Secondary", secondary, controls.secondary, latched.secondary, color));
	ImGui::SameLine();
	changed |= Field("Menu", HoldButton("Menu", "M##Menu", controls.menu, latched.menu, color));
	ImGui::SameLine();
	const bool clear = IconButton("Clear hand", Icon::Close, "Release every input of this hand");
	ImGui::EndGroup();
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
	ImGui::BeginDisabled(DrawScenarioNotice(hand ? ControlChannel::RightPose : ControlChannel::LeftPose));
	auto& controller = m_authoredTracking.controllers[hand];
	DrawInputPad(hand);
	if (placement)
		placement();
	ImGui::SeparatorText("Pose");
	DrawHandPresets(hand);
	DrawExactPose(static_cast<int>(hand) + 1);
	if (ImGui::CollapsingHeader("Connection and profile"))
	{
		if (Field("Connected", ImGui::Checkbox("Connected", &controller.connected), false,
				  [&]
				  {
					  controller.connected = true;
				  }))
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
	ImGui::SeparatorText("Body");
	DrawOwner(ControlChannel::Body);
	if (m_observed && !m_observed->bodyOverridesAvailable)
		ImGui::TextUnformatted("Body unavailable");
	const bool scenario = DrawScenarioNotice(ControlChannel::Body);
	ImGui::BeginDisabled(scenario || !m_observed || !m_observed->bodyOverridesAvailable);
	DrawBodyPoses();
	auto& body = m_authoredTracking.body;
	bool edited = Field("Body active", ImGui::Checkbox("Body active", &body.active), false,
						[&]
						{
							body.active = true;
						});
	ImGui::Text("Confidence");
	edited |= Field(
		"##Confidence", ImGui::SliderFloat("##Confidence", &body.confidence, 0, 1), true,
		[&]
		{
			body.confidence = 1;
		},
		true);
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Body, SetBodyControls{body});
	ImGui::EndDisabled();
	ImGui::SeparatorText("Body proportions");
	DrawOwner(ControlChannel::Proportions);
	const BodyProportions defaults;
	ImGui::Text("Height");
	bool proportionsEdited = Field(
		"##Height", ImGui::SliderFloat("##Height", &m_authoredProportions.height, 0.5f, 2.5f, "%.2f m"), true,
		[&]
		{
			m_authoredProportions.height = defaults.height;
		},
		true);
	ImGui::Text("Shoulder width");
	proportionsEdited |= Field(
		"##Shoulder width",
		ImGui::SliderFloat("##Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f, "%.2f m"), true,
		[&]
		{
			m_authoredProportions.shoulderWidth = defaults.shoulderWidth;
		},
		true);
	ImGui::Text("Hip width");
	proportionsEdited |= Field(
		"##Hip width", ImGui::SliderFloat("##Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f, "%.2f m"), true,
		[&]
		{
			m_authoredProportions.hipWidth = defaults.hipWidth;
		},
		true);
	if (proportionsEdited && !m_edit)
		ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
}
void DeviceControlPanel::DrawGaze()
{
	ImGui::SeparatorText("Eye gaze");
	DrawOwner(ControlChannel::Gaze);
	ImGui::BeginDisabled(DrawScenarioNotice(ControlChannel::Gaze));
	auto& gaze = m_authoredTracking.gaze;
	const GazeInput defaults;
	ImGui::Text("Yaw");
	bool edited = Field(
		"##Yaw", ImGui::SliderAngle("##Yaw", &gaze.yaw, -90, 90), true,
		[&]
		{
			gaze.yaw = defaults.yaw;
		},
		true);
	ImGui::Text("Pitch");
	edited |= Field(
		"##Pitch", ImGui::SliderAngle("##Pitch", &gaze.pitch, -90, 90), true,
		[&]
		{
			gaze.pitch = defaults.pitch;
		},
		true);
	edited |= Field("Gaze available", ImGui::Checkbox("Gaze available", &gaze.available), false,
					[&]
					{
						gaze.available = defaults.available;
					});
	edited |= Field("Gaze tracked", ImGui::Checkbox("Gaze tracked", &gaze.tracked), false,
					[&]
					{
						gaze.tracked = defaults.tracked;
					});
	if (edited && !m_edit)
		ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawTrackers()
{
	if (m_observed && !m_observed->trackerAttachmentsAvailable)
		ImGui::TextUnformatted("Trackers unavailable");
	const bool scenario = DrawScenarioNotice(ControlChannel::Trackers);
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
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("Attached to");
	ImGui::SameLine();
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
						[&]
						{
							tracker.connected = true;
						});
	ImGui::SameLine();
	edited |= Field("Tracker tracked", ImGui::Checkbox("Tracked", &tracker.tracked), false,
					[&]
					{
						tracker.tracked = true;
					});
	ImGui::SetItemTooltip("Identity %llu", static_cast<unsigned long long>(tracker.identity.value));
	if (ImGui::CollapsingHeader("Role"))
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
	ImGui::SeparatorText("View settings");
	DrawOwner(ControlChannel::Views);
	ImGui::Text("Eye separation (IPD)");
	const ViewSettings defaults;
	bool edited = Field(
		"##IPD", ImGui::SliderFloat("##IPD", &m_authoredViews.ipd, 0.04f, 0.09f, "%.3f m"), true,
		[&]
		{
			m_authoredViews.ipd = defaults.ipd;
		},
		true);
	ImGui::Text("Inset angle");
	float maximum = std::numbers::pi_v<float> / 2;
	for (const auto& fov : m_authoredViews.outerFov)
		maximum = std::min({maximum, (fov.angleRight - fov.angleLeft) / 2, (fov.angleUp - fov.angleDown) / 2});
	edited |= Field("##Inset half angle",
					ImGui::SliderAngle("##Inset half angle", &m_authoredViews.insetHalfAngle, 0.1f,
									   maximum * 180 / std::numbers::pi_v<float>),
					true,
					[&]
					{
						m_authoredViews.insetHalfAngle = std::min(defaults.insetHalfAngle, maximum);
					},
					true);
	if (edited)
		ApplyOwned(ControlChannel::Views, SetViewSettings{m_authoredViews});
	ImGui::Text("View size: %u x %u; inset: %u x %u", m_authoredViews.outerWidth, m_authoredViews.outerHeight,
				m_authoredViews.insetWidth, m_authoredViews.insetHeight);
}
void DeviceControlPanel::DrawScenarios()
{
	ImGui::SeparatorText("Failure scenarios");
	static constexpr const char* scenarios[] = {"Head untracked",
												"Head unavailable",
												"Body untracked",
												"Body unavailable",
												"Gaze untracked",
												"Gaze unavailable",
												"Left controller disconnected",
												"Right controller disconnected",
												"Trackers disconnected",
												"Unfocused",
												"Hidden",
												"Recenter",
												"Session loss"};
	ImGui::Text("Scenario");
	ImGui::Combo("##Scenario", &m_selectedScenario, scenarios, IM_ARRAYSIZE(scenarios));
	const auto scenario = static_cast<FailureScenario>(m_selectedScenario);
	const auto channel = ScenarioInputChannel(scenario);
	DrawOwner(channel);
	if (scenario == FailureScenario::Recenter)
	{
		ImGui::TextUnformatted("Recenter origin");
		if (ImGui::Button("Recenter now", {-1, 0}))
			ApplyOwned(channel, SetScenario{scenario, true});
	}
	else if (scenario == FailureScenario::SessionLoss)
	{

		if (ImGui::Button("Request session loss", {-1, 0}))
			ImGui::OpenPopup("Request session loss?");
		if (ImGui::BeginPopupModal("Request session loss?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("The app must recreate its XR session or be relaunched.");
			if (ImGui::Button("Request loss"))
			{
				ApplyOwned(channel, SetScenario{scenario, true});
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	else
	{
		const bool active = m_observed && m_observed->scenarios[static_cast<std::size_t>(m_selectedScenario)];
		ImGui::TextUnformatted(active ? "Applied state: active" : "Applied state: inactive");
		if (ImGui::Button(active ? "Restore normal behavior" : "Activate scenario", {-1, 0}))
			ApplyOwned(channel, SetScenario{scenario, !active});
	}
}
} // namespace simxr::app
