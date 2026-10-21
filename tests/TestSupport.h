#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

inline void Require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template<typename Predicate>
void Eventually(Predicate predicate, const char* message)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!predicate())
	{
		Require(std::chrono::steady_clock::now() < deadline, message);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
