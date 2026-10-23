#include "../TestSupport.h"
#include "app/platform/Window.h"

#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

static bool s_rejectRelease = false;
extern "C" bool __real_SDL_SetWindowRelativeMouseMode(SDL_Window*, bool);
extern "C" bool __wrap_SDL_SetWindowRelativeMouseMode(SDL_Window* window, bool enabled)
{
	if (!enabled && std::exchange(s_rejectRelease, false))
		return SDL_SetError("Injected native pointer release failure");
	return __real_SDL_SetWindowRelativeMouseMode(window, enabled);
}

static void CheckEvents(simxr::app::Window& window)
{
	Require(window.PollEvents(), "Native events without an ImGui context failed");
	SDL_Event focus{};
	focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	focus.window.windowID = SDL_GetWindowID(window.Native());
	Require(SDL_PushEvent(&focus), "Queue native focus event");
	unsigned observed = 0;
	Require(window.PollEvents(
				[&](const SDL_Event& event)
				{
					observed += event.type == SDL_EVENT_WINDOW_FOCUS_LOST;
				}),
			"Focus event closed the window");
	Require(observed == 1 && window.TakeFocusLost() && !window.TakeFocusLost(),
			"Focus observation was lost or repeated");

	Require(window.CapturePointer(true), "Enable relative pointer mode on isolated hidden window");
	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.windowID = SDL_GetWindowID(window.Native());
	motion.motion.xrel = 5;
	motion.motion.yrel = -3;
	Require(SDL_PushEvent(&motion), "Queue captured motion");
	unsigned forwarded = 0;
	auto observer = [&](const SDL_Event& event)
	{
		forwarded += event.type == SDL_EVENT_MOUSE_MOTION;
	};
	Require(window.PollEvents(observer) && forwarded == 0 && window.TakePointerMotion() == std::pair{5.f, -3.f},
			"Pilot motion was lost or forwarded to ordinary UI input");
	s_rejectRelease = true;
	Require(!window.CapturePointer(false) && SDL_GetWindowRelativeMouseMode(window.Native()),
			"Native release failure was not exercised");
	Require(SDL_PushEvent(&motion), "Queue motion during release retry");
	Require(window.PollEvents(observer) && !SDL_GetWindowRelativeMouseMode(window.Native()) && forwarded != 0 &&
				window.TakePointerMotion() == std::pair{0.f, 0.f},
			"Failed pointer release permanently swallowed UI motion");
}

static void CheckGyro(simxr::app::Window& window)
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_VirtualJoystickSensorDesc sensor{SDL_SENSOR_GYRO, 100};
	SDL_VirtualJoystickDesc description{};
	SDL_INIT_INTERFACE(&description);
	description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
	description.nsensors = 1;
	description.sensors = &sensor;
	description.name = "SimXR rejected gyro fixture";
	unsigned attempts = 0;
	description.userdata = &attempts;
	description.SetSensorsEnabled = [](void* data, bool enabled)
	{
		if (enabled)
		{
			++*static_cast<unsigned*>(data);
			return SDL_SetError("Injected gyro enable failure");
		}
		return true;
	};
	const auto id = SDL_AttachVirtualJoystick(&description);
	Require(id != 0, "Attach virtual gyro controller");
	struct Joystick
	{
		SDL_JoystickID id;
		SDL_Joystick* handle = nullptr;
		~Joystick()
		{
			if (handle)
				SDL_CloseJoystick(handle);
			SDL_DetachVirtualJoystick(id);
		}
	} joystick{id, SDL_OpenJoystick(id)};
	Require(joystick.handle, "Open virtual joystick");
	window.PollEvents();
	Require(window.GamepadIdentity() == id, "Fixture did not own the selected gamepad");
	simxr::app::GamepadBindings bindings;
	bindings.gyroHold = "a";
	Require(SDL_SetJoystickVirtualButton(joystick.handle, SDL_GAMEPAD_BUTTON_SOUTH, true), "Hold gyro input");
	SDL_UpdateJoysticks();
	const auto sample = window.ReadGamepad(bindings);
	Require(attempts == 1 && sample.connected && sample.controls[1].primary && !sample.gyroActive,
			"Failed sensor enable allowed cached gyro data to drive hand poses");
}

int main()
{
	try
	{
		constexpr auto subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
		Require(SDL_InitSubSystem(subsystems), "Initialize independent SDL owner");
		struct Quit
		{
			~Quit()
			{
				SDL_Quit();
			}
		} quit;
		{
			simxr::app::Window window(true);
			CheckEvents(window);
			CheckGyro(window);
		}
		Require((SDL_WasInit(subsystems) & subsystems) == subsystems,
				"Window teardown terminated another owner's SDL subsystems");
		// A failed Vulkan window creation must also balance its SDL references.
		SDL_QuitSubSystem(subsystems);
		Require(SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE),
				"Select isolated failing Vulkan surface driver");
		Require(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "Initialize independent gamepad owner");
		bool rejected = false;
		try
		{
			simxr::app::Window unavailable(true);
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected && SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_WasInit(SDL_INIT_GAMEPAD) != 0,
				"Failed window creation leaked video ownership or terminated another gamepad owner");
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
		std::cout << "PASS native window event ownership, SDL lifetime, pointer release retry and rejected gyro\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
