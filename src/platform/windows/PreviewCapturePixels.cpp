#include "platform/windows/PreviewCapturePixels.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace simxr::windows
{
PreviewCaptureLayout::PreviewCaptureLayout(PreviewExtent extent, PreviewCrop crop, std::uint32_t slice,
										   std::uint32_t subresource)
	: m_extent(extent), m_crop(crop), m_slice(slice), m_subresource(subresource)
{
}

std::optional<PreviewCaptureLayout> PreviewCaptureLayout::Make(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
															   std::uint32_t mipLevels, std::uint32_t arrayLayers,
															   std::uint32_t slice, PreviewCrop crop)
{
	if (!mipLevels || !arrayLayers || slice >= arrayLayers || crop.x > sourceWidth || crop.y > sourceHeight ||
		crop.width > sourceWidth - crop.x || crop.height > sourceHeight - crop.y ||
		std::uint64_t(slice) * mipLevels > std::numeric_limits<std::uint32_t>::max())
		return {};
	const auto extent = PreviewExtent::Make(crop.width, crop.height);
	if (!extent || extent->readbackBytes > MaxBytes)
		return {};
	return PreviewCaptureLayout{*extent, crop, slice, slice * mipLevels};
}
const PreviewExtent& PreviewCaptureLayout::Extent() const
{
	return m_extent;
}
const PreviewCrop& PreviewCaptureLayout::Crop() const
{
	return m_crop;
}
std::uint32_t PreviewCaptureLayout::Slice() const
{
	return m_slice;
}
std::uint32_t PreviewCaptureLayout::Subresource() const
{
	return m_subresource;
}
std::vector<std::uint8_t> PreviewCaptureLayout::Rgba(std::span<const std::uint8_t> mapped, bool bgra) const
{
	if (mapped.size() < m_extent.readbackBytes)
		throw std::invalid_argument("Explicit preview readback has incomplete mapped rows");
	const auto rowBytes = std::size_t(m_extent.width) * 4;
	std::vector<std::uint8_t> output(rowBytes * m_extent.height);
	for (std::uint32_t row = 0; row < m_extent.height; ++row)
	{
		auto* destination = output.data() + rowBytes * row;
		std::memcpy(destination, mapped.data() + std::size_t(m_extent.rowPitch) * row, rowBytes);
		if (bgra)
			for (std::uint32_t column = 0; column < m_extent.width; ++column)
				std::swap(destination[column * 4], destination[column * 4 + 2]);
	}
	return output;
}
std::vector<std::uint8_t> CopyPreviewRgbaCrop(std::span<const std::uint8_t> source, std::uint32_t sourceWidth,
											  std::uint32_t sourceHeight, PreviewCrop crop, bool flipVertically)
{
	const auto layout = PreviewCaptureLayout::Make(sourceWidth, sourceHeight, 1, 1, 0, crop);
	const auto sourcePixels = std::uint64_t(sourceWidth) * sourceHeight;
	if (!layout || sourceWidth == 0 || sourcePixels > PreviewCaptureLayout::MaxBytes / 4 ||
		sourcePixels * 4 > source.size())
		throw std::invalid_argument("Preview RGBA crop exceeds its bounded source storage");
	const auto sourceRowBytes = std::size_t(sourceWidth) * 4;
	const auto rowBytes = std::size_t(crop.width) * 4;
	std::vector<std::uint8_t> pixels(rowBytes * crop.height);
	for (std::size_t row = 0; row < crop.height; ++row)
	{
		const auto sourceRow = flipVertically ? crop.height - 1 - row : row;
		const auto offset = (sourceRow + crop.y) * sourceRowBytes + std::size_t(crop.x) * 4;
		std::memcpy(pixels.data() + row * rowBytes, source.data() + offset, rowBytes);
	}
	return pixels;
}
std::vector<std::uint8_t> PadPreviewRgba(std::span<const std::uint8_t> source, std::uint32_t width,
										 std::uint32_t height, std::uint32_t canvasWidth, std::uint32_t canvasHeight)
{
	const auto sourcePixels = std::uint64_t(width) * height;
	const auto canvasPixels = std::uint64_t(canvasWidth) * canvasHeight;
	if (!width || !height || width > canvasWidth || height > canvasHeight ||
		canvasPixels > PreviewCaptureLayout::MaxBytes / 4 || sourcePixels * 4 > source.size())
		throw std::invalid_argument("Preview RGBA canvas exceeds its bounded source or destination");
	std::vector<std::uint8_t> result(static_cast<std::size_t>(canvasPixels * 4), 0);
	for (std::size_t pixel = 0; pixel < canvasPixels; ++pixel)
		result[pixel * 4 + 3] = 255;
	const auto rowBytes = std::size_t(width) * 4;
	for (std::size_t row = 0; row < height; ++row)
		std::memcpy(result.data() + row * canvasWidth * 4, source.data() + row * rowBytes, rowBytes);
	return result;
}
} // namespace simxr::windows
