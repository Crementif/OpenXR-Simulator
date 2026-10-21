#pragma once

#include <utility>

struct SDL_Window;

namespace simxr::app
{
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
	void Resize(int width, int height);
	bool CapturePointer(bool enabled);
	std::pair<float, float> TakePointerMotion();

  private:
	SDL_Window* m_window = nullptr;
	bool m_focusLost = false;
	bool m_pointerCaptured = false;
	std::pair<float, float> m_pointerMotion{};
};
} // namespace simxr::app
