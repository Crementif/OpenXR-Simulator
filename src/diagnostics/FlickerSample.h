#pragma once

#include "diagnostics/FlickerTypes.h"

#include <memory>
#include <vector>

namespace flicker
{
enum class PixelOrder
{
	Bgra,
	Rgba
};

struct Sample
{
	uint64_t frame = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	std::shared_ptr<const std::vector<uint8_t>> pixels;
	SampleMetrics metrics;
};

bool ValidPixels(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes);
Sample Downsample(const uint8_t* source, uint32_t width, uint32_t height, uint32_t pitchBytes, uint64_t frame,
				  PixelOrder order = PixelOrder::Bgra);
Sample CropUi(const uint8_t* source, uint32_t width, uint32_t height, uint32_t pitchBytes, const int32_t rects[2][4],
			  uint64_t frame, PixelOrder order = PixelOrder::Bgra);
void CalculateTemporal(Sample& current, const Sample& previous);
float TemporalBetween(const Sample& first, const Sample& second);
} // namespace flicker
