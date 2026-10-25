#include "app/widgets/PilotController.h"
#include "Math.h"
#include "Pch.h"
#include "app/platform/Window.h"
#include "app/services/ManualInputController.h"
#include "app/services/PlayerRig.h"
#include <cmath>

namespace simxr::app
{
static unsigned Modifiers()
{
	const auto& io = ImGui::GetIO();
	return (io.KeyCtrl ? 1u : 0u) | (io.KeyShift ? 2u : 0u) | (io.KeyAlt ? 4u : 0u) | (io.KeySuper ? 8u : 0u);
}
static bool ButtonsDiffer(const ControllerControls& a, const ControllerControls& b)
{
	return a.primary != b.primary || a.secondary != b.secondary || a.menu != b.menu || a.thumbClick != b.thumbClick;
}
static bool ControlsDiffer(const ControllerControls& a, const ControllerControls& b)
{
	return ButtonsDiffer(a, b) || a.trigger != b.trigger || a.grip != b.grip || a.stick.x != b.stick.x ||
		   a.stick.y != b.stick.y;
}
static bool AnyHeld(const ControllerControls& value)
{
	return value.primary || value.secondary || value.menu || value.thumbClick || value.trigger || value.grip ||
		   value.stick.x || value.stick.y;
}
PilotController::PilotController(PlayerRig& rig) : m_rig(rig)
{
	Configure(m_keys, m_gamepad);
}
void PilotController::Configure(const PilotBindings& keys, const GamepadBindings& gamepad, bool remoteMouseDrag)
{
	if (m_remoteMouseDrag != remoteMouseDrag)
	{
		m_remoteMouseDrag = remoteMouseDrag;
		m_captureModeChanged = true;
	}
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
	{
		input.DiscardUnsentPose();
		m_poseEngaged = false;
		m_handEdits = 0;
	}
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
			Submit(input, observed, false, {}, true);
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
			m_error = "Too many inputs. Release the controls and try again.";
		m_lastInput = ImGui::GetTime();
	}
	m_controls = controls;
}
void PilotController::Update(Window& window, ManualInputController& input, bool available, bool hovered,
							 bool automaticGamepad)
{
	const auto gamepadIdentity = window.GamepadIdentity();
	const bool removed = m_gamepadIdentity && m_gamepadIdentity != gamepadIdentity;
	m_gamepadIdentity = gamepadIdentity;
	const auto& io = ImGui::GetIO();
	if (std::exchange(m_captureModeChanged, false) || removed || !available || !window.Focused() || io.WantTextInput ||
		ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
		Pressed(PilotAction::Escape))
	{
		Stop(&window, input, true);
		return;
	}
	const auto state = input.Snapshot();
	if (!state.observation)
	{
		Stop(&window, input, true);
		return;
	}
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
		if (io.KeyCtrl && !m_remoteMouseDrag)
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
		if (!window.CapturePointer(true, !m_remoteMouseDrag))
		{
			m_error = "Pointer capture unavailable";
			Stop(&window, input, true);
			return;
		}
		m_starting = false;
		m_captured = true;
	}
	const auto pad = automaticGamepad || m_captured ? window.ReadGamepad(m_gamepad) : GamepadSample{};
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
				m_target = target;
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
				const unsigned hand = m_target == PilotTarget::Left ? 0u : 1u;
				PoseSample pose = m_target == PilotTarget::Head ? observed.values.tracking.head
																: observed.values.tracking.controllers[hand].pose;
				pose.pose = m_target == PilotTarget::Head ? m_rig.Head() : m_rig.Hand(hand);
				pose = PilotPose(pose, velocity, yaw, pitch, io.DeltaTime);
				if (m_target == PilotTarget::Head)
					m_rig.SetHeadWorld(pose.pose);
				else
				{
					m_rig.SetHandWorld(hand, pose.pose);
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
} // namespace simxr::app
