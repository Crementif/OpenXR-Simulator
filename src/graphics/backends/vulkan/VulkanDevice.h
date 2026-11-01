#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vulkan/vulkan.h>

namespace simxr
{
class VulkanCommands;

class VulkanError : public std::runtime_error
{
public:
	VulkanError(VkResult result, const char* operation);
	VkResult Result() const noexcept;

private:
	VkResult m_result;
};

void CheckVulkan(VkResult result, const char* operation);

// The runtime shares the application's process, so its own instances must not load
// the implicit layers the application enabled (overlays, capture or mod layers).
VkResult CreateRuntimeVulkanInstance(const VkInstanceCreateInfo& create, VkInstance* instance);

struct VulkanBinding
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	std::uint32_t queueFamily;
	std::uint32_t queueIndex;
};

// Host bindings borrow handles and must drain before host destruction. An
// optional shared native owner extends an inspection device's lifetime.
class VulkanDevice
{
public:
	explicit VulkanDevice(const VulkanBinding& binding, std::shared_ptr<const void> owner = {});
	~VulkanDevice();
	std::unique_ptr<VulkanCommands> AcquireCommands();
	// Return only after GPU completion and the last completion observer have retired.
	void RecycleCommands(std::unique_ptr<VulkanCommands> commands);
	void ClearCommands();
	const VulkanBinding binding;
	VkQueue queue = VK_NULL_HANDLE;
	// Serializes independently owned graphics and video conversion submissions.
	std::mutex queueMutex;
	std::uint32_t encodeFamily = VK_QUEUE_FAMILY_IGNORED;
	std::uint32_t decodeFamily = VK_QUEUE_FAMILY_IGNORED;
	std::uint32_t timestampBits = 0;
	float timestampPeriod = 0;
	std::atomic<bool> lost{false};
	bool HasNativeOwner() const noexcept;
	std::uint32_t MemoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const;

private:
	const std::shared_ptr<const void> m_owner;
	std::mutex m_commandsMutex;
	std::array<std::unique_ptr<VulkanCommands>, 16> m_commands;
	bool m_commandsClosed = false;
};
} // namespace simxr
