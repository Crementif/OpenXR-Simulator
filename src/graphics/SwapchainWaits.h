#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>

namespace simxr
{
class SwapchainWaits
{
public:
	class Registration
	{
	public:
		Registration(Registration&& other) noexcept;
		~Registration();
		Registration(const Registration&) = delete;
		Registration& operator=(const Registration&) = delete;

	private:
		friend class SwapchainWaits;
		explicit Registration(SwapchainWaits& owner);
		SwapchainWaits* m_owner;
	};

	std::optional<Registration> Enter();
	void Close();
	bool Drain(std::chrono::nanoseconds timeout);

private:
	void Leave();
	std::mutex m_mutex;
	std::condition_variable m_changed;
	std::size_t m_readers = 0;
	bool m_closed = false;
};
} // namespace simxr
