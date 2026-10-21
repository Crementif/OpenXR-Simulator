#include "graphics/backends/vulkan/VulkanExternal.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace simxr
{
namespace
{
constexpr auto MemoryHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto SemaphoreHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto FenceHandle = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr std::array SharingNames{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
								  VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
constexpr VkImageUsageFlags ImageUsages = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
										  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

bool SupportedShape(const VulkanExternalImageSpec& image)
{
	return (image.format == VK_FORMAT_R8G8B8A8_SRGB || image.format == VK_FORMAT_R8G8B8A8_UNORM) &&
		   image.extent.width && image.extent.height && image.extent.depth == 1 && image.arrayLayers >= 1 &&
		   image.arrayLayers <= 2 && image.mipLevels == 1 && image.samples == VK_SAMPLE_COUNT_1_BIT &&
		   image.tiling == VK_IMAGE_TILING_OPTIMAL && image.imageType == VK_IMAGE_TYPE_2D && !image.flags &&
		   image.usage && !(image.usage & ~ImageUsages);
}

bool HasName(std::span<const char* const> extensions, const char* name)
{
	return std::any_of(extensions.begin(), extensions.end(),
					   [&](const char* candidate) { return candidate && std::strcmp(candidate, name) == 0; });
}
bool HasNames(std::span<const char* const> extensions)
{
	return std::all_of(SharingNames.begin(), SharingNames.end(),
					   [&](const char* name) { return HasName(extensions, name); });
}

VkResult PhysicalExtensions(VkPhysicalDevice physical, std::span<const char* const> required = SharingNames)
{
	std::uint32_t count = 0;
	auto result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
	if (result != VK_SUCCESS)
		return result;
	for (unsigned attempt = 0; attempt < 4; ++attempt)
	{
		std::vector<VkExtensionProperties> extensions(count);
		result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
		if (result == VK_INCOMPLETE)
		{
			result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
			if (result != VK_SUCCESS)
				return result;
			continue;
		}
		if (result != VK_SUCCESS)
			return result;
		const auto contains = [&](const char* name)
		{
			return std::any_of(extensions.begin(), extensions.begin() + count,
							   [&](const auto& candidate) { return std::strcmp(candidate.extensionName, name) == 0; });
		};
		return std::all_of(required.begin(), required.end(), contains) ? VK_SUCCESS : VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	return VK_INCOMPLETE;
}

VkExternalSemaphoreProperties SemaphoreCapabilities(VkPhysicalDevice physical)
{
	VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
	info.handleType = SemaphoreHandle;
	VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &properties);
	return properties;
}

VulkanExternalIdentity DeviceIdentity(VkPhysicalDevice physical)
{
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
	vkGetPhysicalDeviceProperties2(physical, &properties);
	VulkanExternalIdentity result;
	std::copy_n(identity.deviceUUID, VK_UUID_SIZE, result.deviceUuid.begin());
	std::copy_n(identity.driverUUID, VK_UUID_SIZE, result.driverUuid.begin());
	return result;
}

void RequireLive(const VulkanDevice& device)
{
	if (device.lost.load())
		throw VulkanError(VK_ERROR_DEVICE_LOST, "External sharing device is lost");
}

void CloseOnExec(const UniqueFd& fd)
{
	int flags;
	do
	{
		flags = fcntl(fd.Get(), F_GETFD);
	} while (flags < 0 && errno == EINTR);
	if (flags < 0)
		throw std::system_error(errno, std::generic_category(), "Query exported FD flags");
	int result;
	do
	{
		result = fcntl(fd.Get(), F_SETFD, flags | FD_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::system_error(errno, std::generic_category(), "Set exported FD close-on-exec");
}

VkMemoryRequirements Requirements(const VulkanImage& image)
{
	VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
	VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	info.image = image.image;
	vkGetImageMemoryRequirements2(image.device->binding.device, &info, &requirements);
	// Every allocation in this subset is dedicated, satisfying either preference.
	return requirements.memoryRequirements;
}

std::uint32_t MemoryType(VkPhysicalDevice physical, std::uint32_t bits)
{
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	for (bool local : {true, false})
		for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
		{
			const auto flags = properties.memoryTypes[index].propertyFlags;
			if ((bits & (1u << index)) && !(flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) &&
				(!local || (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
				return index;
		}
	throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "No non-protected external image memory type");
}
} // namespace

UniqueFd::UniqueFd(int descriptor) noexcept : m_descriptor(descriptor) {}
UniqueFd::~UniqueFd()
{
	Reset();
}
UniqueFd::UniqueFd(UniqueFd&& other) noexcept : m_descriptor(other.Release()) {}
UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept
{
	if (this != &other)
		Reset(other.Release());
	return *this;
}
int UniqueFd::Get() const noexcept
{
	return m_descriptor;
}
UniqueFd::operator bool() const noexcept
{
	return m_descriptor >= 0;
}
int UniqueFd::Release() noexcept
{
	return std::exchange(m_descriptor, -1);
}
void UniqueFd::Reset(int descriptor) noexcept
{
	if (m_descriptor == descriptor)
		return;
	const auto previous = std::exchange(m_descriptor, descriptor);
	if (previous >= 0)
	{
		const auto saved = errno;
		// Linux releases the descriptor even when close reports EINTR. Retrying
		// could close a newly reused descriptor owned by another thread.
		close(previous);
		errno = saved;
	}
}
UniqueFd UniqueFd::Duplicate() const
{
	if (!*this)
		throw std::invalid_argument("Cannot duplicate an empty descriptor");
	int result;
	do
	{
		result = fcntl(m_descriptor, F_DUPFD_CLOEXEC, 0);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::system_error(errno, std::generic_category(), "Duplicate external descriptor");
	return UniqueFd(result);
}

bool VulkanExternalCapabilities::CanExportImage() const noexcept
{
	return result == VK_SUCCESS && (memory.compatibleHandleTypes & MemoryHandle) &&
		   (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
}
bool VulkanExternalCapabilities::CanImportImage() const noexcept
{
	return result == VK_SUCCESS && (memory.compatibleHandleTypes & MemoryHandle) &&
		   (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}
bool VulkanExternalCapabilities::CanExportSemaphore() const noexcept
{
	return result == VK_SUCCESS && (semaphore.compatibleHandleTypes & SemaphoreHandle) &&
		   (semaphore.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
}
bool VulkanExternalCapabilities::CanImportSemaphore() const noexcept
{
	return result == VK_SUCCESS && (semaphore.compatibleHandleTypes & SemaphoreHandle) &&
		   (semaphore.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
}

bool VulkanExternalFenceCapabilities::CanExport() const noexcept
{
	return result == VK_SUCCESS && (fence.compatibleHandleTypes & FenceHandle) &&
		   (fence.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT);
}
bool VulkanExternalFenceCapabilities::CanImport() const noexcept
{
	return result == VK_SUCCESS && (fence.compatibleHandleTypes & FenceHandle) &&
		   (fence.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT);
}

VulkanExternalFence::VulkanExternalFence(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
										 PFN_vkGetFenceFdKHR getFenceFd, bool exportable)
	: m_device(std::move(device)), m_identity(identity), m_getFenceFd(getFenceFd), m_exportable(exportable)
{
}
VulkanExternalFence::~VulkanExternalFence()
{
	if (m_fence)
		vkDestroyFence(m_device->binding.device, m_fence, nullptr);
}
VkFence VulkanExternalFence::Get() const noexcept
{
	return m_fence;
}
std::shared_ptr<VulkanDevice> VulkanExternalFence::Device() const noexcept
{
	return m_device;
}
const VulkanExternalIdentity& VulkanExternalFence::Identity() const noexcept
{
	return m_identity;
}
UniqueFd VulkanExternalFence::ExportOnce()
{
	std::lock_guard lock(m_exportMutex);
	RequireLive(*m_device);
	if (!m_exportable || m_exported || !m_getFenceFd)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Fence is unavailable for one-shot export");
	VkFenceGetFdInfoKHR info{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
	info.fence = m_fence;
	info.handleType = FenceHandle;
	int descriptor = -1;
	CheckVulkan(m_getFenceFd(m_device->binding.device, &info, &descriptor), "Export fence FD");
	UniqueFd result(descriptor);
	if (!result)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Fence export returned an invalid descriptor");
	CloseOnExec(result);
	m_exported = true;
	return result;
}
CompletionStatus VulkanExternalFence::PollCompletion() const noexcept
{
	auto status = m_status.load();
	if (status != CompletionStatus::Pending)
		return status;
	const auto native =
		m_device->lost.load() ? VK_ERROR_DEVICE_LOST : vkGetFenceStatus(m_device->binding.device, m_fence);
	if (native == VK_NOT_READY)
		return m_status.load();
	if (native == VK_ERROR_DEVICE_LOST)
		m_device->lost.store(true);
	const auto observed = native == VK_SUCCESS ? CompletionStatus::Complete : CompletionStatus::Failed;
	m_status.compare_exchange_strong(status, observed);
	return m_status.load();
}

VulkanExternalFenceCapabilities VulkanExternalDevice::QueryFence(VkPhysicalDevice physical,
																 std::uint32_t instanceApiVersion)
{
	VulkanExternalFenceCapabilities result;
	if (!physical || instanceApiVersion < VK_API_VERSION_1_1)
		return result;
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(physical, &properties);
	if (properties.apiVersion < VK_API_VERSION_1_1)
		return result;
	constexpr std::array names{VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
	result.result = PhysicalExtensions(physical, names);
	if (result.result != VK_SUCCESS)
		return result;
	VkPhysicalDeviceExternalFenceInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
	info.handleType = FenceHandle;
	vkGetPhysicalDeviceExternalFenceProperties(physical, &info, &result.fence);
	return result;
}

VulkanExternalImage::VulkanExternalImage(std::shared_ptr<VulkanImage> image, VulkanExternalImageDescriptor descriptor,
										 PFN_vkGetMemoryFdKHR getMemoryFd, bool exportable)
	: m_image(std::move(image)), m_descriptor(descriptor), m_getMemoryFd(getMemoryFd), m_exportable(exportable)
{
}
std::shared_ptr<VulkanImage> VulkanExternalImage::Image() const noexcept
{
	return m_image;
}
const VulkanExternalImageDescriptor& VulkanExternalImage::Descriptor() const noexcept
{
	return m_descriptor;
}
UniqueFd VulkanExternalImage::ExportMemory() const
{
	std::lock_guard lock(m_exportMutex);
	RequireLive(*m_image->device);
	if (!m_exportable || !m_getMemoryFd)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Imported image is not export-enabled");
	VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	info.memory = m_image->memory;
	info.handleType = MemoryHandle;
	int descriptor = -1;
	CheckVulkan(m_getMemoryFd(m_image->device->binding.device, &info, &descriptor), "Export dedicated image memory FD");
	UniqueFd result(descriptor);
	if (!result)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Image export returned an invalid descriptor");
	CloseOnExec(result);
	return result;
}

VulkanExternalSemaphore::VulkanExternalSemaphore(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
												 PFN_vkGetSemaphoreFdKHR getSemaphoreFd, bool exportable)
	: m_device(std::move(device)), m_identity(identity), m_getSemaphoreFd(getSemaphoreFd), m_exportable(exportable)
{
}
VulkanExternalSemaphore::~VulkanExternalSemaphore()
{
	if (m_semaphore)
		vkDestroySemaphore(m_device->binding.device, m_semaphore, nullptr);
}
VkSemaphore VulkanExternalSemaphore::Get() const noexcept
{
	return m_semaphore;
}
std::shared_ptr<VulkanDevice> VulkanExternalSemaphore::Device() const noexcept
{
	return m_device;
}
const VulkanExternalIdentity& VulkanExternalSemaphore::Identity() const noexcept
{
	return m_identity;
}
UniqueFd VulkanExternalSemaphore::ExportOnce()
{
	std::lock_guard lock(m_exportMutex);
	RequireLive(*m_device);
	if (!m_exportable || m_exported || !m_getSemaphoreFd)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Binary semaphore is unavailable for one-shot export");
	VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	info.semaphore = m_semaphore;
	info.handleType = SemaphoreHandle;
	int descriptor = -1;
	CheckVulkan(m_getSemaphoreFd(m_device->binding.device, &info, &descriptor), "Export binary semaphore FD");
	UniqueFd result(descriptor);
	if (!result)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Semaphore export returned an invalid descriptor");
	CloseOnExec(result);
	m_exported = true;
	return result;
}

VulkanExternalCapabilities VulkanExternalDevice::Query(VkPhysicalDevice physical, const VulkanExternalImageSpec& image,
													   std::uint32_t instanceApiVersion)
{
	VulkanExternalCapabilities result;
	if (!physical || instanceApiVersion < VK_API_VERSION_1_1)
	{
		result.result = VK_ERROR_FEATURE_NOT_PRESENT;
		return result;
	}
	if (!SupportedShape(image))
		return result;
	VkPhysicalDeviceProperties deviceProperties;
	vkGetPhysicalDeviceProperties(physical, &deviceProperties);
	if (deviceProperties.apiVersion < VK_API_VERSION_1_1)
	{
		result.result = VK_ERROR_FEATURE_NOT_PRESENT;
		return result;
	}
	result.result = PhysicalExtensions(physical);
	if (result.result != VK_SUCCESS)
		return result;
	VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
	external.handleType = MemoryHandle;
	VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external};
	info.format = image.format;
	info.type = image.imageType;
	info.tiling = image.tiling;
	info.usage = image.usage;
	info.flags = image.flags;
	VkExternalImageFormatProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 format{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &properties};
	result.result = vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &format);
	if (result.result != VK_SUCCESS)
		return result;
	result.imageLimits = format.imageFormatProperties;
	result.memory = properties.externalMemoryProperties;
	result.semaphore = SemaphoreCapabilities(physical);
	const auto& limits = result.imageLimits;
	if (image.extent.width > limits.maxExtent.width || image.extent.height > limits.maxExtent.height ||
		image.extent.depth > limits.maxExtent.depth || image.arrayLayers > limits.maxArrayLayers ||
		image.mipLevels > limits.maxMipLevels || !(limits.sampleCounts & image.samples))
		result.result = VK_ERROR_FORMAT_NOT_SUPPORTED;
	return result;
}

VulkanExternalDevice::VulkanExternalDevice(std::shared_ptr<VulkanDevice> device,
										   std::span<const char* const> enabledExtensions,
										   std::uint32_t instanceApiVersion)
	: m_device(std::move(device)), m_instanceApiVersion(instanceApiVersion)
{
	if (!m_device)
		throw std::invalid_argument("External sharing requires a device");
	RequireLive(*m_device);
	if (instanceApiVersion < VK_API_VERSION_1_1)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "External sharing requires a Vulkan 1.1 instance");
	if (!HasNames(enabledExtensions))
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Logical device did not enable both FD sharing extensions");
	CheckVulkan(PhysicalExtensions(m_device->binding.physicalDevice), "Query physical FD sharing extensions");
	const auto native = m_device->binding.device;
	m_getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(native, "vkGetMemoryFdKHR"));
	m_getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(native, "vkGetSemaphoreFdKHR"));
	m_importSemaphoreFd =
		reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(native, "vkImportSemaphoreFdKHR"));
	if (!m_getMemoryFd || !m_getSemaphoreFd || !m_importSemaphoreFd)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Enabled logical-device FD commands are unavailable");
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_device->binding.physicalDevice, &properties);
	if (properties.apiVersion < VK_API_VERSION_1_1)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "External sharing requires Vulkan 1.1 physical support");
	m_identity = DeviceIdentity(m_device->binding.physicalDevice);
	// Fence delivery is optional. Missing/disabled commands affect only fence
	// methods; image and semaphore sharing retain their existing capability.
	if (HasName(enabledExtensions, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME))
	{
		m_getFenceFd = reinterpret_cast<PFN_vkGetFenceFdKHR>(vkGetDeviceProcAddr(native, "vkGetFenceFdKHR"));
		m_importFenceFd = reinterpret_cast<PFN_vkImportFenceFdKHR>(vkGetDeviceProcAddr(native, "vkImportFenceFdKHR"));
	}
}
const VulkanExternalIdentity& VulkanExternalDevice::Identity() const noexcept
{
	return m_identity;
}
std::shared_ptr<VulkanDevice> VulkanExternalDevice::Device() const noexcept
{
	return m_device;
}

std::shared_ptr<VulkanImage> VulkanExternalDevice::CreateImage(const VulkanExternalImageSpec& spec) const
{
	auto result = std::shared_ptr<VulkanImage>(new VulkanImage(m_device));
	result->format = spec.format;
	result->extent = {spec.extent.width, spec.extent.height};
	result->layers = spec.arrayLayers;
	result->usage = spec.usage;
	VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	external.handleTypes = MemoryHandle;
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
	info.flags = spec.flags;
	info.imageType = spec.imageType;
	info.format = spec.format;
	info.extent = spec.extent;
	info.mipLevels = spec.mipLevels;
	info.arrayLayers = spec.arrayLayers;
	info.samples = spec.samples;
	info.tiling = spec.tiling;
	info.usage = spec.usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage image = VK_NULL_HANDLE;
	CheckVulkan(vkCreateImage(m_device->binding.device, &info, nullptr, &image), "Create external two-eye image");
	result->image = image;
	return result;
}

std::shared_ptr<VulkanExternalImage> VulkanExternalDevice::CreateExportable(const VulkanExternalImageSpec& spec,
																			VkImageLayout producerLayout) const
{
	RequireLive(*m_device);
	const auto capabilities = Query(m_device->binding.physicalDevice, spec, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external image shape and format");
	if (!capabilities.CanExportImage() || producerLayout != VK_IMAGE_LAYOUT_GENERAL)
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "External image export or handoff layout unsupported");
	auto image = CreateImage(spec);
	const auto requirements = Requirements(*image);
	VulkanExternalImageDescriptor descriptor;
	descriptor.identity = m_identity;
	descriptor.image = spec;
	descriptor.allocationSize = requirements.size;
	descriptor.memoryTypeIndex = MemoryType(m_device->binding.physicalDevice, requirements.memoryTypeBits);
	descriptor.dedicated = true;
	descriptor.producerQueueFamily = m_device->binding.queueFamily;
	descriptor.producerLayout = producerLayout;
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image->image;
	VkExportMemoryAllocateInfo external{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
	external.handleTypes = MemoryHandle;
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &external};
	allocation.allocationSize = descriptor.allocationSize;
	allocation.memoryTypeIndex = descriptor.memoryTypeIndex;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	CheckVulkan(vkAllocateMemory(m_device->binding.device, &allocation, nullptr, &memory),
				"Allocate dedicated exportable image memory");
	image->memory = memory;
	image->allocationSize = descriptor.allocationSize;
	CheckVulkan(vkBindImageMemory(m_device->binding.device, image->image, image->memory, 0),
				"Bind exportable image allocation");
	return std::shared_ptr<VulkanExternalImage>(
		new VulkanExternalImage(std::move(image), descriptor, m_getMemoryFd, true));
}

std::shared_ptr<VulkanExternalImage> VulkanExternalDevice::ImportImage(const VulkanExternalImageDescriptor& descriptor,
																	   UniqueFd& fd) const
{
	RequireLive(*m_device);
	if (!fd || descriptor.identity != m_identity || !descriptor.dedicated || !descriptor.allocationSize ||
		descriptor.producerLayout != VK_IMAGE_LAYOUT_GENERAL)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External image identity, dedication or layout mismatch");
	const auto capabilities = Query(m_device->binding.physicalDevice, descriptor.image, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external image import shape and format");
	if (!capabilities.CanImportImage())
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "External image FD import unsupported");
	std::uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_device->binding.physicalDevice, &count, nullptr);
	if (descriptor.producerQueueFamily >= count)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External producer queue family mismatch");
	auto image = CreateImage(descriptor.image);
	const auto requirements = Requirements(*image);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(m_device->binding.physicalDevice, &properties);
	if (descriptor.allocationSize != requirements.size || descriptor.memoryTypeIndex >= properties.memoryTypeCount ||
		!(requirements.memoryTypeBits & (1u << descriptor.memoryTypeIndex)) ||
		(properties.memoryTypes[descriptor.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT))
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External image exact allocation parameters mismatch");
	auto imported = fd.Duplicate();
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image->image;
	VkImportMemoryFdInfoKHR external{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dedicated};
	external.handleType = MemoryHandle;
	external.fd = imported.Get();
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &external};
	allocation.allocationSize = descriptor.allocationSize;
	allocation.memoryTypeIndex = descriptor.memoryTypeIndex;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	CheckVulkan(vkAllocateMemory(m_device->binding.device, &allocation, nullptr, &memory),
				"Import exact dedicated image FD");
	imported.Release();
	image->memory = memory;
	image->allocationSize = descriptor.allocationSize;
	CheckVulkan(vkBindImageMemory(m_device->binding.device, image->image, image->memory, 0),
				"Bind imported image allocation");
	auto result =
		std::shared_ptr<VulkanExternalImage>(new VulkanExternalImage(std::move(image), descriptor, nullptr, false));
	fd.Reset();
	return result;
}

std::shared_ptr<VulkanExternalSemaphore> VulkanExternalDevice::CreateExportableSemaphore() const
{
	RequireLive(*m_device);
	const auto properties = SemaphoreCapabilities(m_device->binding.physicalDevice);
	if (!(properties.compatibleHandleTypes & SemaphoreHandle) ||
		!(properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT))
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Binary semaphore FD export unsupported");
	auto result = std::shared_ptr<VulkanExternalSemaphore>(
		new VulkanExternalSemaphore(m_device, m_identity, m_getSemaphoreFd, true));
	VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	external.handleTypes = SemaphoreHandle;
	VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &external};
	VkSemaphore semaphore = VK_NULL_HANDLE;
	CheckVulkan(vkCreateSemaphore(m_device->binding.device, &info, nullptr, &semaphore),
				"Create exportable binary semaphore");
	result->m_semaphore = semaphore;
	return result;
}

std::shared_ptr<VulkanExternalSemaphore> VulkanExternalDevice::ImportSemaphore(const VulkanExternalIdentity& identity,
																			   UniqueFd& fd) const
{
	RequireLive(*m_device);
	if (!fd || identity != m_identity)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External binary semaphore identity mismatch");
	const auto properties = SemaphoreCapabilities(m_device->binding.physicalDevice);
	if (!(properties.compatibleHandleTypes & SemaphoreHandle) ||
		!(properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Binary semaphore FD import unsupported");
	auto result =
		std::shared_ptr<VulkanExternalSemaphore>(new VulkanExternalSemaphore(m_device, m_identity, nullptr, false));
	VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore semaphore = VK_NULL_HANDLE;
	CheckVulkan(vkCreateSemaphore(m_device->binding.device, &create, nullptr, &semaphore),
				"Create imported binary semaphore");
	result->m_semaphore = semaphore;
	auto imported = fd.Duplicate();
	VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	info.semaphore = result->m_semaphore;
	info.handleType = SemaphoreHandle;
	info.fd = imported.Get();
	CheckVulkan(m_importSemaphoreFd(m_device->binding.device, &info), "Import binary semaphore FD");
	imported.Release();
	fd.Reset();
	return result;
}
bool VulkanExternalDevice::FenceCommandsEnabled() const noexcept
{
	return m_getFenceFd && m_importFenceFd;
}
std::shared_ptr<VulkanExternalFence> VulkanExternalDevice::CreateExportableFence() const
{
	RequireLive(*m_device);
	if (!m_getFenceFd || !m_importFenceFd)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Logical device did not enable external fence FD commands");
	const auto capabilities = QueryFence(m_device->binding.physicalDevice, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external fence support");
	if (!capabilities.CanExport())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Fence FD export unsupported");
	auto result =
		std::shared_ptr<VulkanExternalFence>(new VulkanExternalFence(m_device, m_identity, m_getFenceFd, true));
	VkExportFenceCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
	external.handleTypes = FenceHandle;
	VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, &external};
	CheckVulkan(vkCreateFence(m_device->binding.device, &info, nullptr, &result->m_fence), "Create exportable fence");
	return result;
}
std::shared_ptr<VulkanExternalFence> VulkanExternalDevice::ImportFence(const VulkanExternalIdentity& identity,
																	   UniqueFd& fd) const
{
	RequireLive(*m_device);
	if (!fd || identity != m_identity)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External fence identity mismatch");
	if (!m_getFenceFd || !m_importFenceFd)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Logical device did not enable external fence FD commands");
	const auto capabilities = QueryFence(m_device->binding.physicalDevice, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external fence support");
	if (!capabilities.CanImport())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Fence FD import unsupported");
	auto result = std::shared_ptr<VulkanExternalFence>(new VulkanExternalFence(m_device, m_identity, nullptr, false));
	VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(m_device->binding.device, &create, nullptr, &result->m_fence), "Create imported fence");
	auto imported = fd.Duplicate();
	VkImportFenceFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
	info.fence = result->m_fence;
	info.handleType = FenceHandle;
	info.fd = imported.Get();
	CheckVulkan(m_importFenceFd(m_device->binding.device, &info), "Import fence FD");
	imported.Release();
	fd.Reset();
	return result;
}

} // namespace simxr
