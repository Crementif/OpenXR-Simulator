#pragma once

#include "graphics/backends/vulkan/VulkanExternal.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace simxr
{
// One internal binary signal/wait pair, shared by its producer and inspection work.
class VulkanFrameSync
{
  public:
	VkSemaphore Signal() const noexcept;
	// The frame's single-consumer preparation claim serializes imports.
	std::shared_ptr<VulkanExternalSemaphore> Import(const VulkanExternalDevice& consumer);

  private:
	friend class VulkanFrameSyncPool;
	friend class VulkanInspectionWork;
	// Only confirmed completion of the imported wait permits reuse.
	void MarkConsumed() noexcept;
	explicit VulkanFrameSync(std::shared_ptr<VulkanExternalSemaphore> producer);
	const std::shared_ptr<VulkanExternalSemaphore> m_producer;
	std::shared_ptr<VulkanExternalSemaphore> m_consumer;
	VulkanExternalHandle m_handle;
	std::atomic<bool> m_consumed{false};
};

class VulkanFrameSyncPool final : public std::enable_shared_from_this<VulkanFrameSyncPool>
{
  public:
	explicit VulkanFrameSyncPool(std::shared_ptr<VulkanExternalDevice> producer);
	std::shared_ptr<VulkanFrameSync> Acquire();
	// Clear idle native objects before the borrowed producer device is destroyed.
	void Close();

  private:
	void Recycle(std::unique_ptr<VulkanFrameSync> sync);
	const std::shared_ptr<VulkanExternalDevice> m_producer;
	std::mutex m_mutex;
	std::array<std::unique_ptr<VulkanFrameSync>, 16> m_available;
	bool m_closed = false;
};
} // namespace simxr
