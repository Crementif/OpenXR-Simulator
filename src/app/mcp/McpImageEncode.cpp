#include "Pch.h"
#include "app/mcp/McpImageEncode.h"
#include "graphics/ColorPixels.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <atomic>
#include <cmath>
#include <stdexcept>

namespace simxr::app::mcp
{
static std::atomic<unsigned> s_imageWorkers{0};

ImageEncodingPermit::ImageEncodingPermit()
{
	auto active = s_imageWorkers.load();
	for (;;)
	{
		if (active >= 2)
			throw std::runtime_error("Image encoding capacity is busy; retry explicitly");
		if (s_imageWorkers.compare_exchange_weak(active, active + 1))
			break;
	}
}
ImageEncodingPermit::~ImageEncodingPermit()
{
	--s_imageWorkers;
}

struct ImageOutput
{
	std::vector<std::byte> bytes;
	bool overflow = false;
};
static void WriteImage(void* context, void* data, int size)
{
	auto& output = *static_cast<ImageOutput*>(context);
	if (size < 0 || static_cast<std::size_t>(size) > MaxEncodedImage - output.bytes.size())
	{
		output.overflow = true;
		return;
	}
	const auto* begin = static_cast<const std::byte*>(data);
	output.bytes.insert(output.bytes.end(), begin, begin + size);
}
EncodedImage EncodeImage(std::span<const std::byte> pixels, std::uint32_t format, std::uint32_t width,
						 std::uint32_t height, bool png, int quality)
{
	const auto pixelSize = ColorPixelBytes(format);
	if (!pixelSize || !width || !height || width > 4096 || height > 4096 ||
		std::uint64_t{width} * height > MaxImageInput / pixelSize ||
		pixels.size() != std::uint64_t{width} * height * pixelSize)
		throw std::invalid_argument("Image shape, format or input size is unsupported");
	if (quality < 1 || quality > 100)
		throw std::invalid_argument("JPEG quality must be between 1 and 100");
	const auto count = static_cast<std::size_t>(width) * height;
	std::vector<unsigned char> rgba(count * 4);
	for (std::size_t index = 0; index < count; ++index)
	{
		const auto channels = DecodeColorPixel(format, pixels.subspan(index * pixelSize, pixelSize));
		for (std::size_t channel = 0; channel < 4; ++channel)
		{
			const auto value = std::isnan(channels[channel]) ? 0.0f : std::clamp(channels[channel], 0.0f, 1.0f);
			rgba[index * 4 + channel] = static_cast<unsigned char>(std::lround(value * 255.0f));
		}
	}
	ImageOutput output;
	const int ok = png ? stbi_write_png_to_func(WriteImage, &output, static_cast<int>(width),
											static_cast<int>(height), 4, rgba.data(), static_cast<int>(width * 4))
					   : stbi_write_jpg_to_func(WriteImage, &output, static_cast<int>(width),
											static_cast<int>(height), 4, rgba.data(), quality);
	if (!ok || output.overflow || output.bytes.empty())
		throw std::runtime_error(output.overflow ? "Compressed image exceeds the 8 MiB response limit"
												: "Image encoding failed");
	const bool eightBit = format == static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm) ||
		format == static_cast<std::uint32_t>(ColorFormat::Rgba8Srgb) ||
		format == static_cast<std::uint32_t>(ColorFormat::Bgra8Unorm) ||
		format == static_cast<std::uint32_t>(ColorFormat::Bgra8Srgb);
	return {std::move(output.bytes), png ? "image/png" : "image/jpeg", !png || !eightBit};
}
} // namespace simxr::app::mcp
