#include "app/Window.h"

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <string>

namespace simxr::app
{
Window::Window(bool hidden)
{
	if (!SDL_Init(SDL_INIT_VIDEO))
		throw std::runtime_error(std::string("SDL video unavailable: ") + SDL_GetError());
	const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
	SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (hidden)
		flags |= SDL_WINDOW_HIDDEN;
	m_window =
		SDL_CreateWindow("OpenXR Simulator", static_cast<int>(1280 * scale), static_cast<int>(800 * scale), flags);
	if (!m_window)
	{
		const auto error = std::string("SDL Vulkan window unavailable: ") + SDL_GetError();
		SDL_Quit();
		throw std::runtime_error(error);
	}
}

Window::~Window()
{
	SDL_DestroyWindow(m_window);
	SDL_Quit();
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

bool Window::PollEvents()
{
	SDL_Event event;
	bool running = true;
	while (SDL_PollEvent(&event))
	{
		ImGui_ImplSDL3_ProcessEvent(&event);
		if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && event.window.windowID == SDL_GetWindowID(m_window))
			m_focusLost = true;
		if (event.type == SDL_EVENT_QUIT ||
			(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(m_window)))
			running = false;
	}
	return running;
}

bool Window::TakeFocusLost()
{
	return std::exchange(m_focusLost, false);
}

void Window::Resize(int width, int height)
{
	if (!SDL_SetWindowSize(m_window, width, height))
		throw std::runtime_error(SDL_GetError());
}
} // namespace simxr::app
