#pragma once

#include <cstdint>

namespace simxr::app
{
struct FrameIdentity
{
	std::uint64_t session = 0, run = 0, frame = 0;
	bool operator==(const FrameIdentity&) const = default;
};
} // namespace simxr::app
