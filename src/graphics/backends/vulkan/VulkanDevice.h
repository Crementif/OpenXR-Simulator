#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vulkan/vulkan.h>

namespace simxr
{
class VulkanError : public std::runtime_error
{
  public:
	VulkanError(VkResult result, const char* operation);
	VkResult Result() const noexcept;

  private:
	VkResult m_result;
};
void CheckVulkan(VkResult result, const char* operation);

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
	const VulkanBinding binding;
	VkQueue queue = VK_NULL_HANDLE;
	std::atomic<bool> lost{false};
	std::uint32_t MemoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const;

  private:
	const std::shared_ptr<const void> m_owner;
};
} // namespace simxr
