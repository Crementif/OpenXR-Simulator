#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace simxr
{
// Values match the Vulkan formats carried in image descriptions.
enum class ColorFormat : std::uint32_t
{
	Rgba8Unorm = 37,
	Rgba8Srgb = 43,
	Bgra8Unorm = 44,
	Bgra8Srgb = 50,
	Rgb10A2Unorm = 64,
	Rgba16Float = 97,
	Rgba32Float = 109
};

std::uint32_t ColorPixelBytes(std::uint32_t format) noexcept;
// Decode stored components without clamping or changing their transfer function.
std::array<float, 4> DecodeColorPixel(std::uint32_t format, std::span<const std::byte> bytes);
}
