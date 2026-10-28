#include "app/UiPch.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/services/PlayerRig.h"
#include "app/widgets/Controls.h"
#include "app/widgets/InputPad.h"
#include "app/widgets/Theme.h"
#include <imgui_internal.h>

namespace simxr::app
{
void DeviceControlPanel::ClearHeldInputs()
{
	CancelEditing();
	for (const auto id : m_padActiveIds)
		if (id && ImGui::GetActiveID() == id)
			ImGui::ClearActiveID();
	m_authoredControls = {};
	m_latches = {};
	m_heldInputs = {};
	m_padFrames = {-1, -1};
	m_padActiveIds = {};
}
std::optional<SimulationEdit> DeviceControlPanel::EndInputFrame()
{
	if (m_fieldFrame != ImGui::GetFrameCount() || !Active())
		CancelEditing();
	ManualTransaction release;
	release.releaseEdge = true;
	for (std::uint32_t hand = 0; hand < m_heldInputs.size(); ++hand)
	{
		if (m_padFrames[hand] == ImGui::GetFrameCount())
			continue;
		const auto held = std::exchange(m_heldInputs[hand], {});
		const auto id = std::exchange(m_padActiveIds[hand], 0);
		if (id && ImGui::GetActiveID() == id)
			ImGui::ClearActiveID();
		if (!m_observed || !(held.trigger || held.grip || held.stick || held.primary || held.secondary || held.menu))
			continue;
		const auto channel = hand ? ControlChannel::RightControls : ControlChannel::LeftControls;
		const auto owner = m_observed->owners[static_cast<std::size_t>(channel)];
		if (owner && owner != m_manualSource)
		{
			m_latches[hand] = {};
			continue;
		}
		auto& controls = m_authoredControls[hand];
		const auto& latched = m_latches[hand];
		if (held.trigger && !latched.trigger)
			controls.trigger = 0;
		if (held.grip && !latched.grip)
			controls.grip = 0;
		if (held.primary && !latched.primary)
			controls.primary = false;
		if (held.secondary && !latched.secondary)
			controls.secondary = false;
		if (held.menu && !latched.menu)
			controls.menu = false;
		if (held.stick)
		{
			controls.thumbClick = false;
			if (!latched.stick)
				controls.stick = {};
		}
		controls.connected = m_authoredTracking.controllers[hand].connected;
		controls.physicalProfile.clear();
		controls.triggerClick.reset();
		release.commands.emplace_back(TakeControl{channel, false});
		release.commands.emplace_back(SetControllerControls{hand, controls});
	}
	if (release.commands.empty())
		return {};
	return release;
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
	const bool touch = profile.find("oculus") != std::string_view::npos ||
					   profile.find("meta") != std::string_view::npos || profile.find("pico") != std::string_view::npos;
	const bool index = profile.find("valve/index") != std::string_view::npos;
	const char* primary = touch ? (hand ? "A##Primary" : "X##Primary") : index ? "A##Primary" : "1##Primary";
	const char* secondary = touch ? (hand ? "B##Secondary" : "Y##Secondary") : index ? "B##Secondary" : "2##Secondary";
	ImGui::PushID(static_cast<int>(hand));
	SectionLabel("Input");
	const auto channel = hand ? ControlChannel::RightControls : ControlChannel::LeftControls;
	DrawOwner(channel, false);
	const auto owner = m_observed ? m_observed->owners[static_cast<std::size_t>(channel)] : std::nullopt;
	const bool otherOwner = owner && owner != m_manualSource;
	if (otherOwner)
	{
		latched = {};
		if (ImGui::SmallButton("Take over##Input"))
			Apply({TakeControl{channel, true}});
	}
	ImGui::BeginDisabled(otherOwner);
	const bool enabled = !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
	if (enabled)
	{
		m_padFrames[hand] = ImGui::GetFrameCount();
		m_padActiveIds[hand] = 0;
	}
	const auto held = [&](bool& value)
	{
		if (enabled)
		{
			value = ImGui::IsItemActive() && ImGui::GetCurrentContext()->ActiveIdMouseButton == ImGuiMouseButton_Left &&
					ImGui::IsMouseDown(ImGuiMouseButton_Left);
			if (value)
				m_padActiveIds[hand] = ImGui::GetItemID();
		}
	};
	auto& holds = m_heldInputs[hand];
	bool changed = false, continuous = false;
	bool stickClick = controls.thumbClick;
	const auto top = ImGui::GetCursorPosY();
	const bool stick = StickPad("Stick", controls.stick.x, controls.stick.y, stickClick, latched.stick, color);
	held(holds.stick);
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
	held(holds.trigger);
	changed |= InputField("Trigger", trigger);
	continuous |= trigger && ImGui::IsItemActive();
	const bool grip = TriggerBar("Grip", "Grip", controls.grip, latched.grip, color, width);
	held(holds.grip);
	changed |= InputField("Grip", grip);
	continuous |= grip && ImGui::IsItemActive();
	changed |= InputField("Primary", HoldButton("Primary", primary, controls.primary, latched.primary, color));
	held(holds.primary);
	ImGui::SameLine();
	changed |=
		InputField("Secondary", HoldButton("Secondary", secondary, controls.secondary, latched.secondary, color));
	held(holds.secondary);
	ImGui::SameLine();
	changed |= InputField("Menu", HoldButton("Menu", "M##Menu", controls.menu, latched.menu, color));
	held(holds.menu);
	// Release-all sits at the row's right end, centred on the round buttons.
	const float buttonRow = ImGui::GetItemRectSize().y;
	ImGui::SameLine(width - ImGui::GetFrameHeight());
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (buttonRow - ImGui::GetFrameHeight()) / 2);
	const bool clear = IconButton("Clear hand", Icon::Close, "Release every input of this hand");
	ImGui::EndGroup();
	ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + stickHeight + spacing));
	ImGui::EndDisabled();
	ImGui::PopID();
	if (clear)
	{
		controls = {};
		latched = {};
	}
	if ((!clear && !changed) || !CanControl(channel))
		return;
	controls.connected = controller.connected;
	controls.physicalProfile.clear();
	controls.triggerClick.reset();
	ApplyOwned(hand ? ControlChannel::RightControls : ControlChannel::LeftControls,
			   SetControllerControls{hand, controls});
	auto& edit = std::get<ManualTransaction>(*m_edit);
	const bool buttonEdge = previous.primary != controls.primary || previous.secondary != controls.secondary ||
							previous.menu != controls.menu || previous.thumbClick != controls.thumbClick;
	const bool barEdge = (previous.trigger != controls.trigger && (controls.trigger == 0 || previous.trigger == 0)) ||
						 (previous.grip != controls.grip && (controls.grip == 0 || previous.grip == 0));
	const bool stickReleased =
		(previous.stick.x != 0 || previous.stick.y != 0) && controls.stick.x == 0 && controls.stick.y == 0;
	edit.continuous = continuous && !buttonEdge && !barEdge && !stickReleased;
	edit.releaseEdge =
		clear || (previous.primary && !controls.primary) || (previous.secondary && !controls.secondary) ||
		(previous.menu && !controls.menu) || (previous.thumbClick && !controls.thumbClick) ||
		(previous.trigger != 0 && controls.trigger == 0) || (previous.grip != 0 && controls.grip == 0) || stickReleased;
}
void DeviceControlPanel::DrawController(std::uint32_t hand, const std::function<void()>& placement)
{
	const auto channel = hand ? ControlChannel::RightPose : ControlChannel::LeftPose;
	DrawOwner(channel);
	ImGui::BeginDisabled(!CanControl(channel) || ScenarioActive(channel));
	DrawAnchor(hand);
	ImGui::EndDisabled();
	ImGui::BeginDisabled(ScenarioActive(channel));
	auto& controller = m_authoredTracking.controllers[hand];
	DrawInputPad(hand);
	if (placement)
		placement();
	SectionLabel("Pose");
	ImGui::BeginDisabled(!CanControl(channel));
	DrawHandPresets(hand);
	DrawExactPose(static_cast<int>(hand) + 1);
	ImGui::EndDisabled();
	if (Section("Connection and profile"))
	{
		ImGui::BeginDisabled(!CanControl(channel));
		if (Field("Connected", ImGui::Checkbox("Connected", &controller.connected), false,
				  SwitchValue(controller.connected, true)))
			ApplyOwned(channel, SetControllerPose{hand, controller});
		ImGui::EndDisabled();
		if (m_observed)
		{
			const auto& physical = m_observed->values.input.controllers[hand].physicalProfile;
			if (!physical.empty())
			{
				ImGui::TextWrapped("Physical: %s", physical.c_str());
				if (m_observed->controllerProfiles)
				{
					const auto& effective = (*m_observed->controllerProfiles)[hand];
					ImGui::TextWrapped("Game: %s", effective.empty() ? "No active binding" : effective.c_str());
				}
				else
					ImGui::TextUnformatted("Game: Unavailable");
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("Profile from the game's latest action synchronization. It may differ if the "
									  "game uses fallback bindings.");
			}
		}
		ImGui::TextUnformatted("Simulated profile path");
		DrawOwner(ControlChannel::Profiles);
		ImGui::BeginDisabled(!CanControl(ControlChannel::Profiles));
		auto& profile = m_controllerProfiles[hand];
		Field("##Interaction profile", ImGui::InputText("##Interaction profile", profile.data(), profile.size()),
			  false);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", profile.data());
		if (!m_edit && !(ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) && ImGui::IsItemDeactivatedAfterEdit())
			ApplyOwned(ControlChannel::Profiles, SetControllerProfile{hand, profile.data()});
		ImGui::EndDisabled();
	}
	ImGui::EndDisabled();
}
} // namespace simxr::app
