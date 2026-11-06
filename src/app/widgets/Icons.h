#pragma once

#include <imgui.h>

namespace simxr::app
{
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
	Point,
};
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color);
} // namespace simxr::app
