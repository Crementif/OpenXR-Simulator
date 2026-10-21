#pragma once

#include "graphics/Compositor.h"
#include "graphics/FramePool.h"
#include "graphics/backends/vulkan/VulkanSubmission.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <openxr/openxr.h>
#include <span>

namespace simxr
{
using VulkanImageResolver = std::function<std::shared_ptr<VulkanImage>(XrSwapchain)>;

class VulkanBackend
{
  public:
	// Caller serializes backend operations. Frame completion observers may run concurrently.
	explicit VulkanBackend(const VulkanBinding& binding, std::size_t frameCapacity = 8,
						   std::span<const char* const> enabledExtensions = {},
						   std::uint32_t instanceApiVersion = VK_API_VERSION_1_0, std::size_t liveCapacity = 3);
	~VulkanBackend();
	VulkanBackend(const VulkanBackend&) = delete;
	VulkanBackend& operator=(const VulkanBackend&) = delete;
	std::shared_ptr<VulkanImage> CreateImage(VkFormat format, VkExtent2D extent, std::uint32_t layers,
											 VkImageUsageFlags usage);
	// These two submission points require the host's binding-queue access permit.
	std::shared_ptr<VulkanSubmission> InitializeImage(std::shared_ptr<VulkanImage> image);
	XrResult Submit(const std::shared_ptr<VulkanFrame>& frame);
	XrResult Submit(std::span<const std::shared_ptr<VulkanFrame>> frames);
	// Preparation records commands and reserves private storage, without submitting.
	XrResult Prepare(const CompositionPlan& plan, const VulkanImageResolver& resolve,
					 std::shared_ptr<VulkanFrame>& output, std::shared_ptr<FrameReservation> reservation = {});
	XrResult PrepareCapture(const LayerImageDescription& source, const VulkanImageResolver& resolve,
							std::shared_ptr<VulkanFrame>& output, std::shared_ptr<FrameReservation> reservation = {});
	FramePool Reservations() const;
	FramePool LiveReservations() const;
	void RetireCompleted();
	VkResult Drain(std::uint64_t timeout);
	// Closes new reservation admission, even when readers delay native closure.
	// Existing reservations remain usable until a successful Close. No queue access.
	// Readers must release actual borrowed-device native owners first.
	XrResult Close();
	std::shared_ptr<VulkanDevice> Device() const;
	bool Sharing() const;

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr
