#include "app/DeviceControlPanel.h"
#include "app/Theme.h"
#include "Math.h"
#include "app/widgets/Controls.h"
#include "app/widgets/InputPad.h"
#include "app/widgets/LiveField.h"
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
bool DeviceControlPanel::Field(const char* key, bool changed, bool continuous)
{
	const std::string field = m_fieldScope + key;
	const bool finished = continuous && ImGui::IsItemDeactivatedAfterEdit();
	changed |= finished;
	if (changed)
	{
		m_pendingFields.insert(field);
		m_continuousEdit = continuous && !finished;
		m_rejectedFields.erase(field);
	}
	DrawLiveField(m_pending && m_pendingFields.contains(field),
				  m_rejectedFields.contains(field) && ImGui::GetTime() < m_rejectedUntil ? m_deliveryError
																						 : std::string{});
	return changed;
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
	m_bodyPreset = static_cast<int>(values.tracking.body.preset);
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
		m_bodyPreset = static_cast<int>(values.tracking.body.preset);
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
	DrawHandRest();
	ImGui::SeparatorText("Head pose");
	DrawOwner(ControlChannel::Head);
	DrawExactPose(0);
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawExactPose(int device)
{
	if (!ImGui::CollapsingHeader("Exact pose") || !m_playerRig.Ready())
		return;
	const auto channel = device == 0   ? ControlChannel::Head
						 : device == 1 ? ControlChannel::LeftPose
						 : device == 2 ? ControlChannel::RightPose
									   : ControlChannel::Trackers;
	const auto owner = m_observed ? m_observed->owners[static_cast<std::size_t>(channel)] : std::nullopt;
	ImGui::BeginDisabled(owner && owner != m_manualSource);
	if (ImGui::SmallButton(m_exactAnchor ? "Anchor" : "World"))
		m_exactAnchor = !m_exactAnchor;
	const auto anchor = device == 0	 ? Anchor::Body
						: device < 3 ? m_playerRig.HandAnchor(device - 1)
									 : m_playerRig.TrackerAnchor(device - 3);
	const auto base = m_exactAnchor ? m_playerRig.AnchorPose(anchor) : IdentityPose();
	const auto world = device == 0	? m_playerRig.Head()
					   : device < 3 ? m_playerRig.Hand(device - 1)
									: m_playerRig.Tracker(device - 3);
	auto pose = RelativePose(world, base);
	const auto q = pose.orientation;
	constexpr float degrees = 180 / std::numbers::pi_v<float>, radians = 1 / degrees;
	std::array<float, 3> angles{std::atan2(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y)) * degrees,
								std::asin(std::clamp(2 * (q.w * q.x - q.y * q.z), -1.0f, 1.0f)) * degrees,
								std::atan2(2 * (q.x * q.y + q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z)) * degrees};
	ImGui::TextUnformatted("Position (m)");
	bool edited =
		Field("##Exact position", ImGui::DragFloat3("##Exact position", &pose.position.x, .005f, 0, 0, "%.3f"), true);
	ImGui::TextUnformatted("Rotation (deg)");
	if (Field("##Exact rotation", ImGui::DragFloat3("##Exact rotation", angles.data(), .5f, 0, 0, "%.1f"), true))
	{
		pose.orientation = EulerAngles(angles[0] * radians, angles[1] * radians, angles[2] * radians);
		edited = true;
	}
	if (edited)
	{
		pose = Compose(base, pose);
		if (device == 0)
			m_playerRig.SetHeadWorld(pose);
		else if (device < 3)
			m_playerRig.SetHandWorld(device - 1, pose);
		else
			m_playerRig.SetTrackerWorld(device - 3, pose);
		Apply(PlayerEdit(true).commands);
	}
	ImGui::EndDisabled();
}
ManualTransaction DeviceControlPanel::PlayerEdit(bool includeWorld) const
{
	return m_observed ? m_playerRig.Transaction(*m_observed, m_manualSource, includeWorld) : ManualTransaction{};
}
void DeviceControlPanel::DrawPlayer()
{
	DrawHandRest();
	ImGui::TextUnformatted("Walk speed (m/s)");
	ImGui::SliderFloat("##Walk speed", &m_playerRig.walkSpeed, .1f, 5.0f, "%.1f");
	ImGui::TextUnformatted("Height (m)");
	bool changed = Field("##Player height",
						 ImGui::DragFloat("##Player height", &m_playerRig.height, .01f, .4f, 2.2f, "%.2f"), true);
	ImGui::TextUnformatted("Floor position (m)");
	changed |= Field("##Player position",
					 ImGui::DragFloat3("##Player position", &m_playerRig.position.x, .01f, 0, 0, "%.2f"), true);
	if (ImGui::Button("Reset rig", {-1, 0}) && m_observed)
	{
		m_playerRig.Reset(*m_observed);
		m_playerRig.position = {};
		m_playerRig.facing = 0;
		m_playerRig.height = m_observed->values.proportions.height;
		for (std::size_t hand = 0; hand < 2; ++hand)
			m_playerRig.Preset(hand, HandPreset::Rest);
		changed = true;
	}
	if (changed)
	{
		auto edit = PlayerEdit();
		edit.continuous = m_continuousEdit;
		m_edit = std::move(edit);
	}
}
void DeviceControlPanel::DrawAnchor(bool tracker, std::size_t index)
{
	int anchor = static_cast<int>(tracker ? m_playerRig.TrackerAnchor(index) : m_playerRig.HandAnchor(index));
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
	{
		if (tracker)
			m_playerRig.SetTrackerAnchor(index, static_cast<Anchor>(anchor));
		else
			m_playerRig.SetHandAnchor(index, static_cast<Anchor>(anchor));
		if (tracker)
		{
			auto edit = PlayerEdit(true);
			edit.continuous = false;
			m_edit = std::move(edit);
		}
	}
}
void DeviceControlPanel::DrawHandRest()
{
	static constexpr const char* names[] = {"Rest", "Forward", "Up", "T-pose"};
	for (int index = 0; index < 4; ++index)
	{
		if (index)
			ImGui::SameLine();
		if (ImGui::Button(names[index]))
		{
			for (std::size_t hand = 0; hand < 2; ++hand)
				m_playerRig.Preset(hand, static_cast<HandPreset>(index));
			auto edit = PlayerEdit(true);
			edit.continuous = false;
			m_edit = std::move(edit);
		}
	}
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
	DrawAnchor(false, hand);
	ImGui::BeginDisabled(DrawScenarioNotice(hand ? ControlChannel::RightPose : ControlChannel::LeftPose));
	auto& controller = m_authoredTracking.controllers[hand];
	DrawInputPad(hand);
	if (placement)
		placement();
	ImGui::SeparatorText("Pose");
	DrawHandRest();
	DrawExactPose(static_cast<int>(hand) + 1);
	if (ImGui::CollapsingHeader("Connection and profile"))
	{
		if (Field("Connected", ImGui::Checkbox("Connected", &controller.connected), false))
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
	auto& body = m_authoredTracking.body;
	bool edited = Field("Body active", ImGui::Checkbox("Body active", &body.active), false);
	ImGui::Text("Confidence");
	edited |= Field("##Confidence", ImGui::SliderFloat("##Confidence", &body.confidence, 0, 1), true);
	ImGui::Text("Pose preset");
	edited |= Field("##Pose preset", ImGui::Combo("##Pose preset", &m_bodyPreset, "Standing\0T pose\0Seated\0"), false);
	body.preset = static_cast<BodyPreset>(m_bodyPreset);
	if (edited)
		ApplyOwned(ControlChannel::Body, SetBodyControls{body});
	ImGui::EndDisabled();
	ImGui::SeparatorText("Body proportions");
	DrawOwner(ControlChannel::Proportions);
	ImGui::Text("Height / metres");
	bool proportionsEdited =
		Field("##Height", ImGui::SliderFloat("##Height", &m_authoredProportions.height, 0.5f, 2.5f), true);
	ImGui::Text("Shoulder width / metres");
	proportionsEdited |=
		Field("##Shoulder width",
			  ImGui::SliderFloat("##Shoulder width", &m_authoredProportions.shoulderWidth, 0.1f, 0.8f), true);
	ImGui::Text("Hip width / metres");
	proportionsEdited |=
		Field("##Hip width", ImGui::SliderFloat("##Hip width", &m_authoredProportions.hipWidth, 0.1f, 0.8f), true);
	if (proportionsEdited)
		ApplyOwned(ControlChannel::Proportions, SetBodyProportions{m_authoredProportions});
}
void DeviceControlPanel::DrawGaze()
{
	ImGui::SeparatorText("Eye gaze");
	DrawOwner(ControlChannel::Gaze);
	ImGui::BeginDisabled(DrawScenarioNotice(ControlChannel::Gaze));
	auto& gaze = m_authoredTracking.gaze;
	ImGui::Text("Yaw");
	bool edited = Field("##Yaw", ImGui::SliderAngle("##Yaw", &gaze.yaw, -90, 90), true);
	ImGui::Text("Pitch");
	edited |= Field("##Pitch", ImGui::SliderAngle("##Pitch", &gaze.pitch, -90, 90), true);
	edited |= Field("Gaze available", ImGui::Checkbox("Gaze available", &gaze.available), false);
	edited |= Field("Gaze tracked", ImGui::Checkbox("Gaze tracked", &gaze.tracked), false);
	if (edited)
		ApplyOwned(ControlChannel::Gaze, SetGaze{gaze});
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawTrackers()
{
	ImGui::SeparatorText("Trackers");
	if (m_observed && !m_observed->trackerAttachmentsAvailable)
		ImGui::TextUnformatted("Trackers unavailable");
	const bool scenario = DrawScenarioNotice(ControlChannel::Trackers);
	ImGui::BeginDisabled(scenario || !m_observed || !m_observed->trackerAttachmentsAvailable);
	DrawOwner(ControlChannel::Trackers);
	ImGui::Text("Tracker slot");
	const bool selected =
		ImGui::Combo("##Tracker slot", &m_selectedTracker, "Tracker 1\0Tracker 2\0Tracker 3\0Tracker 4\0");
	auto& tracker = m_authoredTracking.trackers[static_cast<std::size_t>(m_selectedTracker)];
	DrawAnchor(true, static_cast<std::size_t>(m_selectedTracker));
	DrawExactPose(m_selectedTracker + 3);
	if (selected)
		m_trackerRole = static_cast<int>(tracker.role);
	static constexpr const char* roles[] = {
		"Waist",		 "Chest",		   "Left foot",	 "Right foot",	"Unassigned", "Handheld object",
		"Left shoulder", "Right shoulder", "Left elbow", "Right elbow", "Left knee",  "Right knee",
		"Left wrist",	 "Right wrist",	   "Left ankle", "Right ankle", "Camera",	  "Keyboard"};
	ImGui::Text("Role");
	bool edited = Field("##Role", ImGui::Combo("##Role", &m_trackerRole, roles, IM_ARRAYSIZE(roles)), false);
	tracker.role = static_cast<TrackerRole>(m_trackerRole);
	edited |= Field("Tracker connected", ImGui::Checkbox("Tracker connected", &tracker.connected), false);
	edited |= Field("Tracker tracked", ImGui::Checkbox("Tracker tracked", &tracker.tracked), false);
	ImGui::Text("Persistent identity: %llu", static_cast<unsigned long long>(tracker.identity.value));
	bool duplicateRole = false;
	for (std::size_t index = 0; index < m_authoredTracking.trackers.size(); ++index)
	{
		const auto& other = m_authoredTracking.trackers[index];
		if (index != static_cast<std::size_t>(m_selectedTracker) && tracker.connected && other.connected &&
			tracker.role != TrackerRole::Unassigned && other.role == tracker.role)
		{
			ImGui::TextColored(theme::Vec(theme::Warn), "Role used by tracker %u", static_cast<unsigned>(index + 1));
			duplicateRole = true;
		}
	}
	ImGui::BeginDisabled(duplicateRole);
	if (edited && !duplicateRole)
	{
		ApplyOwned(ControlChannel::Trackers, SetTracker{static_cast<std::uint32_t>(m_selectedTracker), tracker});
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawViews()
{
	ImGui::SeparatorText("View settings");
	DrawOwner(ControlChannel::Views);
	ImGui::Text("Eye separation (IPD)");
	bool edited = Field("##IPD", ImGui::SliderFloat("##IPD", &m_authoredViews.ipd, 0.04f, 0.09f, "%.3f m"), true);
	ImGui::Text("Inset angle");
	float maximum = std::numbers::pi_v<float> / 2;
	for (const auto& fov : m_authoredViews.outerFov)
		maximum = std::min({maximum, (fov.angleRight - fov.angleLeft) / 2, (fov.angleUp - fov.angleDown) / 2});
	edited |= Field("##Inset half angle",
					ImGui::SliderAngle("##Inset half angle", &m_authoredViews.insetHalfAngle, 0.1f,
									   maximum * 180 / std::numbers::pi_v<float>),
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
