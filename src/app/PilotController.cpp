#include "app/PilotController.h"
#include "Math.h"
#include "app/ManualInputController.h"
#include <algorithm>
#include <cmath>

namespace simxr::app
{
namespace
{
unsigned Modifiers()
{
	const auto& io = ImGui::GetIO();
	return (io.KeyCtrl ? 1u : 0u) | (io.KeyShift ? 2u : 0u) | (io.KeyAlt ? 4u : 0u) | (io.KeySuper ? 8u : 0u);
}
bool ButtonsDiffer(const ControllerControls& a, const ControllerControls& b)
{
	return a.primary != b.primary || a.secondary != b.secondary || a.menu != b.menu || a.thumbClick != b.thumbClick;
}
bool ControlsDiffer(const ControllerControls& a, const ControllerControls& b)
{
	return ButtonsDiffer(a, b) || a.trigger != b.trigger || a.grip != b.grip || a.stick.x != b.stick.x ||
		   a.stick.y != b.stick.y;
}
bool AnyHeld(const ControllerControls& value)
{
	return value.primary || value.secondary || value.menu || value.thumbClick || value.trigger || value.grip ||
		   value.stick.x || value.stick.y;
}
} // namespace
PilotController::PilotController(PlayerRig& rig) : m_rig(rig)
{
	Configure(m_keys, m_gamepad);
}
void PilotController::Configure(const PilotBindings& keys, const GamepadBindings& gamepad)
{
	if (m_keys == keys && m_gamepad == gamepad && m_resolved[0] != ImGuiKey_None)
		return;
	m_keys = keys;
	m_gamepad = gamepad;
	m_resolved.fill(ImGuiKey_None);
	for (std::size_t action = 0; action < keys.size(); ++action)
		for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
			if (keys[action].key == ImGui::GetKeyName(static_cast<ImGuiKey>(key)))
				m_resolved[action] = static_cast<ImGuiKey>(key);
}
bool PilotController::Down(PilotAction action) const
{
	const auto index = static_cast<std::size_t>(action);
	return m_resolved[index] != ImGuiKey_None && (Modifiers() & m_keys[index].modifiers) == m_keys[index].modifiers &&
		   ImGui::IsKeyDown(m_resolved[index]);
}
bool PilotController::Pressed(PilotAction action) const
{
	const auto index = static_cast<std::size_t>(action);
	return Down(action) && ImGui::IsKeyPressed(m_resolved[index], false);
}
void PilotController::Stop(Window* window, ManualInputController& input, bool discard, bool forceButtons)
{
	const bool wasActive =
		forceButtons || Active() || m_gamepadActive || AnyHeld(m_controls[0]) || AnyHeld(m_controls[1]);
	if (window)
		window->CapturePointer(false);
	m_captured = m_starting = m_locked = m_gamepadActive = false;
	if (discard)
		input.DiscardUnsentPose();
	if (wasActive)
	{
		try
		{
			const auto state = input.Snapshot();
			const auto observed = ReadObservedSetup(state.state);
			for (unsigned hand = 0; hand < 2; ++hand)
			{
				const auto& owner = observed.owners[static_cast<std::size_t>(hand ? ControlChannel::RightControls
																				  : ControlChannel::LeftControls)];
				m_engaged[hand] |=
					(owner && state.source && *owner == *state.source) || (forceButtons && state.editPending);
			}
			Submit(input, observed, !discard, {}, true);
		}
		catch (const std::exception&)
		{
			input.Submit(ReleaseManualInputs{});
		}
	}
	m_controls = {};
	m_engaged = {};
	m_dpad = {};
	m_handEdits = 0;
	m_poseEngaged = false;
}
void PilotController::Submit(ManualInputController& input, const ObservedSetup& observed, bool poseChanged,
							 const std::array<ControllerControls, 2>& controls, bool final)
{
	const auto state = input.Snapshot();
	const auto source = state.source;
	bool controlsChanged = false;
	for (unsigned hand = 0; hand < 2; ++hand)
	{
		if (ControlsDiffer(controls[hand], m_controls[hand]))
		{
			m_engaged[hand] = true;
			controlsChanged = true;
		}
		const auto& owner =
			observed
				.owners[static_cast<std::size_t>(hand ? ControlChannel::RightControls : ControlChannel::LeftControls)];
		if (m_engaged[hand] && !state.editPending && (!owner || (source && *owner == *source)) &&
			ControlsDiffer(controls[hand], observed.values.input.controllers[hand]))
			controlsChanged = true;
	}
	if (!poseChanged && !controlsChanged && !final)
		return;
	ManualTransaction edit;
	edit.continuous = true;
	m_poseEngaged |= poseChanged;
	if (m_poseEngaged)
		edit = m_rig.Transaction(observed, source);
	for (unsigned hand = 0; hand < 2; ++hand)
	{
		if (!(m_handEdits & (1u << hand)) || m_rig.HandAnchor(hand) != Anchor::World)
			continue;
		const auto channel = hand ? ControlChannel::RightPose : ControlChannel::LeftPose;
		const auto& owner = observed.owners[static_cast<std::size_t>(channel)];
		if (owner && (!source || *owner != *source))
			continue;
		edit.commands.emplace_back(TakeControl{channel, false});
		auto controller = observed.values.tracking.controllers[hand];
		controller.pose.pose = m_rig.Hand(hand);
		edit.commands.emplace_back(SetControllerPose{hand, controller});
	}
	for (unsigned hand = 0; hand < 2; ++hand)
	{
		if (!m_engaged[hand])
			continue;
		const auto channel = hand ? ControlChannel::RightControls : ControlChannel::LeftControls;
		const auto& owner = observed.owners[static_cast<std::size_t>(channel)];
		if (owner && (!source || *owner != *source))
			continue;
		edit.commands.emplace_back(TakeControl{channel, false});
		auto values = controls[hand];
		values.connected = observed.values.tracking.controllers[hand].connected;
		edit.commands.emplace_back(SetControllerControls{hand, values});
		const auto& previous = m_controls[hand];
		const auto& next = controls[hand];
		if (ButtonsDiffer(previous, next))
			edit.continuous = false;
		edit.releaseEdge |= (previous.primary && !next.primary) || (previous.secondary && !next.secondary) ||
							(previous.menu && !next.menu) || (previous.thumbClick && !next.thumbClick) ||
							(previous.trigger && !next.trigger) || (previous.grip && !next.grip);
		// Mouse trigger/grip are button-like edges, even though their wire values are analog.
		if ((previous.trigger == 0 && next.trigger == 1) || (previous.trigger == 1 && next.trigger == 0) ||
			(previous.grip != next.grip))
			edit.continuous = false;
	}
	if (final)
	{
		edit.continuous = false;
		edit.releaseEdge = true;
	}
	if (!edit.commands.empty())
	{
		if (!input.Submit(edit))
			m_error = "Input queue full — release inputs";
		m_lastInput = ImGui::GetTime();
	}
	m_controls = controls;
}
void PilotController::Update(Window& window, ManualInputController& input, bool available, bool hovered)
{
	const auto& io = ImGui::GetIO();
	if (!available || !window.Focused() || io.WantTextInput ||
		ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
		Pressed(PilotAction::Escape))
	{
		Stop(&window, input, true);
		return;
	}
	const auto state = input.Snapshot();
	if (!state.observation)
		return;
	ObservedSetup observed;
	try
	{
		observed = ReadObservedSetup(state.state);
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
		Stop(&window, input, true);
		return;
	}
	if (Pressed(PilotAction::Capture) && (m_captured || (hovered && !ImGui::IsAnyItemActive())))
	{
		if (io.KeyCtrl)
			m_locked = !m_locked;
		if (!m_captured)
		{
			m_starting = true;
			m_error.clear();
			m_startObservation = state.observation;
			input.Refresh();
		}
	}
	if (!Down(PilotAction::Capture) && !m_locked && Active())
		Stop(&window, input, false);
	if (m_starting && state.observation != m_startObservation && !state.posePending)
	{
		m_rig.Observe(observed);
		m_devicePose = m_target == PilotTarget::Head
						   ? observed.values.tracking.head
						   : observed.values.tracking.controllers[m_target == PilotTarget::Left ? 0 : 1].pose;
		if (!window.CapturePointer(true))
		{
			m_error = "Pointer capture unavailable";
			Stop(&window, input, true);
			return;
		}
		m_starting = false;
		m_captured = true;
	}
	const auto pad = window.ReadGamepad(m_gamepad);
	m_gamepadActive = pad.connected;
	std::array<ControllerControls, 2> controls = pad.controls;
	bool poseChanged = false;
	if (pad.connected)
	{
		for (std::size_t button = 0; button < 4; ++button)
			if (pad.dpad[button] && !m_dpad[button])
			{
				if (button < 2)
				{
					for (std::size_t hand = 0; hand < 2; ++hand)
						m_rig.Preset(hand, button == 0 ? HandPreset::Up : HandPreset::Rest);
					poseChanged = true;
					m_handEdits = 3;
				}
				else
					m_hand = button == 2 ? 0 : 1;
			}
		if (pad.gyroActive)
		{
			auto hand = m_rig.Hand(m_hand);
			hand.orientation =
				Multiply(hand.orientation, EulerAngles(-pad.gyro[1] * io.DeltaTime, -pad.gyro[0] * io.DeltaTime,
													   -pad.gyro[2] * io.DeltaTime));
			m_rig.SetHandWorld(m_hand, hand);
			m_handEdits |= 1u << m_hand;
			poseChanged = true;
		}
	}
	m_dpad = pad.dpad;
	if (m_captured)
	{
		if (Pressed(PilotAction::SwapHand))
			m_hand = 1 - m_hand;
		for (const auto& [action, target] : {std::pair{PilotAction::TargetHead, PilotTarget::Head},
											 {PilotAction::TargetLeft, PilotTarget::Left},
											 {PilotAction::TargetRight, PilotTarget::Right},
											 {PilotAction::TargetPlayer, PilotTarget::Player}})
			if (Pressed(action))
			{
				m_target = target;
				m_rig.Observe(observed);
				m_devicePose = target == PilotTarget::Head
								   ? observed.values.tracking.head
								   : observed.values.tracking.controllers[target == PilotTarget::Left ? 0 : 1].pose;
			}
		const auto [dx, dy] = window.TakePointerMotion();
		const auto axis = [&](PilotAction positive, PilotAction negative)
		{
			return float(Down(positive)) - float(Down(negative));
		};
		XrVector3f velocity{axis(PilotAction::Right, PilotAction::Left), axis(PilotAction::Up, PilotAction::Down),
							axis(PilotAction::Back, PilotAction::Forward)};
		const float length = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
		if (length > 1)
		{
			velocity.x /= length;
			velocity.z /= length;
		}
		const float speed = Down(PilotAction::Fast) ? 3.0f : Down(PilotAction::Slow) ? .3f : 1.0f;
		velocity = {velocity.x * speed, velocity.y * speed, velocity.z * speed};
		const bool reachLeft = Down(PilotAction::ReachLeft), reachRight = Down(PilotAction::ReachRight);
		const bool freeLook = Down(PilotAction::FreeLook);
		if (reachLeft || reachRight)
		{
			for (unsigned hand = 0; hand < 2; ++hand)
			{
				if (!(hand ? reachRight : reachLeft))
					continue;
				auto pose = m_rig.Hand(hand);
				if (freeLook)
					pose.orientation =
						Multiply(EulerAngles(-dx * .004f, -dy * .004f, io.MouseWheel * .1f), pose.orientation);
				else
				{
					const auto movement =
						Rotate(m_rig.Head().orientation, {dx * .002f, -dy * .002f, -io.MouseWheel * .05f});
					pose.position = {pose.position.x + movement.x, pose.position.y + movement.y,
									 pose.position.z + movement.z};
				}
				m_rig.SetHandWorld(hand, pose);
				m_handEdits |= 1u << hand;
			}
			poseChanged |= dx || dy || io.MouseWheel;
		}
		if (velocity.x || velocity.y || velocity.z || ((!reachLeft && !reachRight) && (dx || dy)))
		{
			const float yaw = reachLeft || reachRight ? 0 : -dx * .003f;
			const float pitch = reachLeft || reachRight ? 0 : -dy * .003f;
			if (m_target == PilotTarget::Player)
				m_rig.Move(velocity, yaw, pitch, io.DeltaTime, freeLook);
			else
			{
				m_devicePose = PilotPose(m_devicePose, velocity, yaw, pitch, io.DeltaTime);
				if (m_target == PilotTarget::Head)
					m_rig.SetHeadWorld(m_devicePose.pose);
				else
				{
					const unsigned hand = m_target == PilotTarget::Left ? 0u : 1u;
					m_rig.SetHandWorld(hand, m_devicePose.pose);
					m_handEdits |= 1u << hand;
				}
			}
			poseChanged = true;
		}
		auto& hand = controls[m_hand];
		hand.trigger = std::max(hand.trigger, Down(PilotAction::Trigger) ? 1.0f : 0.0f);
		hand.grip = std::max(hand.grip, Down(PilotAction::Grip) ? 1.0f : 0.0f);
		hand.primary |= Down(PilotAction::Primary);
		hand.secondary |= Down(PilotAction::Secondary);
		hand.menu |= Down(PilotAction::Menu);
		hand.thumbClick |= Down(PilotAction::StickClick);
		hand.stick.x = std::clamp(hand.stick.x + axis(PilotAction::StickRight, PilotAction::StickLeft), -1.0f, 1.0f);
		hand.stick.y = std::clamp(hand.stick.y + axis(PilotAction::StickUp, PilotAction::StickDown), -1.0f, 1.0f);
	}
	Submit(input, observed, poseChanged, controls);
}
void PilotController::DrawHelp() const
{
	ImGui::TextUnformatted("Ctrl + capture: lock   |   Esc: release");
	if (ImGui::BeginTable("Pilot bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, {360, 300}))
	{
		for (std::size_t index = 0; index < m_keys.size(); ++index)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(PilotActionNames[index]);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(PilotBindingLabel(m_keys[index]).c_str());
		}
		ImGui::EndTable();
	}
}
bool PilotController::DrawHud(ImVec2 minimum, ImVec2 maximum, ManualInputController& input)
{
	const float scale = ImGui::GetFontSize() / 16;
	const float width = std::min((maximum.x - minimum.x) * .7f, 500 * scale);
	if (width < 160)
		return false;
	const auto saved = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos({(minimum.x + maximum.x - width) / 2, maximum.y - 42 * scale});
	ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 25, 33, 215));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, Active() || ImGui::GetTime() - m_lastInput < 3 ? 1.0f : .4f);
	ImGui::BeginChild("Pilot HUD", {width, 36 * scale}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
	ImGui::SetNextItemWidth(width * .24f);
	int target = static_cast<int>(m_target);
	if (ImGui::Combo("##Pilot target", &target, "Player\0Head\0Left\0Right\0"))
		m_target = static_cast<PilotTarget>(target);
	ImGui::SameLine();
	if (ImGui::SmallButton(m_hand ? "R" : "L"))
		m_hand = 1 - m_hand;
	ImGui::SetItemTooltip("Input hand (Tab)");
	ImGui::SameLine();
	if (ImGui::SmallButton(m_locked ? "Locked" : "Lock"))
		m_locked = !m_locked;
	ImGui::SameLine();
	bool conflict = false;
	const auto state = input.Snapshot();
	std::vector<ControlChannel> channels{m_hand ? ControlChannel::RightControls : ControlChannel::LeftControls};
	if (m_target == PilotTarget::Player)
		channels.insert(channels.end(), {ControlChannel::Head, ControlChannel::LeftPose, ControlChannel::RightPose,
										 ControlChannel::Trackers});
	else
		channels.push_back(m_target == PilotTarget::Head   ? ControlChannel::Head
						   : m_target == PilotTarget::Left ? ControlChannel::LeftPose
														   : ControlChannel::RightPose);
	std::optional<ObservedSetup> observed;
	try
	{
		if (state.observation)
			observed = ReadObservedSetup(state.state);
	}
	catch (const std::exception&)
	{
	}
	if (observed)
		for (const auto channel : channels)
		{
			const auto& owner = observed->owners[static_cast<std::size_t>(channel)];
			conflict |= owner && (!state.source || *owner != *state.source);
		}
	if (conflict)
	{
		if (ImGui::SmallButton("Take over"))
		{
			m_rig.Observe(*observed);
			ManualTransaction edit;
			for (const auto channel : channels)
				edit.commands.emplace_back(TakeControl{channel, true});
			input.Submit(std::move(edit));
		}
		ImGui::SetItemTooltip("Another source owns a pilot channel");
	}
	else
	{
		ImGui::Text("T %.1f G %.1f%s%s", m_controls[m_hand].trigger, m_controls[m_hand].grip,
					m_controls[m_hand].primary ? " A" : "", m_gamepadActive ? " Pad" : "");
	}
	ImGui::SameLine();
	const auto stickMin = ImGui::GetCursorScreenPos();
	const float radius = 7 * scale;
	ImGui::Dummy({radius * 2 + 2, ImGui::GetFrameHeight()});
	const ImVec2 stickCenter{stickMin.x + radius + 1, stickMin.y + ImGui::GetFrameHeight() / 2};
	ImGui::GetWindowDrawList()->AddCircle(stickCenter, radius, ImGui::GetColorU32(ImGuiCol_TextDisabled));
	ImGui::GetWindowDrawList()->AddCircleFilled(
		{stickCenter.x + m_controls[m_hand].stick.x * radius, stickCenter.y - m_controls[m_hand].stick.y * radius},
		2 * scale, ImGui::GetColorU32(ImGuiCol_Text));
	const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	ImGui::SetCursorScreenPos({saved.x, saved.y - ImGui::GetStyle().ItemSpacing.y});
	ImGui::Dummy({0, 0});
	return hovered;
}
} // namespace simxr::app
