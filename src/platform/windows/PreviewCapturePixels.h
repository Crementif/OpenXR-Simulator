#pragma once

#include "platform/windows/PreviewFrameSchedule.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace simxr::windows
{
struct PreviewCrop
{
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

// One validated mip-zero slice/crop and its bounded D3D12 footprint. It survives
// asynchronously with the native readback, rather than consulting current views.
class PreviewCaptureLayout
{
  public:
	static constexpr std::uint64_t MaxBytes = 128 * 1024 * 1024;
	static std::optional<PreviewCaptureLayout> Make(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
													std::uint32_t mipLevels, std::uint32_t arrayLayers,
													std::uint32_t slice, PreviewCrop crop);
	const PreviewExtent& Extent() const;
	const PreviewCrop& Crop() const;
	std::uint32_t Slice() const;
	std::uint32_t Subresource() const;
	std::vector<std::uint8_t> Rgba(std::span<const std::uint8_t> mapped, bool bgra) const;

  private:
	PreviewCaptureLayout(PreviewExtent extent, PreviewCrop crop, std::uint32_t slice, std::uint32_t subresource);
	PreviewExtent m_extent;
	PreviewCrop m_crop;
	std::uint32_t m_slice;
	std::uint32_t m_subresource;
};
// A tightly packed RGBA source, copied into an independently owned crop.
std::vector<std::uint8_t> CopyPreviewRgbaCrop(std::span<const std::uint8_t> source, std::uint32_t sourceWidth,
											  std::uint32_t sourceHeight, PreviewCrop crop,
											  bool flipVertically = false);
std::vector<std::uint8_t> PadPreviewRgba(std::span<const std::uint8_t> source, std::uint32_t width,
										 std::uint32_t height, std::uint32_t canvasWidth, std::uint32_t canvasHeight);
} // namespace simxr::windows
