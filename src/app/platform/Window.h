#pragma once

#include "app/platform/GamepadFeedback.h"
#include "app/services/PilotBindings.h"
#include "simulation/InputState.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

struct SDL_Window;
struct SDL_Gamepad;
union SDL_Event;

namespace simxr::app
{
struct GamepadSample
{
	bool connected = false, gyroActive = false;
	std::array<ControllerControls, 2> controls;
	std::array<float, 3> gyro{};
	std::array<bool, 4> dpad{};
};
class Window
{
  public:
	explicit Window(bool hidden);
	~Window();
	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;
	SDL_Window* Native() const;
	std::pair<int, int> PixelSize() const;
	// Pilot motion stays with the pilot; other events go to the UI owner.
	bool PollEvents(const std::function<void(const SDL_Event&)>& processEvent = {});
	bool TakeFocusLost();
	bool Focused() const;
	GamepadSample ReadGamepad(const GamepadBindings& bindings);
	bool Rumble(std::array<std::uint16_t, 2> motors,
				std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
	bool HasGamepad() const noexcept;
	std::uint32_t GamepadIdentity() const noexcept;
	void Resize(int width, int height);
	bool CapturePointer(bool enabled, bool relative = true);
	std::pair<float, float> TakePointerMotion();

  private:
	SDL_Window* m_window = nullptr;
	SDL_Gamepad* m_gamepad = nullptr;
	GamepadFeedback m_feedback;
	void OpenGamepad();
	bool m_focusLost = false;
	bool m_pointerCaptured = false;
	bool m_pointerRelative = false;
	bool m_pointerReleasePending = false;
	bool m_gyroEnabled = false;
	std::pair<float, float> m_pointerMotion{};
};
} // namespace simxr::app
