#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sharing
{
struct Descriptors
{
	std::array<int, 3> values{-1, -1, -1};
	Descriptors() = default;
	Descriptors(const Descriptors&) = delete;
	Descriptors& operator=(const Descriptors&) = delete;
	~Descriptors();
	int Take(std::size_t index);
};

void Send(int socket, const void* data, std::size_t size, std::span<const int> descriptors = {});
void Receive(int socket, void* data, std::size_t size, Descriptors& descriptors, std::size_t expectedDescriptors = 0);
} // namespace sharing
