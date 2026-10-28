#pragma once

#include "app/services/CompanionSettings.h"
#include "app/mcp/McpUiState.h"
#include "app/services/UpdateChecker.h"

#include <cstdint>
#include <imgui.h>

namespace simxr::app
{
// The Preferences modal: UI scale, snapshot memory and the pilot keymap.
class PreferencesDialog
{
  public:
	// Returns true on the frame the dialog closes; marks dirty on any change.
	// keptBytes and keptBudget describe the preview history ring (0 budget hides it).
	bool Draw(CompanionSettings& settings, bool& open, bool& dirty, std::uint64_t keptBytes = 0,
			  std::uint64_t keptBudget = 0, std::uint64_t ramBytes = 0, std::uint64_t ramBudget = 0,
			  const std::string& historyError = {}, std::uint64_t tabSnapshots = 0, std::uint64_t allSnapshots = 0,
			  McpUiState* mcp = nullptr, UpdateUiState* update = nullptr);

  private:
	bool DrawKeyboard(CompanionSettings& settings, bool& dirty);
	void DrawGamepad(CompanionSettings& settings, bool& dirty);
	void CaptureKey(CompanionSettings& settings, bool& dirty, bool overBinding);
	void CancelCapture();
	int m_capture = -1, m_captureFrame = 0, m_lastFrame = -1;
	ImGuiKey m_modifier = ImGuiKey_None;
	bool m_escapeBlocked = false;
	int m_mcpClient = 0;
	std::string m_clipboardError;
	std::string m_urlError;
};
} // namespace simxr::app
