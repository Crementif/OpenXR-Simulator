#include "app/PilotController.h"
#include "app/Theme.h"
#include "Math.h"
#include "app/ManualInputController.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
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
std::string PilotController::KeyName(PilotAction action) const
{
	const auto& binding = m_keys[static_cast<std::size_t>(action)];
	auto name = PilotBindingLabel(binding);
	static constexpr std::pair<std::string_view, std::string_view> friendly[] = {
		{"MouseRight", "right mouse"}, {"MouseLeft", "left mouse"}, {"MouseMiddle", "middle mouse"},
		{"Escape", "Esc"},			   {"LeftCtrl", "Ctrl"},		{"LeftShift", "Shift"},
		{"LeftAlt", "Alt"}};
	for (const auto& [raw, shown] : friendly)
		if (binding.key == raw)
			name = name.substr(0, name.size() - raw.size()) + std::string{shown};
	return name;
}
void PilotController::DrawHelp() const
{
	ImGui::Text("%s releases the pointer and all held inputs.", KeyName(PilotAction::Escape).c_str());
	ImGui::Text("Ctrl+%s locks capture so you don't have to hold it.", KeyName(PilotAction::Capture).c_str());
	if (ImGui::BeginTable("Pilot bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, {360, 300}))
	{
		for (std::size_t index = 0; index < m_keys.size(); ++index)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(PilotActionNames[index]);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(KeyName(static_cast<PilotAction>(index)).c_str());
		}
		ImGui::EndTable();
	}
}
bool PilotController::DrawHud(ImVec2 minimum, ImVec2 maximum, ManualInputController& input)
{
	const float scale = ImGui::GetFontSize() / 16;
	const float width = std::min((maximum.x - minimum.x) * .8f, 720 * scale);
	if (width < 200 * scale)
		return false;
	const auto& style = ImGui::GetStyle();
	const float rowHeight = ImGui::GetFrameHeight();
	const auto textWidth = [](const char* text) { return ImGui::CalcTextSize(text).x; };
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
	static constexpr const char* targetNames[] = {"Player", "Head", "Left hand", "Right hand"};
	static constexpr PilotAction targetKeys[] = {PilotAction::TargetPlayer, PilotAction::TargetHead,
												 PilotAction::TargetLeft, PilotAction::TargetRight};
	const auto targetIndex = static_cast<std::size_t>(m_target);
	const std::string preview = std::string{"Move: "} + targetNames[targetIndex];
	const char* lock = m_locked ? "Unlock" : "Lock";
	const ImVec2 padding{8 * scale, 6 * scale};
	const float buttonPadding = style.FramePadding.x * 2;
	// Groups wrap onto further rows when the image is too narrow for one row.
	std::array<float, 5> groupWidths{
		textWidth(preview.c_str()) + ImGui::GetFrameHeight() * 1.5f,
		textWidth("Buttons: Right") + buttonPadding,
		conflict ? textWidth("Take over") + buttonPadding
				 : textWidth("Trigger") + textWidth("Grip") + 2 * 40 * scale + style.ItemSpacing.x,
		conflict ? 0 : textWidth("A") + textWidth("B") + textWidth("Menu") + 3 * 14 * scale + 26 * scale,
		textWidth(lock) + buttonPadding};
	std::array<int, 5> groupRows{};
	int rows = 1;
	float height = 0;
	const auto layout = [&]
	{
		rows = 1;
		for (float used = 0, available = width - padding.x * 2; const auto index : {0, 1, 2, 3, 4})
		{
			if (!groupWidths[index])
				continue;
			const float needed = groupWidths[index] + (used > 0 ? style.ItemSpacing.x : 0);
			if (used > 0 && used + needed > available)
			{
				++rows;
				used = 0;
			}
			used += used > 0 ? needed : groupWidths[index];
			groupRows[index] = rows - 1;
		}
		height = rows * rowHeight + ImGui::GetTextLineHeight() + padding.y * 2 + rows * style.ItemSpacing.y;
	};
	layout();
	// On a small image the HUD keeps only what is not on the hand page: target, hand and lock.
	const bool compact = !conflict && height > (maximum.y - minimum.y) * .3f;
	if (compact)
	{
		groupWidths[2] = groupWidths[3] = 0;
		layout();
	}
	const auto place = [&](int group)
	{
		int previous = group - 1;
		while (previous >= 0 && !groupWidths[previous])
			--previous;
		if (previous >= 0 && groupRows[group] == groupRows[previous])
			ImGui::SameLine();
	};
	const auto saved = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos({(minimum.x + maximum.x - width) / 2, maximum.y - height - 6 * scale});
	// Idle, only the content dims; the backing stays solid so markers under it do not show through.
	ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, Active() || ImGui::GetTime() - m_lastInput < 3 ? 1.0f : .6f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::BeginChild("Pilot HUD", {width, height}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
					  ImGuiWindowFlags_NoScrollbar);
	auto* draw = ImGui::GetWindowDrawList();
	const auto corner = ImGui::GetWindowPos();
	draw->AddRectFilled(corner, {corner.x + width, corner.y + height}, theme::Bg,
						ImGui::GetStyle().ChildRounding);
	const auto& controls = m_controls[m_hand];
	ImGui::SetNextItemWidth(groupWidths[0]);
	if (ImGui::BeginCombo("##Pilot target", preview.c_str()))
	{
		for (std::size_t index = 0; index < std::size(targetNames); ++index)
		{
			const auto label = std::string{targetNames[index]} + "  (" + KeyName(targetKeys[index]) + ")";
			if (ImGui::Selectable(label.c_str(), index == targetIndex))
				m_target = static_cast<PilotTarget>(index);
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("What mouse look and walking keys move");
	place(1);
	if (ImGui::Button(m_hand ? "Buttons: Right" : "Buttons: Left"))
		m_hand = 1 - m_hand;
	ImGui::SetItemTooltip("Trigger, grip and buttons go to this hand. Switch with %s.",
						  KeyName(PilotAction::SwapHand).c_str());
	const auto bar = [&](const char* label, float value)
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine(0, 4 * scale);
		const auto position = ImGui::GetCursorScreenPos();
		const ImVec2 size{36 * scale, 8 * scale};
		ImGui::Dummy({size.x, rowHeight});
		const ImVec2 top{position.x, position.y + (rowHeight - size.y) / 2};
		draw->AddRectFilled(top, {top.x + size.x, top.y + size.y}, ImGui::GetColorU32(theme::Line), 2);
		draw->AddRectFilled(top, {top.x + size.x * std::clamp(value, 0.0f, 1.0f), top.y + size.y},
							ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2);
	};
	const auto pill = [&](const char* label, bool held)
	{
		const auto text = ImGui::CalcTextSize(label);
		const auto position = ImGui::GetCursorScreenPos();
		const ImVec2 size{text.x + 10 * scale, text.y + 4 * scale};
		ImGui::Dummy({size.x, rowHeight});
		const ImVec2 top{position.x, position.y + (rowHeight - size.y) / 2};
		const ImVec2 bottom{top.x + size.x, top.y + size.y};
		if (held)
			draw->AddRectFilled(top, bottom, ImGui::GetColorU32(ImGuiCol_ButtonActive), size.y / 2);
		else
			draw->AddRect(top, bottom, ImGui::GetColorU32(ImGuiCol_Border), size.y / 2);
		draw->AddText({top.x + 5 * scale, top.y + 2 * scale},
					  ImGui::GetColorU32(held ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
	};
	if (conflict)
	{
		place(2);
		if (ImGui::Button("Take over"))
		{
			m_rig.Observe(*observed);
			ManualTransaction edit;
			for (const auto channel : channels)
				edit.commands.emplace_back(TakeControl{channel, true});
			input.Submit(std::move(edit));
		}
		ImGui::SetItemTooltip("Take these devices back from the replay or script that controls them.");
	}
	else if (!compact)
	{
		place(2);
		bar("Trigger", controls.trigger);
		ImGui::SameLine();
		bar("Grip", controls.grip);
		place(3);
		pill(m_hand ? "A" : "X", controls.primary);
		ImGui::SameLine(0, 4 * scale);
		pill(m_hand ? "B" : "Y", controls.secondary);
		ImGui::SameLine(0, 4 * scale);
		pill("Menu", controls.menu);
		ImGui::SameLine(0, 8 * scale);
		const auto stickMin = ImGui::GetCursorScreenPos();
		const float radius = 8 * scale;
		ImGui::Dummy({radius * 2 + 2, rowHeight});
		ImGui::SetItemTooltip("Thumbstick");
		const ImVec2 stickCenter{stickMin.x + radius + 1, stickMin.y + rowHeight / 2};
		draw->AddCircle(stickCenter, radius, ImGui::GetColorU32(controls.thumbClick ? ImGuiCol_ButtonActive
																					: ImGuiCol_TextDisabled));
		draw->AddCircleFilled({stickCenter.x + controls.stick.x * radius, stickCenter.y - controls.stick.y * radius},
							  2.5f * scale, ImGui::GetColorU32(ImGuiCol_Text));
	}
	place(4);
	if (ImGui::SmallButton(lock))
		m_locked = !m_locked;
	ImGui::SetItemTooltip("Keep the pointer captured without holding %s.", KeyName(PilotAction::Capture).c_str());
	std::string hint;
	if (conflict)
		hint = "A replay or script controls these devices";
	else if (m_captured)
		hint = (m_locked ? std::string{"Capture locked · "} : std::string{}) + KeyName(PilotAction::Escape) +
			   " releases · " + KeyName(PilotAction::FreeLook) + ": look without turning";
	else
		hint = "Hold " + KeyName(PilotAction::Capture) + " over the image to pilot";
	if (m_gamepadActive)
		hint += " · Gamepad active";
	if (textWidth(hint.c_str()) > width - padding.x * 2)
		hint = m_captured ? KeyName(PilotAction::Escape) + " releases"
						  : "Hold " + KeyName(PilotAction::Capture) + " to pilot";
	ImGui::TextDisabled("%s", hint.c_str());
	const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	ImGui::SetCursorScreenPos({saved.x, saved.y - ImGui::GetStyle().ItemSpacing.y});
	ImGui::Dummy({0, 0});
	return hovered;
}
} // namespace simxr::app
