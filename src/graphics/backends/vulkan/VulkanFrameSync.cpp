#include "graphics/backends/vulkan/VulkanFrameSync.h"
#include "Pch.h"

namespace simxr
{
VulkanFrameSync::VulkanFrameSync(std::shared_ptr<VulkanExternalSemaphore> producer) : m_producer(std::move(producer)) {}
VkSemaphore VulkanFrameSync::Signal() const noexcept
{
	return m_producer->Get();
}
std::shared_ptr<VulkanExternalSemaphore> VulkanFrameSync::Import(const VulkanExternalDevice& consumer)
{
	if (m_producer->Device()->lost.load() || consumer.Device()->lost.load() ||
		(m_consumer && m_consumer->Device()->lost.load()))
		throw VulkanError(VK_ERROR_DEVICE_LOST, "Frame synchronization device is lost");
	if (m_consumer && m_consumer->Device() == consumer.Device())
		return m_consumer;
	if (!m_handle)
		m_handle = m_producer->ExportOnce();
	auto handle = m_handle.Duplicate();
	m_consumer = consumer.ImportSemaphore(m_producer->Identity(), handle);
	return m_consumer;
}
void VulkanFrameSync::MarkConsumed() noexcept
{
	m_consumed.store(true);
}

VulkanFrameSyncPool::VulkanFrameSyncPool(std::shared_ptr<VulkanExternalDevice> producer)
	: m_producer(std::move(producer))
{
}
std::shared_ptr<VulkanFrameSync> VulkanFrameSyncPool::Acquire()
{
	const auto pool = shared_from_this();
	std::unique_ptr<VulkanFrameSync> sync;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			throw VulkanError(VK_ERROR_INITIALIZATION_FAILED, "Frame synchronization pool is closed");
		for (auto& available : m_available)
			if (available)
			{
				sync = std::move(available);
				break;
			}
	}
	if (sync && sync->m_consumer->Device()->lost.load())
		sync.reset();
	if (!sync)
		sync.reset(new VulkanFrameSync(m_producer->CreateExportableSemaphore()));
	sync->m_consumed.store(false);
	return std::shared_ptr<VulkanFrameSync>(sync.release(),
											[pool](VulkanFrameSync* retired)
											{
												pool->Recycle(std::unique_ptr<VulkanFrameSync>(retired));
											});
}
void VulkanFrameSyncPool::Recycle(std::unique_ptr<VulkanFrameSync> sync)
{
	if (!sync->m_consumed.load() || m_producer->Device()->lost.load() || !sync->m_consumer ||
		sync->m_consumer->Device()->lost.load())
		return;
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return;
	for (auto& available : m_available)
		if (!available)
		{
			available = std::move(sync);
			return;
		}
}
void VulkanFrameSyncPool::Close()
{
	decltype(m_available) retired;
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		retired.swap(m_available);
	}
}
} // namespace simxr
