#pragma once

#include "graphics/backends/vulkan/VulkanExternal.h"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>

namespace simxr
{
// Reuse requires the end of both image leases, including all submitted GPU readers.
// Producers must fully reinitialize each lease from UNDEFINED before releasing it.
class VulkanImagePool final : public std::enable_shared_from_this<VulkanImagePool>
{
	struct Allocation;

public:
	class Lease final : public std::enable_shared_from_this<Lease>
	{
	public:
		~Lease();

		std::shared_ptr<VulkanExternalImage> External();
		std::shared_ptr<VulkanImage> Import(const VulkanExternalDevice& consumer);

	private:
		friend class VulkanImagePool;
		Lease(std::shared_ptr<VulkanImagePool> pool, std::shared_ptr<Allocation> allocation,
			  std::shared_ptr<VulkanExternalImage> image, bool producer);

		const std::shared_ptr<VulkanImagePool> m_pool;
		const std::shared_ptr<Allocation> m_allocation;
		std::shared_ptr<VulkanExternalImage> m_image;
		const bool m_producer;
	};

	explicit VulkanImagePool(std::shared_ptr<VulkanExternalDevice> producer);

	std::shared_ptr<Lease> Acquire(const VulkanExternalImageSpec& spec);
	// Drop every cached borrowed-device owner, even if its owned inspection lease survives.
	void Close();

private:
	struct Allocation
	{
		std::shared_ptr<VulkanExternalImage> producer;
		std::shared_ptr<VulkanExternalImage> consumer;
		bool producerUsed = false;
		std::size_t consumerUses = 0;
		bool cached = false;
		bool Idle() const noexcept;
	};

	std::shared_ptr<VulkanExternalImage> External(const std::shared_ptr<Lease>& lease);
	std::shared_ptr<VulkanImage> Import(const Lease& lease, const VulkanExternalDevice& consumer);
	void Return(const std::shared_ptr<Allocation>& allocation, bool producer);

	const std::shared_ptr<VulkanExternalDevice> m_producer;
	std::mutex m_mutex;
	std::array<std::shared_ptr<Allocation>, 64> m_allocations;
	bool m_closed = false;
};
} // namespace simxr
