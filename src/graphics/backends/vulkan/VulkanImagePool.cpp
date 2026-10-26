#include "graphics/backends/vulkan/VulkanImagePool.h"
#include "Pch.h"

namespace simxr
{
static constexpr VkDeviceSize s_idleBytesLimit = 64ULL * 1024 * 1024;

bool VulkanImagePool::Allocation::Idle() const noexcept
{
	return !producerUsed && !consumerUses;
}
VulkanImagePool::Lease::Lease(std::shared_ptr<VulkanImagePool> pool, std::shared_ptr<Allocation> allocation,
							  std::shared_ptr<VulkanExternalImage> image, bool producer)
	: m_pool(std::move(pool)), m_allocation(std::move(allocation)), m_image(std::move(image)), m_producer(producer)
{
}
VulkanImagePool::Lease::~Lease()
{
	m_image.reset();
	m_pool->Return(m_allocation, m_producer);
}
std::shared_ptr<VulkanExternalImage> VulkanImagePool::Lease::External()
{
	return m_pool->External(shared_from_this());
}
std::shared_ptr<VulkanImage> VulkanImagePool::Lease::Import(const VulkanExternalDevice& consumer)
{
	return m_pool->Import(*this, consumer);
}
VulkanImagePool::VulkanImagePool(std::shared_ptr<VulkanExternalDevice> producer) : m_producer(std::move(producer)) {}

std::shared_ptr<VulkanImagePool::Lease> VulkanImagePool::Acquire(const VulkanExternalImageSpec& spec)
{
	if (m_producer->Device()->lost.load())
		throw VulkanError(VK_ERROR_DEVICE_LOST, "Image allocation device is lost");
	const auto pool = shared_from_this();
	std::shared_ptr<Allocation> allocation;
	std::shared_ptr<Allocation> retired;
	std::shared_ptr<VulkanExternalImage> image;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			throw VulkanError(VK_ERROR_INITIALIZATION_FAILED, "Image allocation pool is closed");
		for (const auto& entry : m_allocations)
			if (entry && entry->Idle() && entry->producer->Descriptor().image == spec &&
				(!entry->consumer || !entry->consumer->Image()->device->lost.load()))
			{
				allocation = entry;
				allocation->producerUsed = true;
				image = allocation->producer;
				break;
			}
	}
	if (!allocation)
	{
		allocation = std::make_shared<Allocation>();
		image = m_producer->CreateExportable(spec);
		allocation->producer = image;
		allocation->producerUsed = true;
		std::lock_guard lock(m_mutex);
		if (m_closed)
			throw VulkanError(VK_ERROR_INITIALIZATION_FAILED, "Image allocation pool closed during allocation");
		auto available = std::find(m_allocations.begin(), m_allocations.end(), nullptr);
		if (available == m_allocations.end())
			available = std::find_if(m_allocations.begin(), m_allocations.end(),
									 [](const auto& entry)
									 {
										 return entry->Idle();
									 });
		if (available != m_allocations.end())
		{
			retired = std::move(*available);
			if (retired)
				retired->cached = false;
			*available = allocation;
			allocation->cached = true;
		}
	}
	std::unique_ptr<Lease> lease;
	try
	{
		lease.reset(new Lease(pool, allocation, std::move(image), true));
	}
	catch (...)
	{
		Return(allocation, true);
		throw;
	}
	return std::shared_ptr<Lease>(std::move(lease));
}

std::shared_ptr<VulkanExternalImage> VulkanImagePool::External(const std::shared_ptr<Lease>& lease)
{
	const auto source = lease->m_image->Image();
	auto image = VulkanImage::Lease(std::shared_ptr<VulkanImage>(lease, source.get()));
	return std::shared_ptr<VulkanExternalImage>(new VulkanExternalImage(std::move(image), lease->m_image));
}

std::shared_ptr<VulkanImage> VulkanImagePool::Import(const Lease& producer, const VulkanExternalDevice& consumer)
{
	if (consumer.Device()->lost.load())
		throw VulkanError(VK_ERROR_DEVICE_LOST, "Image inspection device is lost");
	const auto& allocation = producer.m_allocation;
	std::shared_ptr<VulkanExternalImage> image;
	{
		std::lock_guard lock(m_mutex);
		if (allocation->consumer && allocation->consumer->Image()->device == consumer.Device())
			image = allocation->consumer;
	}
	if (!image)
	{
		auto handle = producer.m_image->ExportMemory();
		image = consumer.ImportImage(producer.m_image->Descriptor(), handle);
	}
	std::shared_ptr<VulkanExternalImage> retired;
	{
		std::lock_guard lock(m_mutex);
		if (allocation->cached && !m_closed && !allocation->consumerUses)
		{
			retired = std::move(allocation->consumer);
			allocation->consumer = image;
		}
		++allocation->consumerUses;
	}
	std::unique_ptr<Lease> pending;
	try
	{
		pending.reset(new Lease(shared_from_this(), allocation, image, false));
	}
	catch (...)
	{
		Return(allocation, false);
		throw;
	}
	const auto lease = std::shared_ptr<Lease>(std::move(pending));
	return VulkanImage::Lease(std::shared_ptr<VulkanImage>(lease, image->Image().get()));
}

void VulkanImagePool::Return(const std::shared_ptr<Allocation>& allocation, bool producer)
{
	std::shared_ptr<VulkanExternalImage> retiredProducer, retiredConsumer;
	std::lock_guard lock(m_mutex);
	if (producer)
		allocation->producerUsed = false;
	else
		--allocation->consumerUses;
	bool cache = allocation->cached && !m_closed && !m_producer->Device()->lost.load() &&
				 (!allocation->consumer || !allocation->consumer->Image()->device->lost.load());
	if (cache && allocation->Idle())
	{
		VkDeviceSize idleBytes = 0;
		for (const auto& entry : m_allocations)
			if (entry && entry->Idle())
				idleBytes += entry->producer->Descriptor().allocationSize;
		cache = idleBytes <= s_idleBytesLimit;
	}
	if (!cache)
	{
		if (allocation->cached)
			for (auto& entry : m_allocations)
				if (entry == allocation)
				{
					entry.reset();
					break;
				}
		allocation->cached = false;
		retiredProducer = std::move(allocation->producer);
		retiredConsumer = std::move(allocation->consumer);
	}
}

void VulkanImagePool::Close()
{
	std::array<std::shared_ptr<VulkanExternalImage>, 128> retired;
	std::size_t count = 0;
	std::lock_guard lock(m_mutex);
	m_closed = true;
	for (auto& allocation : m_allocations)
		if (allocation)
		{
			allocation->cached = false;
			retired[count++] = std::move(allocation->producer);
			retired[count++] = std::move(allocation->consumer);
			allocation.reset();
		}
}
} // namespace simxr
