#include "app/PreferencesDialog.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <utility>

namespace simxr::app
{
bool PreferencesDialog::Draw(CompanionSettings& settings, bool& open, bool& dirty, std::uint64_t keptBytes,
							 std::uint64_t keptBudget)
{
	if (open)
		ImGui::OpenPopup("Workspace settings");
	ImGui::SetNextWindowSize({620, 500}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints({400, 260}, ImGui::GetMainViewport()->WorkSize);
	if (!ImGui::BeginPopupModal("Workspace settings", &open))
		return false;
	if (ImGui::SliderFloat("UI scale", &settings.scale, 0.75f, 2.5f, "%.2f"))
		dirty = true;
	int budget = static_cast<int>(settings.snapshotBudgetMiB);
	if (ImGui::SliderInt("Snapshots (MiB)", &budget, SnapshotBudget::MinMiB, SnapshotBudget::MaxMiB))
	{
		settings.snapshotBudgetMiB = static_cast<std::uint32_t>(budget);
		dirty = true;
	}
	if (keptBudget)
	{
		char usage[48];
		std::snprintf(usage, sizeof(usage), "%llu / %llu MiB", static_cast<unsigned long long>(keptBytes >> 20),
					  static_cast<unsigned long long>(keptBudget >> 20));
		ImGui::ProgressBar(static_cast<float>(keptBytes) / static_cast<float>(keptBudget),
						   {ImGui::CalcItemWidth(), 0}, usage);
		ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextUnformatted("Kept previews");
		ImGui::SetItemTooltip("Downscaled copies of shown frames for looking back while the app runs");
	}
	if (ImGui::BeginTabBar("Preference pages"))
	{
		if (ImGui::BeginTabItem("Keyboard & mouse"))
		{
			DrawKeyboard(settings, dirty);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Gamepad"))
		{
			DrawGamepad(settings, dirty);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	bool closed = false;
	if (ImGui::Button("Close"))
	{
		open = false;
		closed = true;
		m_capture = -1;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
	return closed;
}
void PreferencesDialog::DrawKeyboard(CompanionSettings& settings, bool& dirty)
{
	if (ImGui::Button("Reset keymap"))
	{
		settings.pilotBindings = DefaultPilotBindings();
		dirty = true;
		m_capture = -1;
	}
	if (ImGui::BeginTable("Bindings", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, {0, 320}))
	{
		ImGui::TableSetupColumn("Action");
		ImGui::TableSetupColumn("Binding");
		ImGui::TableHeadersRow();
		for (std::size_t index = 0; index < settings.pilotBindings.size(); ++index)
		{
			ImGui::PushID(static_cast<int>(index));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(PilotActionNames[index]);
			ImGui::TableNextColumn();
			const auto label =
				m_capture == static_cast<int>(index) ? "Press a key…" : PilotBindingLabel(settings.pilotBindings[index]);
			if (ImGui::Button(label.c_str(), {-1, 0}))
			{
				m_capture = static_cast<int>(index);
				m_captureFrame = ImGui::GetFrameCount();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (m_capture < 0 || ImGui::GetFrameCount() <= m_captureFrame + 1)
		return;
	if (ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		m_capture = -1;
		return;
	}
	for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
	{
		if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown)
			continue;
		if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
			continue;
		const auto& io = ImGui::GetIO();
		auto& binding = settings.pilotBindings[static_cast<std::size_t>(m_capture)];
		binding.key = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
		binding.modifiers = (io.KeyCtrl ? 1 : 0) | (io.KeyShift ? 2 : 0) | (io.KeyAlt ? 4 : 0) | (io.KeySuper ? 8 : 0);
		m_capture = -1;
		dirty = true;
		break;
	}
}
void PreferencesDialog::DrawGamepad(CompanionSettings& settings, bool& dirty)
{
	auto& bindings = settings.gamepadBindings;
	dirty |= ImGui::SliderFloat("Deadzone", &bindings.deadzone, 0, .5f, "%.2f");
	const auto mapping = [&](const char* label, std::string& name, bool axis)
	{
		if (!ImGui::BeginCombo(label, name.c_str()))
			return;
		for (int index = 0; index < (axis ? int(SDL_GAMEPAD_AXIS_COUNT) : int(SDL_GAMEPAD_BUTTON_COUNT)); ++index)
		{
			const char* value = axis ? SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(index))
									 : SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(index));
			if (ImGui::Selectable(value, name == value))
			{
				name = value;
				dirty = true;
			}
		}
		ImGui::EndCombo();
	};
	if (ImGui::BeginTable("Gamepad mapping", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, {0, 300}))
	{
		ImGui::TableSetupColumn("Input");
		ImGui::TableSetupColumn("Left");
		ImGui::TableSetupColumn("Right");
		ImGui::TableHeadersRow();
		int row = 0;
		for (const auto& [label, entries] :
			 {std::pair{"Stick X", &bindings.stickX}, {"Stick Y", &bindings.stickY}, {"Trigger", &bindings.trigger},
			  {"Grip", &bindings.grip}, {"Primary", &bindings.primary}, {"Secondary", &bindings.secondary},
			  {"Menu", &bindings.menu}, {"Stick click", &bindings.stickClick}})
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::PushID(row);
			for (int hand = 0; hand < 2; ++hand)
			{
				ImGui::TableNextColumn();
				ImGui::PushID(hand);
				ImGui::SetNextItemWidth(-1);
				mapping("##Mapping", (*entries)[hand], row < 3);
				ImGui::PopID();
			}
			ImGui::PopID();
			++row;
		}
		ImGui::EndTable();
	}
	mapping("Hold for gyro", bindings.gyroHold, false);
	if (ImGui::Button("Reset gamepad"))
	{
		bindings = {};
		dirty = true;
	}
}
} // namespace simxr::app
