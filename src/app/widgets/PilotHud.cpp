#include "Pch.h"
#include "app/Theme.h"
#include "app/services/ManualInputController.h"
#include "app/services/PlayerRig.h"
#include "app/widgets/Controls.h"
#include "app/widgets/PilotController.h"

namespace simxr::app
{
std::string PilotController::KeyName(PilotAction action) const
{
	const auto& binding = m_keys[static_cast<std::size_t>(action)];
	auto name = PilotBindingLabel(binding);
	static constexpr std::pair<std::string_view, std::string_view> s_friendly[] = {{"MouseRight", "right mouse"},
																				 {"MouseLeft", "left mouse"},
																				 {"MouseMiddle", "middle mouse"},
																				 {"Escape", "Esc"},
																				 {"LeftCtrl", "Ctrl"},
																				 {"LeftShift", "Shift"},
																				 {"LeftAlt", "Alt"}};
	for (const auto& [raw, shown] : s_friendly)
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
	const float margin = 8 * scale, available = maximum.x - minimum.x - margin * 2;
	if (available < 120 * scale || maximum.y - minimum.y < 80 * scale)
		return false;
	const auto& style = ImGui::GetStyle();
	const float rowHeight = ImGui::GetFrameHeight();
	const auto textWidth = [](const char* text)
	{
		return ImGui::CalcTextSize(text).x;
	};
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
	{
		if (m_target == PilotTarget::Player && (observed->bodyRootAvailable || observed->bodyOverridesAvailable))
			channels.push_back(ControlChannel::Body);
		for (const auto channel : channels)
		{
			const auto& owner = observed->owners[static_cast<std::size_t>(channel)];
			conflict |= owner && (!state.source || *owner != *state.source);
		}
	}
	static constexpr const char* s_targetNames[] = {"Player", "Head", "Left hand", "Right hand"};
	static constexpr PilotAction s_targetKeys[] = {PilotAction::TargetPlayer, PilotAction::TargetHead,
												 PilotAction::TargetLeft, PilotAction::TargetRight};
	const auto targetIndex = static_cast<std::size_t>(m_target);
	const std::string preview = std::string{"Move: "} + s_targetNames[targetIndex];
	const char* lock = m_locked ? "Unlock" : "Lock";
	const ImVec2 padding{8 * scale, 5 * scale};
	const float buttonPadding = style.FramePadding.x * 2, gap = style.ItemSpacing.x;
	const float chevron = rowHeight;
	const auto& controls = m_controls[m_hand];
	std::string hint;
	if (conflict)
		hint = "A replay or script controls these devices";
	else if (m_captured)
		hint = (m_locked ? std::string{"Locked · "} : std::string{}) + KeyName(PilotAction::Escape) + " releases · " +
			   KeyName(PilotAction::FreeLook) + ": look only";
	else
		hint = "Hold " + KeyName(PilotAction::Capture) + " over the image to pilot";
	// Group widths, content only.
	const float targetWidth = textWidth(preview.c_str()) + rowHeight * 1.5f;
	float left = targetWidth + gap + rowHeight + textWidth(lock) + style.FramePadding.x + gap + rowHeight;
	const float handButton = textWidth("Buttons: Right") + buttonPadding;
	const float bars = textWidth("Trigger") + textWidth("Grip") + 2 * (36 + 4) * scale + gap;
	const float pills = textWidth("A") + textWidth("B") + textWidth("Menu") + 3 * 10 * scale + 8 * scale;
	const float sticks = (m_gamepadActive ? 2 : 1) * (16 * scale + 2) + 8 * scale;
	float middle = conflict ? handButton + gap + textWidth("Take over") + buttonPadding
							: handButton + gap + bars + gap + pills + sticks;
	float right = textWidth(hint.c_str()) + (m_gamepadActive ? textWidth("Gamepad") + 10 * scale + gap : 0);
	const float frame = padding.x * 2;
	// Too narrow for all three: the hint goes, then the middle keeps only the hand switch.
	bool showRight = true, fullMiddle = true;
	if (left + middle + right + chevron + frame * 3 + gap * 2 > available)
		showRight = false;
	if (left + middle + chevron + frame * 2 + gap > available)
	{
		fullMiddle = conflict;
		middle = conflict ? middle : handButton;
	}
	const bool folded = m_hudFolded || left + middle + chevron + frame * 2 + gap > available;
	const float height = rowHeight + padding.y * 2;
	const float y = maximum.y - height - margin;
	const auto saved = ImGui::GetCursorScreenPos();
	bool hovered = false;
	// Idle, only the content dims; each flat backing stays mostly opaque so markers under it stay quiet.
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, Active() || ImGui::GetTime() - m_lastInput < 3 ? 1.0f : .6f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
	const auto group = [&](const char* id, float x, float width, const auto& content)
	{
		ImGui::SetCursorScreenPos({x, y});
		ImGui::BeginChild(id, {width + frame, height}, ImGuiChildFlags_AlwaysUseWindowPadding,
						  ImGuiWindowFlags_NoScrollbar);
		const auto corner = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddRectFilled(corner, {corner.x + width + frame, corner.y + height},
												  theme::Fade(theme::Bg, .78f), theme::Radius * scale);
		content();
		hovered |= ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
		ImGui::EndChild();
	};
	const auto foldButton = [&]
	{
		if (IconButton("Fold HUD", Icon::ChevronDown, "Hide the pilot controls"))
			m_hudFolded = true;
	};
	if (folded)
	{
		// Folded away to one button; while captured, the way out stays readable beside it.
		const std::string release = KeyName(PilotAction::Escape) + " releases";
		const float width = rowHeight + (m_captured ? gap + textWidth(release.c_str()) : 0);
		group(
			"HUD strip", minimum.x + margin, width,
			[&]
			{
				if (IconButton("Show HUD", Icon::Keyboard,
							   m_hudFolded ? "Show the pilot controls" : "Widen the image to show the pilot controls"))
					m_hudFolded = false;
				if (m_captured)
				{
					ImGui::SameLine();
					ImGui::AlignTextToFramePadding();
					ImGui::TextDisabled("%s", release.c_str());
				}
			});
	}
	else
	{
		group("HUD target", minimum.x + margin, left,
			  [&]
			  {
				  ImGui::SetNextItemWidth(targetWidth);
				  // A small chevron like the other menus, instead of the combo's solid arrow box.
				  const bool open = ImGui::BeginCombo("##Pilot target", preview.c_str(), ImGuiComboFlags_NoArrowButton);
				  const auto box = ImGui::GetItemRectMax();
				  DrawIcon(ImGui::GetWindowDrawList(), Icon::ChevronDown,
						   {box.x - rowHeight / 2, box.y - rowHeight / 2}, rowHeight * .5f, theme::TextDim);
				  if (open)
				  {
					  for (std::size_t index = 0; index < std::size(s_targetNames); ++index)
					  {
						  const auto label = std::string{s_targetNames[index]} + "  (" + KeyName(s_targetKeys[index]) + ")";
						  if (ImGui::Selectable(label.c_str(), index == targetIndex))
							  m_target = static_cast<PilotTarget>(index);
					  }
					  ImGui::EndCombo();
				  }
				  ImGui::SetItemTooltip("What mouse look and walking keys move");
				  ImGui::SameLine();
				  const auto lockTip = "Keep the pointer captured without holding " + KeyName(PilotAction::Capture);
				  if (IconButton("Lock", m_locked ? Icon::Lock : Icon::Unlock, lockTip.c_str(), m_locked, lock))
					  m_locked = !m_locked;
				  ImGui::SameLine();
				  if (IconButton("Pilot keys", Icon::Keyboard, "Mouse, keyboard and gamepad bindings for piloting"))
					  ImGui::OpenPopup("Piloting");
				  if (ImGui::BeginPopup("Piloting"))
				  {
					  DrawHelp();
					  ImGui::EndPopup();
				  }
			  });
		const float middleWidth = middle + gap + chevron;
		const float middleX =
			std::max(minimum.x + margin + left + frame + gap, (minimum.x + maximum.x - middleWidth - frame) / 2);
		group("HUD hand", middleX, middleWidth,
			  [&]
			  {
				  auto* draw = ImGui::GetWindowDrawList();
				  if (ImGui::Button(m_hand ? "Buttons: Right" : "Buttons: Left"))
					  m_hand = 1 - m_hand;
				  ImGui::SetItemTooltip("Trigger, grip and buttons go to this hand; %s switches",
										KeyName(PilotAction::SwapHand).c_str());
				  const auto bar = [&](const char* label, float value)
				  {
					  ImGui::SameLine();
					  ImGui::AlignTextToFramePadding();
					  ImGui::TextUnformatted(label);
					  ImGui::SameLine(0, 4 * scale);
					  const auto position = ImGui::GetCursorScreenPos();
					  const ImVec2 size{36 * scale, 8 * scale};
					  ImGui::Dummy({size.x, rowHeight});
					  const ImVec2 top{position.x, position.y + (rowHeight - size.y) / 2};
					  draw->AddRectFilled(top, {top.x + size.x, top.y + size.y}, theme::Line, 2);
					  draw->AddRectFilled(top, {top.x + size.x * std::clamp(value, 0.0f, 1.0f), top.y + size.y},
										  theme::Accent, 2);
				  };
				  const auto pill = [&](const char* label, bool held)
				  {
					  ImGui::SameLine(0, 4 * scale);
					  const auto text = ImGui::CalcTextSize(label);
					  const auto position = ImGui::GetCursorScreenPos();
					  const ImVec2 size{text.x + 10 * scale, text.y + 4 * scale};
					  ImGui::Dummy({size.x, rowHeight});
					  const ImVec2 top{position.x, position.y + (rowHeight - size.y) / 2};
					  const ImVec2 bottom{top.x + size.x, top.y + size.y};
					  if (held)
						  draw->AddRectFilled(top, bottom, theme::Accent, size.y / 2);
					  else
						  draw->AddRect(top, bottom, theme::Line, size.y / 2);
					  draw->AddText({top.x + 5 * scale, top.y + 2 * scale}, held ? theme::Text : theme::TextDim, label);
				  };
				  const auto stick = [&](const ControllerControls& value, const char* tooltip)
				  {
					  ImGui::SameLine(0, 6 * scale);
					  const auto position = ImGui::GetCursorScreenPos();
					  const float radius = 8 * scale;
					  ImGui::Dummy({radius * 2 + 2, rowHeight});
					  ImGui::SetItemTooltip("%s", tooltip);
					  const ImVec2 center{position.x + radius + 1, position.y + rowHeight / 2};
					  draw->AddCircle(center, radius, value.thumbClick ? theme::Accent : theme::TextDim);
					  draw->AddCircleFilled({center.x + value.stick.x * radius, center.y - value.stick.y * radius},
											2.5f * scale, theme::Text);
				  };
				  if (conflict)
				  {
					  ImGui::SameLine();
					  if (ImGui::Button("Take over"))
					  {
						  m_rig.Observe(*observed);
						  ManualTransaction edit;
						  for (const auto channel : channels)
							  edit.commands.emplace_back(TakeControl{channel, true});
						  if (!input.Submit(std::move(edit)))
							  m_error = "Input queue full — release inputs";
					  }
					  ImGui::SetItemTooltip("Take these devices back from the replay or script that controls them");
				  }
				  else if (fullMiddle)
				  {
					  bar("Trigger", controls.trigger);
					  bar("Grip", controls.grip);
					  pill(m_hand ? "A" : "X", controls.primary);
					  pill(m_hand ? "B" : "Y", controls.secondary);
					  pill("Menu", controls.menu);
					  if (m_gamepadActive)
					  {
						  stick(m_controls[0], "Left thumbstick");
						  stick(m_controls[1], "Right thumbstick");
					  }
					  else
						  stick(controls, "Thumbstick");
				  }
				  ImGui::SameLine();
				  foldButton();
			  });
		if (showRight)
			group("HUD hint", maximum.x - margin - right - frame, right,
				  [&]
				  {
					  ImGui::AlignTextToFramePadding();
					  if (m_gamepadActive)
					  {
						  ImGui::TextColored(theme::Vec(theme::Ok), "Gamepad");
						  ImGui::SetItemTooltip("A gamepad is driving the selected hand");
						  ImGui::SameLine();
					  }
					  ImGui::TextDisabled("%s", hint.c_str());
				  });
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
	ImGui::SetCursorScreenPos({saved.x, saved.y - ImGui::GetStyle().ItemSpacing.y});
	ImGui::Dummy({0, 0});
	return hovered;
}
} // namespace simxr::app
