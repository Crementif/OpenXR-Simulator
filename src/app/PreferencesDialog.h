#pragma once

#include "app/CompanionSettings.h"

namespace simxr::app
{
// The Preferences modal: UI scale, snapshot memory and the pilot keymap.
class PreferencesDialog
{
  public:
	// Returns true on the frame the dialog closes; marks dirty on any change.
	bool Draw(CompanionSettings& settings, bool& open, bool& dirty);

  private:
	void DrawKeyboard(CompanionSettings& settings, bool& dirty);
	void DrawGamepad(CompanionSettings& settings, bool& dirty);
	int m_capture = -1, m_captureFrame = 0;
};
} // namespace simxr::app
