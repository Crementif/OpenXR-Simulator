#include "app/platform/Window.h"
#include "Pch.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace simxr::app
{
Window::Window(bool hidden)
{
	constexpr auto subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
	if (!SDL_InitSubSystem(subsystems))
		throw std::runtime_error(std::string("SDL video/gamepad initialization failed: ") + SDL_GetError());
	try
	{
		const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
		if (!std::isfinite(scale) || scale <= 0 || scale > float(std::numeric_limits<int>::max() / 1280))
			throw std::runtime_error("SDL display content scale is invalid");
		SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		if (hidden)
			flags |= SDL_WINDOW_HIDDEN;
		m_window =
			SDL_CreateWindow("OpenXR Simulator", static_cast<int>(1280 * scale), static_cast<int>(800 * scale), flags);
		if (!m_window)
			throw std::runtime_error(std::string("SDL Vulkan window unavailable: ") + SDL_GetError());
	}
	catch (...)
	{
		SDL_QuitSubSystem(subsystems);
		throw;
	}
}

Window::~Window()
{
	m_feedback.Attach(nullptr);
	if (m_gamepad)
		SDL_CloseGamepad(m_gamepad);
	SDL_DestroyWindow(m_window);
	SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

SDL_Window* Window::Native() const
{
	return m_window;
}

std::pair<int, int> Window::PixelSize() const
{
	int width = 0, height = 0;
	if (!SDL_GetWindowSizeInPixels(m_window, &width, &height))
		throw std::runtime_error(SDL_GetError());
	return {width, height};
}

bool Window::PollEvents(const std::function<void(const SDL_Event&)>& processEvent)
{
	if (m_pointerReleasePending)
		CapturePointer(false);
	SDL_Event event;
	bool running = true;
	while (SDL_PollEvent(&event))
	{
		const bool capturedMotion = event.type == SDL_EVENT_MOUSE_MOTION &&
									event.motion.windowID == SDL_GetWindowID(m_window) && m_pointerCaptured;
		if (capturedMotion)
		{
			m_pointerMotion.first += event.motion.xrel;
			m_pointerMotion.second += event.motion.yrel;
		}
		if (event.type == SDL_EVENT_GAMEPAD_ADDED)
			OpenGamepad();
		if (event.type == SDL_EVENT_GAMEPAD_REMOVED && m_gamepad && SDL_GetGamepadID(m_gamepad) == event.gdevice.which)
		{
			m_feedback.Attach(nullptr);
			SDL_CloseGamepad(m_gamepad);
			m_gamepad = nullptr;
			OpenGamepad();
		}
		if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && event.window.windowID == SDL_GetWindowID(m_window))
		{
			m_focusLost = true;
			Rumble({});
			CapturePointer(false);
		}
		if (event.type == SDL_EVENT_QUIT ||
			(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(m_window)))
			running = false;
		if (!capturedMotion && processEvent)
			processEvent(event);
	}
	return running;
}

bool Window::Focused() const
{
	return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}
void Window::OpenGamepad()
{
	if (m_gamepad)
		return;
	int count = 0;
	auto ids = SDL_GetGamepads(&count);
	for (int index = 0; index < count && !m_gamepad; ++index)
		m_gamepad = SDL_OpenGamepad(ids[index]);
	SDL_free(ids);
	m_feedback.Attach(m_gamepad);
	m_gyroEnabled = m_gamepad && SDL_GamepadHasSensor(m_gamepad, SDL_SENSOR_GYRO) &&
					SDL_SetGamepadSensorEnabled(m_gamepad, SDL_SENSOR_GYRO, true);
}
GamepadSample Window::ReadGamepad(const GamepadBindings& bindings)
{
	OpenGamepad();
	GamepadSample result;
	if (!m_gamepad)
		return result;
	result.connected = true;
	const auto button = [&](const std::string& name)
	{
		const auto key = SDL_GetGamepadButtonFromString(name.c_str());
		return key != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(m_gamepad, key);
	};
	const auto axis = [&](const std::string& name)
	{
		const auto key = SDL_GetGamepadAxisFromString(name.c_str());
		if (key == SDL_GAMEPAD_AXIS_INVALID)
			return 0.0f;
		const float raw = std::clamp(SDL_GetGamepadAxis(m_gamepad, key) / 32767.0f, -1.0f, 1.0f);
		if (std::abs(raw) <= bindings.deadzone)
			return 0.0f;
		return std::copysign((std::abs(raw) - bindings.deadzone) / (1 - bindings.deadzone), raw);
	};
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		auto& controls = result.controls[hand];
		controls.stick = {axis(bindings.stickX[hand]), -axis(bindings.stickY[hand])};
		controls.trigger = std::max(0.0f, axis(bindings.trigger[hand]));
		controls.grip = button(bindings.grip[hand]) ? 1 : 0;
		controls.primary = button(bindings.primary[hand]);
		controls.secondary = button(bindings.secondary[hand]);
		controls.menu = button(bindings.menu[hand]);
		controls.thumbClick = button(bindings.stickClick[hand]);
	}
	result.dpad = {button("dpup"), button("dpdown"), button("dpleft"), button("dpright")};
	result.gyroActive = m_gyroEnabled && button(bindings.gyroHold) &&
						SDL_GamepadSensorEnabled(m_gamepad, SDL_SENSOR_GYRO) &&
						SDL_GetGamepadSensorData(m_gamepad, SDL_SENSOR_GYRO, result.gyro.data(), 3);
	return result;
}

std::uint32_t Window::GamepadIdentity() const noexcept
{
	return m_gamepad ? SDL_GetGamepadID(m_gamepad) : 0;
}
bool Window::HasGamepad() const noexcept
{
	return m_gamepad != nullptr;
}
bool Window::Rumble(std::array<std::uint16_t, 2> motors, std::chrono::steady_clock::time_point deadline)
{
	return m_feedback.Rumble(motors, deadline);
}

bool Window::TakeFocusLost()
{
	return std::exchange(m_focusLost, false);
}

bool Window::CapturePointer(bool enabled)
{
	if (!enabled || m_pointerReleasePending)
		m_pointerMotion = {};
	m_pointerReleasePending = !enabled && m_pointerCaptured;
	if (m_pointerCaptured == enabled)
		return true;
	if (!SDL_SetWindowRelativeMouseMode(m_window, enabled))
		return false;
	m_pointerCaptured = enabled;
	m_pointerReleasePending = false;
	m_pointerMotion = {};
	return true;
}
std::pair<float, float> Window::TakePointerMotion()
{
	return std::exchange(m_pointerMotion, {});
}

void Window::Resize(int width, int height)
{
	if (!SDL_SetWindowSize(m_window, width, height))
		throw std::runtime_error(SDL_GetError());
}
} // namespace simxr::app
