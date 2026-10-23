#pragma once

#include "app/services/PilotBindings.h"
#include "simulation/InputState.h"
#include <utility>

struct SDL_Window;
struct SDL_Gamepad;

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
	bool PollEvents();
	bool TakeFocusLost();
	bool Focused() const;
	GamepadSample ReadGamepad(const GamepadBindings& bindings);
	void Resize(int width, int height);
	bool CapturePointer(bool enabled);
	std::pair<float, float> TakePointerMotion();

  private:
	SDL_Window* m_window = nullptr;
	SDL_Gamepad* m_gamepad = nullptr;
	void OpenGamepad();
	bool m_focusLost = false;
	bool m_pointerCaptured = false;
	std::pair<float, float> m_pointerMotion{};
};
} // namespace simxr::app
