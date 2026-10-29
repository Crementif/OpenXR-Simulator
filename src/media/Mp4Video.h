#pragma once
#include "media/H264Frame.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace simxr
{
struct VideoSample
{
	H264Frame image;
	std::uint32_t duration = 0;
};

// Independent H.264 pictures in Annex B format, at a 90 kHz timescale.
std::vector<std::byte> WriteMp4Video(std::span<const VideoSample> samples);
} // namespace simxr
