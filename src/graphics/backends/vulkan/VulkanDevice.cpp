#include "graphics/backends/vulkan/VulkanDevice.h"

#include "Pch.h"
#include "graphics/backends/vulkan/VulkanResources.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace simxr
{
VulkanError::VulkanError(VkResult result, const char* operation)
	: std::runtime_error(std::string(operation) + ": Vulkan result " + std::to_string(result)), m_result(result)
{}

VkResult VulkanError::Result() const noexcept
{
	return m_result;
}

void CheckVulkan(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS)
		throw VulkanError(result, operation);
}

static std::optional<std::string> ReadEnvironment(const char* name)
{
#ifdef _WIN32
	const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
	if (!size)
		return std::nullopt;
	std::string value(size, '\0');
	value.resize(GetEnvironmentVariableA(name, value.data(), size));
	return value;
#else
	const char* value = std::getenv(name);
	return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

static void WriteEnvironment(const char* name, const std::optional<std::string>& value)
{
#ifdef _WIN32
	SetEnvironmentVariableA(name, value ? value->c_str() : nullptr);
#else
	if (value)
		setenv(name, value->c_str(), 1);
	else
		unsetenv(name);
#endif
}

VkResult CreateRuntimeVulkanInstance(const VkInstanceCreateInfo& create, VkInstance* instance)
{
	static std::mutex mutex;
	static constexpr const char* filter = "VK_LOADER_LAYERS_DISABLE";
	std::scoped_lock lock(mutex);
	const auto previous = ReadEnvironment(filter);
	WriteEnvironment(filter, previous && !previous->empty() ? *previous + ",~implicit~" : std::string("~implicit~"));
	const auto result = vkCreateInstance(&create, nullptr, instance);
	WriteEnvironment(filter, previous);
	return result;
}

VulkanDevice::VulkanDevice(const VulkanBinding& value, std::shared_ptr<const void> owner)
	: binding(value), m_owner(std::move(owner))
{
	if (!binding.instance || !binding.physicalDevice || !binding.device)
		throw std::invalid_argument("Vulkan binding requires instance, physical device and device");
	std::uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, families.data());
	if (binding.queueFamily >= count || binding.queueIndex >= families[binding.queueFamily].queueCount ||
		!(families[binding.queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		throw std::invalid_argument("Vulkan binding requires a valid graphics queue");
	timestampBits = families[binding.queueFamily].timestampValidBits;
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(binding.physicalDevice, &properties);
	timestampPeriod = properties.limits.timestampPeriod;
	vkGetDeviceQueue(binding.device, binding.queueFamily, binding.queueIndex, &queue);
	if (!queue)
		throw std::invalid_argument("Vulkan binding queue was not enabled by the application");
}

VulkanDevice::~VulkanDevice()
{
	ClearCommands();
}

std::unique_ptr<VulkanCommands> VulkanDevice::AcquireCommands()
{
	std::unique_ptr<VulkanCommands> commands;
	{
		std::lock_guard lock(m_commandsMutex);
		for (auto& available : m_commands)
			if (available)
			{
				commands = std::move(available);
				break;
			}
	}
	if (commands)
		commands->Reset();
	else
		commands = std::make_unique<VulkanCommands>(binding.device, binding.queueFamily);
	return commands;
}

void VulkanDevice::RecycleCommands(std::unique_ptr<VulkanCommands> commands)
{
	std::lock_guard lock(m_commandsMutex);
	if (m_commandsClosed || lost.load())
		return;
	for (auto& available : m_commands)
		if (!available)
		{
			available = std::move(commands);
			return;
		}
}

void VulkanDevice::ClearCommands()
{
	decltype(m_commands) retired;
	{
		std::lock_guard lock(m_commandsMutex);
		m_commandsClosed = true;
		retired.swap(m_commands);
	}
}

bool VulkanDevice::HasNativeOwner() const noexcept
{
	return static_cast<bool>(m_owner);
}

std::uint32_t VulkanDevice::MemoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const
{
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(binding.physicalDevice, &properties);
	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
		if ((bits & (1u << index)) && (properties.memoryTypes[index].propertyFlags & required) == required)
			return index;
	throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "No compatible Vulkan memory type");
}
} // namespace simxr
