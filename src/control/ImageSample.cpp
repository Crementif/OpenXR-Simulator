#include "Pch.h"
#include "control/ImageSample.h"
#include "control/ControlSchema.h"
#include "graphics/ColorPixels.h"

namespace simxr::control
{
ImageSample ReadImageSample(const ExportReader::Exchange& exchange, const std::function<void()>& wait,
							std::uint64_t image, const ImageDescription& description,
							const std::array<XrRect2Di, 2>& rectangles)
{
	if (!description.width || !description.height || description.arrayLayers < 2)
		throw std::runtime_error("Sampled image is not stereo");
	ImageSample sample;
	sample.pixels.assign(std::size_t(ImageSample::Width) * ImageSample::Height * 4, std::byte{0});
	for (std::size_t i = 3; i < sample.pixels.size(); i += 4)
		sample.pixels[i] = std::byte{255};
	for (unsigned eye = 0; eye < 2; ++eye)
	{
		const auto& rectangle = rectangles[eye];
		const auto x = rectangle.offset.x, y = rectangle.offset.y;
		const auto w = rectangle.extent.width, h = rectangle.extent.height;
		if (!w && !h)
			continue;
		if (x < 0 || y < 0 || w <= 0 || h <= 0 || std::uint64_t(x) + w > description.width ||
			std::uint64_t(y) + h > description.height)
			throw std::runtime_error("Sample rectangle is outside its image");
		const auto scale = std::min({1.0, double(ImageSample::EyeWidth) / w, double(ImageSample::Height) / h});
		const auto width = std::max(1u, static_cast<unsigned>(std::floor(w * scale)));
		const auto height = std::max(1u, static_cast<unsigned>(std::floor(h * scale)));
		ExportReader exported(exchange,
							  {Field::U64(tag::ImageReaderId, image, true), Field::U32(tag::Layer, eye),
							   EncodeImageRegion({static_cast<unsigned>(x), static_cast<unsigned>(y),
												  static_cast<unsigned>(w), static_cast<unsigned>(h), width, height},
												 true)},
							  wait);
		const auto* descriptor = exported.Info().Find(tag::ImageDescriptor);
		if (!descriptor)
			throw std::runtime_error("Runtime omitted the sample export description");
		const auto output = DecodeImageDescription(*descriptor);
		const auto pixelSize = ColorPixelBytes(output.format);
		const auto total = std::uint64_t(width) * height * pixelSize;
		if (!pixelSize || output.width != width || output.height != height || output.arrayLayers != 1 ||
			exported.Size() != total)
			throw std::runtime_error("Sample export has an unexpected shape or format");
		std::vector<std::byte> pixels;
		pixels.reserve(static_cast<std::size_t>(total));
		exported.Read(
			[&](std::span<const std::byte> bytes) { pixels.insert(pixels.end(), bytes.begin(), bytes.end()); });
		const auto left = eye * ImageSample::EyeWidth + (ImageSample::EyeWidth - width) / 2;
		const auto top = (ImageSample::Height - height) / 2;
		sample.rectangles[eye] = {static_cast<int>(left), static_cast<int>(top), static_cast<int>(width),
								  static_cast<int>(height)};
		for (unsigned row = 0; row < height; ++row)
			for (unsigned column = 0; column < width; ++column)
			{
				const auto color = DecodeColorPixel(
					output.format,
					std::span(pixels).subspan((std::size_t(row) * width + column) * pixelSize, pixelSize));
				const auto offset = (std::size_t(top + row) * ImageSample::Width + left + column) * 4;
				for (unsigned channel = 0; channel < 3; ++channel)
				{
					const auto value = std::isnan(color[channel]) ? 0.0f : std::clamp(color[channel], 0.0f, 1.0f);
					sample.pixels[offset + channel] =
						std::byte(static_cast<unsigned char>(std::lround(value * 255.0f)));
				}
			}
	}
	return sample;
}
}
