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

  private:
	SDL_Window* m_window = nullptr;
	bool m_focusLost = false;
};
} // namespace simxr::app
