#pragma once
#include <array>
#include <cstddef>
#include <string>

namespace simxr::app
{
enum class PilotAction
{
	Capture,
	Forward,
	Back,
	Left,
	Right,
	Down,
	Up,
	Fast,
	Slow,
	FreeLook,
	ReachLeft,
	ReachRight,
	Trigger,
	Grip,
	Primary,
	Secondary,
	Menu,
	StickUp,
	StickDown,
	StickLeft,
	StickRight,
	StickClick,
	SwapHand,
	TargetHead,
	TargetLeft,
	TargetRight,
	TargetPlayer,
	Escape,
	Count
};
inline constexpr std::array<const char*, static_cast<std::size_t>(PilotAction::Count)> PilotActionNames{
	"Capture",
	"Walk forward",
	"Walk back",
	"Walk left",
	"Walk right",
	"Lower head",
	"Raise head",
	"Fast",
	"Slow",
	"Free look / rotate hand",
	"Reach left",
	"Reach right",
	"Trigger",
	"Grip",
	"Primary",
	"Secondary",
	"Menu",
	"Stick up",
	"Stick down",
	"Stick left",
	"Stick right",
	"Stick click",
	"Swap input hand",
	"Pilot head",
	"Pilot left",
	"Pilot right",
	"Pilot player",
	"End capture"};

struct PilotBinding
{
	std::string key;
	unsigned modifiers = 0; // Ctrl=1, Shift=2, Alt=4, Super=8.
	bool operator==(const PilotBinding&) const = default;
};

std::string PilotBindingLabel(const PilotBinding& binding);
using PilotBindings = std::array<PilotBinding, static_cast<std::size_t>(PilotAction::Count)>;
PilotBindings DefaultPilotBindings();

struct GamepadBindings
{
	// SDL's stable mapping names, independent of the controller's hardware layout.
	std::array<std::string, 2> stickX{"leftx", "rightx"}, stickY{"lefty", "righty"},
		trigger{"lefttrigger", "righttrigger"};
	std::array<std::string, 2> grip{"leftshoulder", "rightshoulder"}, primary{"x", "a"}, secondary{"y", "b"},
		menu{"back", "start"}, stickClick{"leftstick", "rightstick"};
	std::string gyroHold = "touchpad";
	float deadzone = .15f;
	bool operator==(const GamepadBindings&) const = default;
};
} // namespace simxr::app
