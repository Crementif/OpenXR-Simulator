#pragma once

#include <imgui.h>

namespace simxr::app
{
// Mouse-operable controller inputs (guide §8). Each returns true when its value
// changed this frame. A latched input keeps its value after the mouse lets go;
// right-click toggles the latch.

// Horizontal analog bar: drag sets the value, click-and-hold is fully pressed.
// Right-click latches at the value under the mouse.
bool TriggerBar(const char* id, const char* label, float& value, bool& latched, ImU32 color, float width);
// Round button pressed while the mouse holds it.
bool HoldButton(const char* id, const char* label, bool& pressed, bool& latched, ImU32 color);
// Thumbstick: an arrow pushes the stick fully that way while held, as for menu
// navigation; dragging steers it freely. It springs back unless latched.
// Clicking the knob without dragging presses the stick.
bool StickPad(const char* id, float& x, float& y, bool& click, bool& latched, ImU32 color);
} // namespace simxr::app
