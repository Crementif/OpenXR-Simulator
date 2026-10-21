#pragma once

#include "graphics/Compositor.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace simxr
{
enum class CaptureColorOrder
{
	Rgba,
	Bgra
};

enum class CaptureColorEncoding
{
	Linear,
	Srgb
};

struct CapturePixels
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	CaptureColorOrder order = CaptureColorOrder::Bgra;
	CaptureColorEncoding encoding = CaptureColorEncoding::Srgb;
	std::vector<std::uint8_t> pixels;
};

struct CaptureImageDraw
{
	LayerConstants constants;
	std::uint32_t eye = 0;
	std::size_t source = 0;
};

// Constants use the shared compositor's geometry, rebased to each cropped CPU
// source. The plan retains no application pointers or native resource owners.
struct CaptureImagePlan
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t firstEye = 0;
	std::uint32_t eyeCount = 2;
	std::vector<CaptureImageDraw> draws;
};

std::size_t CapturePixelBytes(std::uint32_t width, std::uint32_t height);
std::vector<std::uint8_t> ComposeCaptureImage(const CaptureImagePlan& plan, std::span<const CapturePixels> sources);
} // namespace simxr
