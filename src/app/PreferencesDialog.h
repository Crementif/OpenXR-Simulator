#pragma once

#include <cstdint>

#include "app/services/CompanionSettings.h"

namespace simxr::app
{
// The Preferences modal: UI scale, snapshot memory and the pilot keymap.
class PreferencesDialog
{
  public:
	// Returns true on the frame the dialog closes; marks dirty on any change.
	// keptBytes and keptBudget describe the preview history ring (0 budget hides it).
	bool Draw(CompanionSettings& settings, bool& open, bool& dirty, std::uint64_t keptBytes = 0,
			  std::uint64_t keptBudget = 0);

  private:
	void DrawKeyboard(CompanionSettings& settings, bool& dirty);
	void DrawGamepad(CompanionSettings& settings, bool& dirty);
	int m_capture = -1, m_captureFrame = 0;
};
} // namespace simxr::app
