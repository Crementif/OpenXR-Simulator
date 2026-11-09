#include "app/dialogs/PreferencesDialog.h"
#include "app/UiPch.h"
#include "simxr/Version.h"
#include "app/platform/PackageDocumentation.h"

namespace simxr::app
{
bool PreferencesDialog::Draw(CompanionSettings& settings, bool& open, bool& dirty, std::uint64_t keptBytes,
							 std::uint64_t keptBudget, std::uint64_t ramBytes, std::uint64_t ramBudget,
							 const std::string& historyError, std::uint64_t tabSnapshots, std::uint64_t allSnapshots,
							 McpUiState* mcp, UpdateUiState* update)
{
	const bool wasOpen = open;
	const auto& io = ImGui::GetIO();
	const int frame = ImGui::GetFrameCount();
	if (!open || io.AppFocusLost || frame > m_lastFrame + 1)
	{
		CancelCapture();
		m_escapeBlocked = false;
	}
	m_lastFrame = frame;
	if (open)
		ImGui::OpenPopup("Workspace settings");
	const auto* viewport = ImGui::GetMainViewport();
	const auto available = viewport->WorkSize;
	ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, {.5f, .5f});
	ImGui::SetNextWindowSize({std::min(620 * settings.scale, available.x), std::min(650 * settings.scale, available.y)},
							 ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(
		{std::min(400 * settings.scale, available.x), std::min(260 * settings.scale, available.y)}, available);
	if (!ImGui::BeginPopupModal("Workspace settings", &open, ImGuiWindowFlags_NoMove))
	{
		CancelCapture();
		m_escapeBlocked = false;
		return wasOpen && !open;
	}
	const bool focused = !io.AppFocusLost && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	bool keyboardVisible = false, overBinding = false;
	const float contentHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
	if (ImGui::BeginChild("Preference contents", {0, contentHeight}))
	{
		if (ImGui::BeginTabBar("Preference pages", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem("General"))
			{
				const float labelWidth =
					std::max(ImGui::CalcTextSize("History GPU (MiB)").x, ImGui::CalcTextSize("Kept previews").x);
				ImGui::PushItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - labelWidth -
													   ImGui::GetStyle().ItemInnerSpacing.x));
				if (ImGui::SliderFloat("UI scale", &settings.scale, 0.75f, 2.5f, "%.2f"))
					dirty = true;
				if (ImGui::Checkbox("Open a tab for each new app", &settings.openNewSessions))
					dirty = true;
				ImGui::SetItemTooltip("Connect to OpenXR apps that start while the simulator runs.\n"
									  "An empty tab takes the app; otherwise it opens in a new tab.");
				int budget = static_cast<int>(settings.snapshotBudgetMiB);
				if (ImGui::SliderInt("Snapshots (MiB)", &budget, SnapshotBudget::MinMiB, SnapshotBudget::MaxMiB))
				{
					settings.snapshotBudgetMiB = static_cast<std::uint32_t>(budget);
					dirty = true;
				}
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextWrapped("App snapshots: %llu MiB. All apps: %llu / %u MiB",
								   static_cast<unsigned long long>(tabSnapshots >> 20),
								   static_cast<unsigned long long>(allSnapshots >> 20), settings.snapshotBudgetMiB);
				ImGui::PopStyleColor();
				ImGui::SetItemTooltip(
					"Estimated memory reserved for snapshot copies, including copies still retiring.");
				int history = static_cast<int>(settings.previewBudgetMiB);
				if (ImGui::InputInt("History GPU (MiB)", &history, 64, 256))
				{
					settings.previewBudgetMiB =
						static_cast<std::uint32_t>(
							std::clamp(history, int(FramePreviewHistory::MinMiB), int(FramePreviewHistory::MaxMiB))) /
						4 * 4;
					dirty = true;
				}
				ImGui::SetItemTooltip("Memory for recent frames: 64 to 16384 MiB. Changes are saved automatically.\n"
									  "About one stereo preview per 4 MiB. Timeline thumbnails use another 32 MiB.");
				int ram = static_cast<int>(settings.previewRamMiB);
				if (ImGui::InputInt("History RAM (MiB)", &ram, 64, 256))
				{
					settings.previewRamMiB = static_cast<std::uint32_t>(std::clamp(ram, 64, 16384)) / 4 * 4;
					dirty = true;
				}
				ImGui::SetItemTooltip("Older frames are compressed with H.264 in RAM. When memory fills, "
									  "the oldest frames are removed. Changes are saved automatically.");
				if (keptBudget)
				{
					const auto usage =
						std::to_string(keptBytes >> 20) + " / " + std::to_string(keptBudget >> 20) + " MiB";
					ImGui::ProgressBar(static_cast<float>(keptBytes) / static_cast<float>(keptBudget),
									   {ImGui::CalcItemWidth(), 0}, usage.c_str());
					ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
					ImGui::TextUnformatted("GPU used");
					ImGui::SetItemTooltip("Keep every frame until memory fills, then remove the oldest.");
				}
				if (ramBudget)
				{
					const auto usage =
						std::to_string(ramBytes >> 20) + " / " + std::to_string(ramBudget >> 20) + " MiB";
					ImGui::ProgressBar(static_cast<float>(ramBytes) / static_cast<float>(ramBudget),
									   {ImGui::CalcItemWidth(), 0}, usage.c_str());
					ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
					ImGui::TextUnformatted("RAM used");
				}
				if (!historyError.empty())
					ImGui::TextWrapped("History: %s", historyError.c_str());
				ImGui::PopItemWidth();
				if (ImGui::Button("Reset layout"))
				{
					ResetLayout(settings);
					dirty = true;
				}
				ImGui::SetItemTooltip(
					"Restore panel sizes and visibility.\n"
					"Hide the timeline with Ctrl+T, the inspector with Ctrl+I. F11 shows only the image.");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Keys & mouse"))
			{
				keyboardVisible = true;
				overBinding = DrawKeyboard(settings, dirty);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Gamepad"))
			{
				DrawGamepad(settings, dirty);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("MCP"))
			{
				if (!mcp)
					ImGui::TextWrapped("MCP controls are unavailable in this session.");
				else
					dirty |= m_mcp.Draw(settings, *mcp);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("About"))
			{
				ImGui::TextUnformatted("OpenXR Simulator");
				ImGui::Text("Version %s", simxr::Version);
				ImGui::TextWrapped("A launcher and simulator for OpenXR applications.");
				ImGui::TextWrapped("MIT license. Bundled dependencies retain their own licenses.");
				if (ImGui::Button("Documentation"))
					m_urlError = OpenPackageDocument(PackageDocument::Guide);
				const auto right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
				const auto licenseWidth = ImGui::CalcTextSize("Licenses").x + 2 * ImGui::GetStyle().FramePadding.x;
				if (right - ImGui::GetItemRectMax().x >= licenseWidth + ImGui::GetStyle().ItemSpacing.x)
					ImGui::SameLine();
				if (ImGui::Button("Licenses"))
					m_urlError = OpenPackageDocument(PackageDocument::Licenses);
				if (ValidReleaseRepository(simxr::ReleaseRepository))
				{
					if (ImGui::Button("Releases"))
					{
						const auto url = "https://github.com/" + std::string(simxr::ReleaseRepository) + "/releases";
						m_urlError = SDL_OpenURL(url.c_str()) ? "" : SDL_GetError();
					}
				}
				else
					ImGui::TextWrapped("Release repository is not configured for this build.");
				if (ImGui::Checkbox("Check for updates at startup", &settings.autoUpdateCheck))
					dirty = true;
				ImGui::SetItemTooltip("Contact the configured GitHub repository when the simulator starts.");
				if (!update)
					ImGui::TextWrapped("Update checks are unavailable in this session.");
				else
				{
					ImGui::BeginDisabled(update->result.status == UpdateStatus::Checking);
					if (ImGui::Button("Check for updates"))
						update->checkRequested = true;
					ImGui::EndDisabled();
					switch (update->result.status)
					{
					case UpdateStatus::Idle:
						break;
					case UpdateStatus::Checking:
						ImGui::TextUnformatted("Checking for updates…");
						break;
					case UpdateStatus::Current:
						ImGui::TextUnformatted("No newer release found.");
						break;
					case UpdateStatus::Available:
						ImGui::Text("Version %s is available.", update->result.version.c_str());
						if (ImGui::Button("Open release"))
							m_urlError = SDL_OpenURL(update->result.releaseUrl.c_str()) ? "" : SDL_GetError();
						break;
					case UpdateStatus::Unavailable:
						ImGui::TextWrapped("Update check unavailable: %s", update->result.error.c_str());
						break;
					}
				}
				if (!m_urlError.empty())
					ImGui::TextWrapped("Cannot open link: %s", m_urlError.c_str());
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::EndChild();
	if (!keyboardVisible || !focused)
		CancelCapture();
	const bool escape = focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	const bool cancelCapture = escape && m_capture >= 0;
	if (cancelCapture)
		CancelCapture();
	bool closed = false;
	if (ImGui::Button("Close") || (escape && !cancelCapture && !m_escapeBlocked))
	{
		open = false;
		closed = true;
		CancelCapture();
		ImGui::CloseCurrentPopup();
	}
	if (!closed)
		CaptureKey(settings, dirty, overBinding);
	// Escape first belongs to an active field or nested popup.
	m_escapeBlocked = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
	ImGui::EndPopup();
	return closed;
}

bool PreferencesDialog::DrawKeyboard(CompanionSettings& settings, bool& dirty)
{
	if (ImGui::Checkbox("Remote desktop mouse drag", &settings.remoteMouseDrag))
		dirty = true;
	ImGui::SetItemTooltip("For remote desktop: hold the capture button and drag to move the view.\n"
						  "Release and reposition at the edge. Esc or leaving the window releases inputs.");
	if (ImGui::Button("Reset keymap"))
	{
		settings.pilotBindings = DefaultPilotBindings();
		dirty = true;
		CancelCapture();
	}
	bool overBinding = false;
	const float height = std::max(3 * ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y);
	if (ImGui::BeginTable("Bindings", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, {0, height}))
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
				(m_capture == static_cast<int>(index) ? std::string{"Press a key…"}
													  : PilotBindingLabel(settings.pilotBindings[index])) +
				"###Binding";
			if (ImGui::Button(label.c_str(), {-1, 0}))
			{
				m_capture = static_cast<int>(index);
				m_captureFrame = ImGui::GetFrameCount();
				m_modifier = ImGuiKey_None;
			}
			if (m_capture == static_cast<int>(index))
				overBinding = ImGui::IsItemHovered() || ImGui::IsItemActive();
			ImGui::SetItemTooltip("Press a key or mouse button. Hold Ctrl, Shift or Alt to combine keys. "
								  "Release a modifier to bind it alone. Esc cancels.");
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	return overBinding;
}

void PreferencesDialog::DrawGamepad(CompanionSettings& settings, bool& dirty)
{
	auto& bindings = settings.gamepadBindings;
	ImGui::PushItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Hold for gyro").x -
										   ImGui::GetStyle().ItemInnerSpacing.x));
	dirty |= ImGui::SliderFloat("Deadzone", &bindings.deadzone, 0, .5f, "%.2f");
	const auto mapping = [&](const char* label, std::string& name, bool axis) {
		if (!ImGui::BeginCombo(label, name.c_str()))
			return;
		for (int index = 0; index < (axis ? int(SDL_GAMEPAD_AXIS_COUNT) : int(SDL_GAMEPAD_BUTTON_COUNT)); ++index)
		{
			const char* value = axis ? SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(index))
									 : SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(index));
			if (!value)
				continue;
			if (ImGui::Selectable(value, name == value))
			{
				name = value;
				dirty = true;
			}
		}
		ImGui::EndCombo();
	};
	mapping("Hold for gyro", bindings.gyroHold, false);
	ImGui::PopItemWidth();
	if (ImGui::Button("Reset gamepad"))
	{
		bindings = {};
		dirty = true;
	}
	const float height = std::max(3 * ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y);
	if (ImGui::BeginTable("Gamepad mapping", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, {0, height}))
	{
		ImGui::TableSetupColumn("Input");
		ImGui::TableSetupColumn("Left");
		ImGui::TableSetupColumn("Right");
		ImGui::TableHeadersRow();
		int row = 0;
		for (const auto& [label, entries] : {std::pair{"Stick X", &bindings.stickX},
											 {"Stick Y", &bindings.stickY},
											 {"Trigger", &bindings.trigger},
											 {"Grip", &bindings.grip},
											 {"Primary", &bindings.primary},
											 {"Secondary", &bindings.secondary},
											 {"Menu", &bindings.menu},
											 {"Stick click", &bindings.stickClick}})
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
}

void PreferencesDialog::CaptureKey(CompanionSettings& settings, bool& dirty, bool overBinding)
{
	if (m_capture < 0 || ImGui::GetFrameCount() <= m_captureFrame)
		return;
	const auto& io = ImGui::GetIO();
	bool mousePressed = false;
	for (int button = 0; button < ImGuiMouseButton_COUNT; ++button)
		mousePressed |= ImGui::IsMouseClicked(button);
	if (io.MouseWheel != 0 || io.MouseWheelH != 0 ||
		(!overBinding && (ImGui::IsAnyItemActive() || (mousePressed && ImGui::IsAnyItemHovered()))))
	{
		CancelCapture();
		return;
	}
	const unsigned modifiers =
		(io.KeyCtrl ? 1u : 0u) | (io.KeyShift ? 2u : 0u) | (io.KeyAlt ? 4u : 0u) | (io.KeySuper ? 8u : 0u);
	ImGuiKey captured = ImGuiKey_None;
	for (int index = ImGuiKey_NamedKey_BEGIN; index <= ImGuiKey_MouseX2; ++index)
	{
		const auto key = static_cast<ImGuiKey>(index);
		if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown)
			continue;
		if (!ImGui::IsKeyPressed(key, false))
			continue;
		if (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper)
		{
			m_modifier = key;
			continue;
		}
		captured = key;
		break;
	}
	if (captured == ImGuiKey_None && m_modifier != ImGuiKey_None && ImGui::IsKeyReleased(m_modifier))
		captured = m_modifier;
	if (captured == ImGuiKey_None)
		return;
	PilotBinding binding{ImGui::GetKeyName(captured), modifiers};
	if (captured >= ImGuiKey_LeftCtrl && captured <= ImGuiKey_RightSuper)
		binding.modifiers &= ~(1u << ((captured - ImGuiKey_LeftCtrl) % 4));
	auto& previous = settings.pilotBindings[static_cast<std::size_t>(m_capture)];
	dirty |= previous != binding;
	previous = std::move(binding);
	CancelCapture();
}

void PreferencesDialog::CancelCapture()
{
	m_capture = -1;
	m_modifier = ImGuiKey_None;
}
} // namespace simxr::app
