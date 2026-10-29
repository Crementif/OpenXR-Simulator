#include "Pch.h"
#include "app/services/ImageExport.h"
#include "graphics/ColorPixels.h"
#include "app/platform/FileStorage.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace simxr::app
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
	std::size_t limit;
	bool overflow = false;
};
static void WriteImage(void* context, void* data, int size)
{
	auto& output = *static_cast<ImageOutput*>(context);
	if (size < 0 || static_cast<std::size_t>(size) > output.limit - output.bytes.size())
	{
		output.overflow = true;
		return;
	}
	const auto* begin = static_cast<const std::byte*>(data);
	output.bytes.insert(output.bytes.end(), begin, begin + size);
}
EncodedImage EncodeImage(std::span<const std::byte> pixels, std::uint32_t format, std::uint32_t width,
						 std::uint32_t height, bool png, int quality, ImageEncodingLimits limits)
{
	const auto pixelSize = ColorPixelBytes(format);
	if (!pixelSize || !width || !height || width > 16384 || height > 16384 ||
		width > limits.dimension || height > limits.dimension ||
		std::uint64_t{width} * height > limits.inputBytes / pixelSize ||
		pixels.size() != std::uint64_t{width} * height * pixelSize)
		throw std::invalid_argument("Image shape, format or input size is unsupported");
	if (quality < 1 || quality > 100)
		throw std::invalid_argument("JPEG quality must be between 1 and 100");
	const bool rgba8 = format == static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm) ||
		format == static_cast<std::uint32_t>(ColorFormat::Rgba8Srgb);
	const bool bgra8 = format == static_cast<std::uint32_t>(ColorFormat::Bgra8Unorm) ||
		format == static_cast<std::uint32_t>(ColorFormat::Bgra8Srgb);
	const auto count = static_cast<std::size_t>(width) * height;
	std::vector<unsigned char> converted;
	const void* rgba = pixels.data();
	if (!rgba8)
	{
		converted.resize(count * 4);
		for (std::size_t index = 0; index < count; ++index)
		{
			if (bgra8)
			{
				converted[index * 4] = std::to_integer<unsigned char>(pixels[index * 4 + 2]);
				converted[index * 4 + 1] = std::to_integer<unsigned char>(pixels[index * 4 + 1]);
				converted[index * 4 + 2] = std::to_integer<unsigned char>(pixels[index * 4]);
				converted[index * 4 + 3] = std::to_integer<unsigned char>(pixels[index * 4 + 3]);
				continue;
			}
			const auto channels = DecodeColorPixel(format, pixels.subspan(index * pixelSize, pixelSize));
			for (std::size_t channel = 0; channel < 4; ++channel)
			{
				auto value = std::isnan(channels[channel]) ? 0.0f : std::clamp(channels[channel], 0.0f, 1.0f);
				if (channel < 3)
					value = value <= .0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - .055f;
				converted[index * 4 + channel] = static_cast<unsigned char>(std::lround(value * 255.0f));
			}
		}
		rgba = converted.data();
	}
	ImageOutput output{{}, limits.outputBytes};
	const int ok = png ? stbi_write_png_to_func(WriteImage, &output, static_cast<int>(width),
											static_cast<int>(height), 4, rgba, static_cast<int>(width * 4))
					   : stbi_write_jpg_to_func(WriteImage, &output, static_cast<int>(width),
											static_cast<int>(height), 4, rgba, quality);
	if (!ok || output.overflow || output.bytes.empty())
		throw std::runtime_error(output.overflow ? "Compressed image exceeds the output byte limit"
												: "Image encoding failed");
	return {std::move(output.bytes), png ? "image/png" : "image/jpeg", !png || !(rgba8 || bgra8)};
}
void SaveExportFile(const std::filesystem::path& destination, std::span<const std::byte> bytes,
	bool overwrite, const std::function<void()>& beforeCommit)
{
	SaveExportFile(destination, [&](std::ostream& output)
	{
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}, overwrite, beforeCommit);
}
void SaveExportFile(const std::filesystem::path& destination, const std::function<void(std::ostream&)>& write,
	bool overwrite, const std::function<void()>& beforeCommit)
{
	const auto temporary = CreatePrivateTemporaryFile(destination);
	try
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("Cannot open temporary export file");
		write(output);
		output.close();
		if (!output)
			throw std::runtime_error("Cannot write or close temporary export file");
		if (beforeCommit)
			beforeCommit();
		if (overwrite)
			CommitTemporaryFile(temporary, destination);
		else
			RenameFileWithoutReplacing(temporary, destination);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}
} // namespace simxr::app
