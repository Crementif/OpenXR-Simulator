#pragma once

#include <cstdint>
#include <functional>
#include <openxr/openxr.h>
#include <optional>

namespace simxr
{
struct SwapchainDescription
{
	std::uint32_t width;
	std::uint32_t height;
	std::uint32_t arraySize;
	bool released;
};

struct LayerValidationContext
{
	std::uint32_t viewCount = 2;
	std::uint32_t maxLayers = 16;
	bool depthEnabled = false;
	std::function<bool(XrSpace)> validSpace;
	std::function<std::optional<SwapchainDescription>(XrSwapchain)> swapchain;
};

XrResult ValidateLayers(const XrFrameEndInfo& info, const LayerValidationContext& context);
} // namespace simxr
