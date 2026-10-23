#include "app/platform/GamepadFeedback.h"
#include "Pch.h"

#include <SDL3/SDL.h>

namespace simxr::app
{
GamepadFeedback::GamepadFeedback()
	: m_worker(
		  [this]
		  {
			  Run();
		  })
{
}
GamepadFeedback::~GamepadFeedback()
{
	Attach(nullptr);
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	m_worker.join();
}
void GamepadFeedback::Attach(SDL_Gamepad* gamepad)
{
	std::lock_guard lock(m_mutex);
	if (m_gamepad && m_active)
		SDL_RumbleGamepad(m_gamepad, 0, 0, 0);
	m_gamepad = gamepad;
	m_active = false;
	m_outputFailed = false;
	m_changed.notify_all();
}
bool GamepadFeedback::Rumble(std::array<std::uint16_t, 2> motors, std::chrono::steady_clock::time_point deadline)
{
	std::lock_guard lock(m_mutex);
	if (!m_gamepad)
		return true;
	const auto now = std::chrono::steady_clock::now();
	if (now >= deadline)
		motors = {};
	const bool active = motors[0] || motors[1];
	if ((m_workerFailed || m_outputFailed) && active)
	{
		SDL_RumbleGamepad(m_gamepad, 0, 0, 0);
		return false;
	}
	if (!active && !m_active)
		return true;
	const bool accepted = SDL_RumbleGamepad(m_gamepad, motors[0], motors[1], active ? 100 : 0);
	m_active = accepted ? active : m_active || active;
	m_deadline = std::min(deadline, now + std::chrono::milliseconds(100));
	m_changed.notify_all();
	return accepted;
}
void GamepadFeedback::Run() noexcept
{
	try
	{
		std::unique_lock lock(m_mutex);
		while (!m_stopping)
		{
			if (!m_active)
				m_changed.wait(lock,
							   [this]
							   {
								   return m_stopping || m_active;
							   });
			else if (std::chrono::steady_clock::now() < m_deadline)
				m_changed.wait_until(lock, m_deadline);
			else
			{
				m_active = !SDL_RumbleGamepad(m_gamepad, 0, 0, 0);
				m_outputFailed |= m_active;
				m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
			}
		}
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		m_workerFailed = true;
		if (m_gamepad)
			SDL_RumbleGamepad(m_gamepad, 0, 0, 0);
	}
}
} // namespace simxr::app
