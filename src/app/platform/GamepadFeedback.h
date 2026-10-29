#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct SDL_Gamepad;

namespace simxr::app
{
// Window owns the gamepad. Detach waits for any motor call before that handle closes.
class GamepadFeedback
{
public:
	GamepadFeedback();
	~GamepadFeedback();
	GamepadFeedback(const GamepadFeedback&) = delete;
	GamepadFeedback& operator=(const GamepadFeedback&) = delete;
	void Attach(SDL_Gamepad* gamepad);
	bool Rumble(std::array<std::uint16_t, 2> motors,
				std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());

private:
	void Run() noexcept;
	std::mutex m_mutex;
	std::condition_variable m_changed;
	SDL_Gamepad* m_gamepad = nullptr;
	std::chrono::steady_clock::time_point m_deadline{};
	bool m_active = false, m_stopping = false, m_workerFailed = false, m_outputFailed = false;
	std::thread m_worker;
};
} // namespace simxr::app
