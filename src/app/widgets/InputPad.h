#pragma once

#include <imgui.h>

namespace simxr::app
{
// Mouse-operable controller inputs (guide §8). Each returns true when its value
// changed this frame. A latched input keeps its value after the mouse lets go;
// right-click toggles the latch.
// Idle and disabled widgets preserve observed values. The caller releases its
// held inputs when hiding the pad or revoking input ownership.

bool TriggerBar(const char* id, const char* label, float& value, bool& latched, ImU32 color, float width);
bool HoldButton(const char* id, const char* label, bool& pressed, bool& latched, ImU32 color);
bool StickPad(const char* id, float& x, float& y, bool& click, bool& latched, ImU32 color);
} // namespace simxr::app
