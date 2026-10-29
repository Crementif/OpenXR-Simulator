#pragma once

#include <cstdint>
#include <string>

namespace simxr::headset
{
class HeadsetGraphics;

struct HeadsetImageSource
{
	std::string endpoint;
	std::uint64_t session = 0;
	std::uint64_t generation = 0;
	// Zero selects the current full-resolution display image without a snapshot lease.
	std::uint64_t capture = 0;
};

// Synchronous feasibility check before the physical frame loop starts.
void VerifyHeadsetImageCopy(HeadsetGraphics& graphics, const HeadsetImageSource& source);
} // namespace simxr::headset
