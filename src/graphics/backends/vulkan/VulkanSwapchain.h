#pragma once

#include "graphics/SwapchainAccess.h"
#include "graphics/backends/vulkan/VulkanBackend.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace simxr
{
// Native calls are externally serialized. Wait touches only access state, so
// closure can cancel it and retire image readers without invalidating its data.
class VulkanSwapchain
{
public:
	static std::span<const std::int64_t> Formats();
	static std::span<const std::int64_t> ColorFormats();
	static XrResult Create(VulkanBackend& backend, const XrSwapchainCreateInfo& description,
						   std::unique_ptr<VulkanSwapchain>& output);
	XrResult EnumerateImages(std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageBaseHeader* images) const;
	XrResult Acquire(VulkanBackend& backend, std::uint32_t& index);
	XrResult Wait(XrDuration timeout);
	XrResult Release();
	SwapchainDescription Description() const;
	std::shared_ptr<VulkanImage> ReleasedImage() const;
	void MarkReleasedUsed(const std::shared_ptr<const GpuCompletion>& completion);
	void Close();

private:
	struct Image
	{
		std::shared_ptr<VulkanImage> resource;
		bool initialized = false;
	};

	explicit VulkanSwapchain(XrSwapchainCreateInfo description);
	const XrSwapchainCreateInfo m_description;
	std::vector<Image> m_images;
	SwapchainAccess m_access;
};
} // namespace simxr
