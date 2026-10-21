#pragma once

#include "control/ImageInspection.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

#include <memory>
#include <span>
#include <vector>

namespace simxr::control
{
// A native consumer shared by the companion and headless inspection clients.
// It imports one immutable delivery and copies it to private sampled storage.
// The owner must retire all UI uses before releasing a completed consumer.
class VulkanImageConsumer
{
  public:
	VulkanImageConsumer(std::shared_ptr<VulkanDevice> device, std::span<const char* const> enabledExtensions,
						std::uint32_t apiVersion);
	~VulkanImageConsumer();
	VulkanImageConsumer(const VulkanImageConsumer&) = delete;
	VulkanImageConsumer& operator=(const VulkanImageConsumer&) = delete;
	ImageDeviceIdentity Identity() const;
	std::shared_ptr<TransferHandle> ConsumerFence();
	void Import(const ImageDescription& description, std::span<const std::shared_ptr<TransferHandle>> handles);
	void Submit();
	CompletionStatus Poll();
	bool NativeIdle() const;
	VkSemaphore SampleReady() const;
	std::span<const VkImageView> Views() const;
	VkSampler Sampler() const;
	const ImageDescription& Description() const;
	const VulkanImage& Image() const;
	// A failed import can acknowledge that no consumer access occurred. This
	// signals the same offered fence on an owned queue without touching source.
	void CancelUnused();
	// Only after transport admission failed before any byte/FD could be sent.
	void AbandonUnsent();

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::control
