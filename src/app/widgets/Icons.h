#pragma once

#include <imgui.h>

namespace simxr::app
{
// Icons are drawn as vector strokes so they scale with the UI and need no
// font file. They always accompany a word or a tooltip.
enum class Icon
{
	Play,
	Pause,
	StepForward,
	ChevronLeft,
	ChevronRight,
	Record,
	Stop,
	Library,
	More,
	Settings,
	Help,
	Globe,
	Axes,
	App,
	Refresh,
	Close,
	Player,
	Head,
	HandLeft,
	HandRight,
	Body,
	Eye,
	Tracker,
	Headset,
	Scenarios,
	Pin,
	Export,
	Live,
	Warning,
};
// Draws an icon centred on center inside a square of the given side length.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color);
} // namespace simxr::app
