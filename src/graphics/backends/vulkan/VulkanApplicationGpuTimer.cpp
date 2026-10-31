#include "graphics/backends/vulkan/VulkanApplicationGpuTimer.h"
#include "Pch.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"
#include "graphics/backends/vulkan/VulkanResources.h"

namespace simxr
{
class VulkanApplicationGpuTimer final : public ApplicationGpuTimer
{
	struct Resources
	{
		VulkanNativePreservation<Resources, 32>::Reservation preservation{"Application timing owners exhausted"};
		std::shared_ptr<VulkanDevice> device;
		std::array<std::unique_ptr<VulkanCommands>, SlotCount * 2> commands;
		std::array<bool, SlotCount * 2> submitted{};
	};

public:
	explicit VulkanApplicationGpuTimer(std::shared_ptr<VulkanDevice> device)
		: m_resources(std::make_unique<Resources>())
	{
		m_resources->device = std::move(device);
		const auto& binding = m_resources->device->binding;
		for (std::size_t index = 0; index < SlotCount * 2; ++index)
		{
			auto& commands = m_resources->commands[index];
			commands = std::make_unique<VulkanCommands>(binding.device, binding.queueFamily);
			if (!(index % 2))
			{
				VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
				query.queryType = VK_QUERY_TYPE_TIMESTAMP;
				query.queryCount = 2;
				CheckVulkan(vkCreateQueryPool(binding.device, &query, nullptr, &commands->timestamps),
							"Create application timestamps");
			}
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			CheckVulkan(vkBeginCommandBuffer(commands->command, &begin), "Begin application marker");
			const auto pool = m_resources->commands[index - index % 2]->timestamps;
			if (!(index % 2))
				vkCmdResetQueryPool(commands->command, pool, 0, 2);
			vkCmdWriteTimestamp(commands->command,
								index % 2 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
								pool, index % 2);
			CheckVulkan(vkEndCommandBuffer(commands->command), "End application marker");
		}
	}

	~VulkanApplicationGpuTimer() override
	{
		Collect();
		if (!m_resources->device->lost.load())
			for (std::size_t index = 0; index < m_resources->commands.size(); ++index)
				if (m_resources->submitted[index] &&
					vkGetFenceStatus(m_resources->device->binding.device, m_resources->commands[index]->fence) !=
						VK_SUCCESS)
				{
					m_resources->preservation.Preserve(std::move(m_resources));
					break;
				}
	}

	bool Drain(std::uint64_t timeout) override
	{
		std::vector<VkFence> fences;
		for (std::size_t index = 0; index < m_resources->commands.size(); ++index)
			if (m_resources->submitted[index])
				fences.push_back(m_resources->commands[index]->fence);
		const auto result = fences.empty() ? VK_SUCCESS
										   : vkWaitForFences(m_resources->device->binding.device,
															 static_cast<std::uint32_t>(fences.size()), fences.data(),
															 VK_TRUE, timeout);
		if (result == VK_ERROR_DEVICE_LOST)
			m_resources->device->lost.store(true);
		Collect();
		return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
	}

private:
	bool Write(std::size_t slot, bool end) override
	{
		const auto index = slot * 2 + static_cast<std::size_t>(end);
		auto& resources = *m_resources;
		auto& commands = *resources.commands[index];
		if (resources.device->lost.load())
			return false;
		if (vkResetFences(resources.device->binding.device, 1, &commands.fence) != VK_SUCCESS)
			return false;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands.command;
		std::lock_guard lock(resources.device->queueMutex);
		resources.submitted[index] = true;
		const auto result = vkQueueSubmit(resources.device->queue, 1, &submit, commands.fence);
		if (result == VK_ERROR_DEVICE_LOST)
			resources.device->lost.store(true);
		if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			resources.submitted[index] = false;
		return result == VK_SUCCESS;
	}

	bool Read(std::size_t slot, std::int64_t& nanoseconds) override
	{
		const auto& device = *m_resources->device;
		if (device.lost.load())
			return true;
		for (std::size_t index = slot * 2; index < slot * 2 + 2; ++index)
			if (vkGetFenceStatus(device.binding.device, m_resources->commands[index]->fence) != VK_SUCCESS)
				return false;
		std::array<std::uint64_t, 2> ticks{};
		const auto result =
			vkGetQueryPoolResults(device.binding.device, m_resources->commands[slot * 2]->timestamps, 0, 2,
								  sizeof(ticks), ticks.data(), sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT);
		if (result != VK_SUCCESS)
			return result != VK_NOT_READY;
		const auto mask = device.timestampBits == 64 ? UINT64_MAX : (std::uint64_t{1} << device.timestampBits) - 1;
		const double duration = static_cast<double>((ticks[1] - ticks[0]) & mask) * device.timestampPeriod;
		if (duration >= 0 && duration < static_cast<double>(INT64_MAX))
			nanoseconds = static_cast<std::int64_t>(duration);
		return true;
	}

	std::unique_ptr<Resources> m_resources;
};

std::unique_ptr<ApplicationGpuTimer> CreateVulkanApplicationGpuTimer(std::shared_ptr<VulkanDevice> device)
{
	if (!device->timestampBits || device->timestampPeriod <= 0)
		return {};
	try
	{
		return std::make_unique<VulkanApplicationGpuTimer>(std::move(device));
	}
	catch (const VulkanError&)
	{
		return {};
	}
}
}
