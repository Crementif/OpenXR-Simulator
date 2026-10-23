#include "Pch.h"
#include "graphics/ColorPixels.h"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace simxr
{
std::uint32_t ColorPixelBytes(std::uint32_t format) noexcept
{
	switch (static_cast<ColorFormat>(format))
	{
	case ColorFormat::Rgba8Unorm:
	case ColorFormat::Rgba8Srgb:
	case ColorFormat::Bgra8Unorm:
	case ColorFormat::Bgra8Srgb:
	case ColorFormat::Rgb10A2Unorm:
		return 4;
	case ColorFormat::Rgba16Float:
		return 8;
	case ColorFormat::Rgba32Float:
		return 16;
	default:
		return 0;
	}
}

static std::uint32_t ReadComponent(std::span<const std::byte> bytes)
{
	std::uint32_t value = 0;
	for (std::size_t index = 0; index < bytes.size(); ++index)
		value |= std::to_integer<std::uint32_t>(bytes[index]) << (index * 8);
	return value;
}

static float DecodeHalf(std::uint32_t bits)
{
	const auto exponent = (bits >> 10) & 31;
	const auto fraction = bits & 1023;
	float value;
	if (exponent == 31)
		value = fraction ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
	else if (!exponent)
		value = std::ldexp(static_cast<float>(fraction), -24);
	else
		value = std::ldexp(static_cast<float>(1024 + fraction), static_cast<int>(exponent) - 25);
	return bits & 0x8000 ? -value : value;
}

std::array<float, 4> DecodeColorPixel(std::uint32_t format, std::span<const std::byte> bytes)
{
	const auto size = ColorPixelBytes(format);
	if (!size || bytes.size() < size)
		throw std::invalid_argument("Unsupported color format or truncated pixel");
	std::array<float, 4> result{};
	if (format == static_cast<std::uint32_t>(ColorFormat::Rgb10A2Unorm))
	{
		const auto pixel = ReadComponent(bytes.first(4));
		for (std::size_t channel = 0; channel < 3; ++channel)
			result[channel] = static_cast<float>((pixel >> (channel * 10)) & 1023) / 1023.0f;
		result[3] = static_cast<float>(pixel >> 30) / 3.0f;
		return result;
	}
	for (std::size_t channel = 0; channel < 4; ++channel)
	{
		if (size == 16)
			result[channel] = std::bit_cast<float>(ReadComponent(bytes.subspan(channel * 4, 4)));
		else if (size == 8)
			result[channel] = DecodeHalf(ReadComponent(bytes.subspan(channel * 2, 2)));
		else
			result[channel] = std::to_integer<unsigned>(bytes[channel]) / 255.0f;
	}
	if (format == static_cast<std::uint32_t>(ColorFormat::Bgra8Unorm) ||
		format == static_cast<std::uint32_t>(ColorFormat::Bgra8Srgb))
		std::swap(result[0], result[2]);
	return result;
}
}
