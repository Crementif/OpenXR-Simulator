#pragma once

#include "control/ExportReader.h"
#include "control/ImageInspection.h"

namespace simxr::control
{
struct ImageSample
{
	static constexpr unsigned EyeWidth = 160, Height = 90, Width = EyeWidth * 2;
	std::vector<std::byte> pixels;
	std::array<std::array<std::int32_t, 4>, 2> rectangles{};
};

ImageSample ReadImageSample(const ExportReader::Exchange& exchange, const std::function<void()>& wait,
								  std::uint64_t image, const ImageDescription& description,
								  const std::array<XrRect2Di, 2>& rectangles);
}
