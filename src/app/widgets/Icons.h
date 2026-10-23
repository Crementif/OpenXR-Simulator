#pragma once

#include <imgui.h>

namespace simxr::app
{
// Icons come from the SVG set in resources/icons, compiled into the binary and
// drawn as tinted vector paths so they scale with the UI. They always
// accompany a word or a tooltip.
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
	Rename,
	Delete,
	Script,
	StepBack,
	ChevronUp,
	ChevronDown,
	Focus,
	Crop,
	Zoom,
	Gauge,
	Lock,
	Unlock,
	ResetPose,
	Keyboard,
	History,
	Fullscreen,
};
// Draws an icon centred on center inside a square of the given side length.
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color);
} // namespace simxr::app
