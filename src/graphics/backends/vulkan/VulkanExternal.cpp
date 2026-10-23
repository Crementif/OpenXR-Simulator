#include "graphics/backends/vulkan/VulkanExternal.h"
#include "Pch.h"

#include <bit>
#include <cerrno>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <stdexcept>
#include <system_error>

namespace simxr
{
#ifdef _WIN32
static constexpr auto s_getMemoryCommand = "vkGetMemoryWin32HandleKHR";
static constexpr auto s_getSemaphoreCommand = "vkGetSemaphoreWin32HandleKHR";
static constexpr auto s_importSemaphoreCommand = "vkImportSemaphoreWin32HandleKHR";
static constexpr auto s_getFenceCommand = "vkGetFenceWin32HandleKHR";
static constexpr auto s_importFenceCommand = "vkImportFenceWin32HandleKHR";
static constexpr auto s_memoryHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static constexpr auto s_semaphoreHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static constexpr auto s_fenceHandle = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
static constexpr auto s_getMemoryCommand = "vkGetMemoryFdKHR";
static constexpr auto s_getSemaphoreCommand = "vkGetSemaphoreFdKHR";
static constexpr auto s_importSemaphoreCommand = "vkImportSemaphoreFdKHR";
static constexpr auto s_getFenceCommand = "vkGetFenceFdKHR";
static constexpr auto s_importFenceCommand = "vkImportFenceFdKHR";
static constexpr auto s_memoryHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
static constexpr auto s_semaphoreHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
static constexpr auto s_fenceHandle = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
static constexpr std::array s_sharingNames{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension};
static constexpr VkImageUsageFlags s_imageUsages = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
												   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static bool SupportedShape(const VulkanExternalImageSpec& image)
{
	return (image.format == VK_FORMAT_R8G8B8A8_SRGB || image.format == VK_FORMAT_R8G8B8A8_UNORM ||
			image.format == VK_FORMAT_B8G8R8A8_SRGB || image.format == VK_FORMAT_B8G8R8A8_UNORM ||
			image.format == VK_FORMAT_R16G16B16A16_SFLOAT || image.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
			image.format == VK_FORMAT_R32G32B32A32_SFLOAT) &&
		   image.extent.width && image.extent.height && image.extent.depth == 1 && image.arrayLayers >= 1 &&
		   image.arrayLayers <= 2 && image.mipLevels >= 1 &&
		   image.mipLevels <=
			   static_cast<std::uint32_t>(std::bit_width(std::max(image.extent.width, image.extent.height))) &&
		   image.samples == VK_SAMPLE_COUNT_1_BIT && image.tiling == VK_IMAGE_TILING_OPTIMAL &&
		   image.imageType == VK_IMAGE_TYPE_2D && !image.flags && image.usage && !(image.usage & ~s_imageUsages);
}

static bool HasName(std::span<const char* const> extensions, const char* name)
{
	return std::any_of(extensions.begin(), extensions.end(),
					   [&](const char* candidate)
					   {
						   return candidate && std::strcmp(candidate, name) == 0;
					   });
}
static bool HasNames(std::span<const char* const> extensions)
{
	return std::all_of(s_sharingNames.begin(), s_sharingNames.end(),
					   [&](const char* name)
					   {
						   return HasName(extensions, name);
					   });
}

static VkResult PhysicalExtensions(VkPhysicalDevice physical, std::span<const char* const> required = s_sharingNames)
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
							   [&](const auto& candidate)
							   {
								   return std::strcmp(candidate.extensionName, name) == 0;
							   });
		};
		return std::all_of(required.begin(), required.end(), contains) ? VK_SUCCESS : VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	return VK_INCOMPLETE;
}

static VkExternalSemaphoreProperties SemaphoreCapabilities(VkPhysicalDevice physical)
{
	VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
	info.handleType = s_semaphoreHandle;
	VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &properties);
	return properties;
}

static VulkanExternalIdentity DeviceIdentity(VkPhysicalDevice physical)
{
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
	vkGetPhysicalDeviceProperties2(physical, &properties);
	VulkanExternalIdentity result;
	std::copy_n(identity.deviceUUID, VK_UUID_SIZE, result.deviceUuid.begin());
	std::copy_n(identity.driverUUID, VK_UUID_SIZE, result.driverUuid.begin());
	return result;
}

static void RequireLive(const VulkanDevice& device)
{
	if (device.lost.load())
		throw VulkanError(VK_ERROR_DEVICE_LOST, "External sharing device is lost");
}

static void PreventInheritance(const VulkanExternalHandle& handle)
{
#ifdef _WIN32
	if (!SetHandleInformation(handle.Get(), HANDLE_FLAG_INHERIT, 0))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
								"Disable exported handle inheritance");
#else
	int flags;
	do
	{
		flags = fcntl(handle.Get(), F_GETFD);
	} while (flags < 0 && errno == EINTR);
	if (flags < 0)
		throw std::system_error(errno, std::generic_category(), "Query exported native-handle flags");
	int result;
	do
	{
		result = fcntl(handle.Get(), F_SETFD, flags | FD_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::system_error(errno, std::generic_category(), "Set exported native-handle close-on-exec");
#endif
}

static VkMemoryRequirements Requirements(const VulkanImage& image)
{
	VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
	VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	info.image = image.image;
	vkGetImageMemoryRequirements2(image.device->binding.device, &info, &requirements);
	// Every allocation in this subset is dedicated, satisfying either preference.
	return requirements.memoryRequirements;
}

static std::uint32_t MemoryType(VkPhysicalDevice physical, std::uint32_t bits)
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

VulkanExternalHandle::VulkanExternalHandle(VulkanNativeHandle descriptor) noexcept : m_descriptor(descriptor) {}
VulkanExternalHandle::~VulkanExternalHandle()
{
	Reset();
}
VulkanExternalHandle::VulkanExternalHandle(VulkanExternalHandle&& other) noexcept : m_descriptor(other.Release()) {}
VulkanExternalHandle& VulkanExternalHandle::operator=(VulkanExternalHandle&& other) noexcept
{
	if (this != &other)
		Reset(other.Release());
	return *this;
}
VulkanNativeHandle VulkanExternalHandle::Get() const noexcept
{
	return m_descriptor;
}
VulkanExternalHandle::operator bool() const noexcept
{
#ifdef _WIN32
	return m_descriptor && m_descriptor != INVALID_HANDLE_VALUE;
#else
	return m_descriptor >= 0;
#endif
}
VulkanNativeHandle VulkanExternalHandle::Release() noexcept
{
	return std::exchange(m_descriptor, InvalidVulkanHandle);
}
void VulkanExternalHandle::Reset(VulkanNativeHandle descriptor) noexcept
{
	if (m_descriptor == descriptor)
		return;
	const auto previous = std::exchange(m_descriptor, descriptor);
#ifdef _WIN32
	if (previous && previous != INVALID_HANDLE_VALUE)
		CloseHandle(previous);
#else
	if (previous >= 0)
	{
		const auto saved = errno;
		// Linux releases the descriptor even when close reports EINTR. Retrying
		// could close a newly reused descriptor owned by another thread.
		close(previous);
		errno = saved;
	}
#endif
}
VulkanExternalHandle VulkanExternalHandle::Duplicate() const
{
	if (!*this)
		throw std::invalid_argument("Cannot duplicate an empty descriptor");
#ifdef _WIN32
	HANDLE result = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), m_descriptor, GetCurrentProcess(), &result, 0, FALSE,
						 DUPLICATE_SAME_ACCESS))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Duplicate external handle");
#else
	int result;
	do
	{
		result = fcntl(m_descriptor, F_DUPFD_CLOEXEC, 0);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::system_error(errno, std::generic_category(), "Duplicate external descriptor");
#endif
	return VulkanExternalHandle(result);
}

bool VulkanExternalCapabilities::CanExportImage() const noexcept
{
	return result == VK_SUCCESS && (memory.compatibleHandleTypes & s_memoryHandle) &&
		   (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
}
bool VulkanExternalCapabilities::CanImportImage() const noexcept
{
	return result == VK_SUCCESS && (memory.compatibleHandleTypes & s_memoryHandle) &&
		   (memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}
bool VulkanExternalCapabilities::CanExportSemaphore() const noexcept
{
	return result == VK_SUCCESS && (semaphore.compatibleHandleTypes & s_semaphoreHandle) &&
		   (semaphore.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
}
bool VulkanExternalCapabilities::CanImportSemaphore() const noexcept
{
	return result == VK_SUCCESS && (semaphore.compatibleHandleTypes & s_semaphoreHandle) &&
		   (semaphore.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
}

bool VulkanExternalFenceCapabilities::CanExport() const noexcept
{
	return result == VK_SUCCESS && (fence.compatibleHandleTypes & s_fenceHandle) &&
		   (fence.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT);
}
bool VulkanExternalFenceCapabilities::CanImport() const noexcept
{
	return result == VK_SUCCESS && (fence.compatibleHandleTypes & s_fenceHandle) &&
		   (fence.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT);
}

VulkanExternalFence::VulkanExternalFence(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
										 VulkanGetFenceHandle getFenceHandle, bool exportable)
	: m_device(std::move(device)), m_identity(identity), m_getFenceHandle(getFenceHandle), m_exportable(exportable)
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
VulkanExternalHandle VulkanExternalFence::ExportOnce()
{
	std::lock_guard lock(m_exportMutex);
	RequireLive(*m_device);
	if (!m_exportable || m_exported || !m_getFenceHandle)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Fence is unavailable for one-shot export");
#ifdef _WIN32
	VkFenceGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_FENCE_GET_WIN32_HANDLE_INFO_KHR};
#else
	VkFenceGetFdInfoKHR info{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
#endif
	info.fence = m_fence;
	info.handleType = s_fenceHandle;
	VulkanNativeHandle descriptor = InvalidVulkanHandle;
	CheckVulkan(m_getFenceHandle(m_device->binding.device, &info, &descriptor), "Export fence native handle");
	VulkanExternalHandle result(descriptor);
	if (!result)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Fence export returned an invalid descriptor");
	PreventInheritance(result);
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
	constexpr std::array names{VulkanExternalFenceExtension};
	result.result = PhysicalExtensions(physical, names);
	if (result.result != VK_SUCCESS)
		return result;
	VkPhysicalDeviceExternalFenceInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
	info.handleType = s_fenceHandle;
	vkGetPhysicalDeviceExternalFenceProperties(physical, &info, &result.fence);
	return result;
}

VulkanExternalImage::VulkanExternalImage(std::shared_ptr<VulkanImage> image, VulkanExternalImageDescriptor descriptor,
										 VulkanGetMemoryHandle getMemoryHandle, bool exportable)
	: m_image(std::move(image)), m_descriptor(descriptor), m_getMemoryHandle(getMemoryHandle), m_exportable(exportable)
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
VulkanExternalHandle VulkanExternalImage::ExportMemory() const
{
	std::lock_guard lock(m_exportMutex);
	RequireLive(*m_image->device);
	if (!m_exportable || !m_getMemoryHandle)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Imported image is not export-enabled");
#ifdef _WIN32
	VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
#else
	VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
#endif
	info.memory = m_image->memory;
	info.handleType = s_memoryHandle;
	VulkanNativeHandle descriptor = InvalidVulkanHandle;
	CheckVulkan(m_getMemoryHandle(m_image->device->binding.device, &info, &descriptor),
				"Export dedicated image memory native handle");
	VulkanExternalHandle result(descriptor);
	if (!result)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Image export returned an invalid descriptor");
	PreventInheritance(result);
	return result;
}

VulkanExternalSemaphore::VulkanExternalSemaphore(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
												 VulkanGetSemaphoreHandle getSemaphoreHandle, bool exportable)
	: m_device(std::move(device)), m_identity(identity), m_getSemaphoreHandle(getSemaphoreHandle),
	  m_exportable(exportable)
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
VulkanExternalHandle VulkanExternalSemaphore::ExportOnce()
{
	std::lock_guard lock(m_exportMutex);
	RequireLive(*m_device);
	if (!m_exportable || m_exported || !m_getSemaphoreHandle)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Semaphore is unavailable for one-shot export");
#ifdef _WIN32
	VkSemaphoreGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
#else
	VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
#endif
	info.semaphore = m_semaphore;
	info.handleType = s_semaphoreHandle;
	VulkanNativeHandle descriptor = InvalidVulkanHandle;
	CheckVulkan(m_getSemaphoreHandle(m_device->binding.device, &info, &descriptor),
				"Export binary semaphore native handle");
	VulkanExternalHandle result(descriptor);
	if (!result)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Semaphore export returned an invalid descriptor");
	PreventInheritance(result);
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
	external.handleType = s_memoryHandle;
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
										   std::uint32_t instanceApiVersion, bool timelineEnabled)
	: m_device(std::move(device)), m_instanceApiVersion(instanceApiVersion)
{
	if (!m_device)
		throw std::invalid_argument("External sharing requires a device");
	RequireLive(*m_device);
	if (instanceApiVersion < VK_API_VERSION_1_1)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "External sharing requires a Vulkan 1.1 instance");
	if (!HasNames(enabledExtensions))
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT,
						  "Logical device did not enable both native-handle sharing extensions");
	CheckVulkan(PhysicalExtensions(m_device->binding.physicalDevice),
				"Query physical native-handle sharing extensions");
	const auto native = m_device->binding.device;
	m_getMemoryHandle = reinterpret_cast<VulkanGetMemoryHandle>(vkGetDeviceProcAddr(native, s_getMemoryCommand));
	m_getSemaphoreHandle =
		reinterpret_cast<VulkanGetSemaphoreHandle>(vkGetDeviceProcAddr(native, s_getSemaphoreCommand));
	m_importSemaphoreHandle =
		reinterpret_cast<VulkanImportSemaphoreHandle>(vkGetDeviceProcAddr(native, s_importSemaphoreCommand));
	if (!m_getMemoryHandle || !m_getSemaphoreHandle || !m_importSemaphoreHandle)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT,
						  "Enabled logical-device native-handle commands are unavailable");
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_device->binding.physicalDevice, &properties);
	if (properties.apiVersion < VK_API_VERSION_1_1)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "External sharing requires Vulkan 1.1 physical support");
	m_identity = DeviceIdentity(m_device->binding.physicalDevice);
	// Fence delivery is optional. Missing/disabled commands affect only fence
	// methods; image and semaphore sharing retain their existing capability.
	if (HasName(enabledExtensions, VulkanExternalFenceExtension))
	{
		m_getFenceHandle = reinterpret_cast<VulkanGetFenceHandle>(vkGetDeviceProcAddr(native, s_getFenceCommand));
		m_importFenceHandle =
			reinterpret_cast<VulkanImportFenceHandle>(vkGetDeviceProcAddr(native, s_importFenceCommand));
	}
	if (timelineEnabled)
	{
		const bool coreTimeline =
			instanceApiVersion >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2;
		if (!HasName(enabledExtensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) && !coreTimeline)
			throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Timeline completion was not enabled");
		m_counter = reinterpret_cast<PFN_vkGetSemaphoreCounterValueKHR>(
			vkGetDeviceProcAddr(native, coreTimeline ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR"));
		if (!m_counter)
			throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Timeline completion command unavailable");
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
	result->mipLevels = spec.mipLevels;
	result->usage = spec.usage;
	VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	external.handleTypes = s_memoryHandle;
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
	external.handleTypes = s_memoryHandle;
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
		new VulkanExternalImage(std::move(image), descriptor, m_getMemoryHandle, true));
}

std::shared_ptr<VulkanExternalImage> VulkanExternalDevice::ImportImage(const VulkanExternalImageDescriptor& descriptor,
																	   VulkanExternalHandle& handle) const
{
	RequireLive(*m_device);
	if (!handle || descriptor.identity != m_identity || !descriptor.dedicated || !descriptor.allocationSize ||
		descriptor.producerLayout != VK_IMAGE_LAYOUT_GENERAL)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External image identity, dedication or layout mismatch");
	const auto capabilities = Query(m_device->binding.physicalDevice, descriptor.image, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external image import shape and format");
	if (!capabilities.CanImportImage())
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "External image native-handle import unsupported");
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
	auto imported = handle.Duplicate();
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image->image;
#ifdef _WIN32
	VkImportMemoryWin32HandleInfoKHR external{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
#else
	VkImportMemoryFdInfoKHR external{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dedicated};
#endif
	external.handleType = s_memoryHandle;
#ifdef _WIN32
	external.handle = imported.Get();
#else
	external.fd = imported.Get();
#endif
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &external};
	allocation.allocationSize = descriptor.allocationSize;
	allocation.memoryTypeIndex = descriptor.memoryTypeIndex;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	CheckVulkan(vkAllocateMemory(m_device->binding.device, &allocation, nullptr, &memory),
				"Import exact dedicated image native handle");
#ifndef _WIN32
	imported.Release(); // Successful FD import transfers ownership to Vulkan.
#endif
	image->memory = memory;
	image->allocationSize = descriptor.allocationSize;
	CheckVulkan(vkBindImageMemory(m_device->binding.device, image->image, image->memory, 0),
				"Bind imported image allocation");
	auto result =
		std::shared_ptr<VulkanExternalImage>(new VulkanExternalImage(std::move(image), descriptor, nullptr, false));
	handle.Reset();
	return result;
}

std::shared_ptr<VulkanExternalSemaphore> VulkanExternalDevice::CreateExportableSemaphore() const
{
	return CreateSemaphore(false);
}
std::shared_ptr<VulkanExternalSemaphore> VulkanExternalDevice::CreateSemaphore(bool timeline) const
{
	RequireLive(*m_device);
	if (timeline && !m_counter)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Timeline feature was not enabled on this device");
	const auto properties = timeline ? QueryTimeline(m_device->binding.physicalDevice, m_instanceApiVersion).semaphore
									 : SemaphoreCapabilities(m_device->binding.physicalDevice);
	if (!(properties.compatibleHandleTypes & s_semaphoreHandle) ||
		!(properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT))
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Semaphore native-handle export unsupported");
	auto result = std::shared_ptr<VulkanExternalSemaphore>(
		new VulkanExternalSemaphore(m_device, m_identity, m_getSemaphoreHandle, true));
	VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	external.handleTypes = s_semaphoreHandle;
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	external.pNext = timeline ? &type : nullptr;
	VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &external};
	VkSemaphore semaphore = VK_NULL_HANDLE;
	CheckVulkan(vkCreateSemaphore(m_device->binding.device, &info, nullptr, &semaphore), "Create exportable semaphore");
	result->m_semaphore = semaphore;
	return result;
}

std::shared_ptr<VulkanExternalSemaphore> VulkanExternalDevice::ImportSemaphore(const VulkanExternalIdentity& identity,
																			   VulkanExternalHandle& handle) const
{
	return ImportSemaphore(identity, handle, false);
}
std::shared_ptr<VulkanExternalSemaphore> VulkanExternalDevice::ImportSemaphore(const VulkanExternalIdentity& identity,
																			   VulkanExternalHandle& handle,
																			   bool timeline) const
{
	RequireLive(*m_device);
	if (!handle || identity != m_identity)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External semaphore identity mismatch");
	if (timeline && !m_counter)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Timeline feature was not enabled on this device");
	const auto properties = timeline ? QueryTimeline(m_device->binding.physicalDevice, m_instanceApiVersion).semaphore
									 : SemaphoreCapabilities(m_device->binding.physicalDevice);
	if (!(properties.compatibleHandleTypes & s_semaphoreHandle) ||
		!(properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Semaphore native-handle import unsupported");
	auto result =
		std::shared_ptr<VulkanExternalSemaphore>(new VulkanExternalSemaphore(m_device, m_identity, nullptr, false));
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, timeline ? &type : nullptr};
	VkSemaphore semaphore = VK_NULL_HANDLE;
	CheckVulkan(vkCreateSemaphore(m_device->binding.device, &create, nullptr, &semaphore), "Create imported semaphore");
	result->m_semaphore = semaphore;
	auto imported = handle.Duplicate();
#ifdef _WIN32
	VkImportSemaphoreWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
#else
	VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
#endif
	info.semaphore = result->m_semaphore;
	info.handleType = s_semaphoreHandle;
#ifdef _WIN32
	info.handle = imported.Get();
#else
	info.fd = imported.Get();
#endif
	CheckVulkan(m_importSemaphoreHandle(m_device->binding.device, &info), "Import semaphore native handle");
#ifndef _WIN32
	imported.Release(); // Successful FD import transfers ownership to Vulkan.
#endif
	handle.Reset();
	return result;
}
VulkanExternalCapabilities VulkanExternalDevice::QueryTimeline(VkPhysicalDevice physical, std::uint32_t api)
{
	VulkanExternalCapabilities result;
	if (!physical || api < VK_API_VERSION_1_1)
		return result;
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(physical, &properties);
	if (properties.apiVersion < VK_API_VERSION_1_1)
	{
		result.result = VK_ERROR_FEATURE_NOT_PRESENT;
		return result;
	}
	constexpr std::array names{VulkanExternalSemaphoreExtension, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
	const bool coreTimeline = api >= VK_API_VERSION_1_2 && properties.apiVersion >= VK_API_VERSION_1_2;
	result.result = PhysicalExtensions(physical, std::span(names).first(coreTimeline ? 1 : names.size()));
	if (result.result != VK_SUCCESS)
		return result;
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
	vkGetPhysicalDeviceFeatures2(physical, &features);
	if (!timeline.timelineSemaphore)
	{
		result.result = VK_ERROR_FEATURE_NOT_PRESENT;
		return result;
	}
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
	info.handleType = s_semaphoreHandle;
	vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &result.semaphore);
	return result;
}
bool VulkanExternalDevice::TimelineCommandsEnabled() const noexcept
{
	return m_counter != nullptr;
}
std::shared_ptr<VulkanExternalTimeline> VulkanExternalDevice::CreateExportableTimeline() const
{
	return std::shared_ptr<VulkanExternalTimeline>(new VulkanExternalTimeline(CreateSemaphore(true), m_counter));
}
std::shared_ptr<VulkanExternalTimeline> VulkanExternalDevice::ImportTimeline(const VulkanExternalIdentity& identity,
																			 VulkanExternalHandle& handle) const
{
	// Allocate the wrapper before consuming caller ownership.
	auto duplicate = handle.Duplicate();
	auto result = std::shared_ptr<VulkanExternalTimeline>(
		new VulkanExternalTimeline(ImportSemaphore(identity, duplicate, true), m_counter));
	handle.Reset();
	return result;
}
VulkanExternalTimeline::VulkanExternalTimeline(std::shared_ptr<VulkanExternalSemaphore> semaphore,
											   PFN_vkGetSemaphoreCounterValueKHR counter)
	: m_semaphore(std::move(semaphore)), m_counter(counter)
{
}
VkSemaphore VulkanExternalTimeline::Get() const noexcept
{
	return m_semaphore->Get();
}
VulkanExternalHandle VulkanExternalTimeline::ExportOnce()
{
	return m_semaphore->ExportOnce();
}
CompletionStatus VulkanExternalTimeline::PollCompletion() const noexcept
{
	auto status = m_status.load();
	if (status != CompletionStatus::Pending)
		return status;
	const auto device = m_semaphore->Device();
	std::uint64_t value = 0;
	const auto result = device->lost.load() ? VK_ERROR_DEVICE_LOST : m_counter(device->binding.device, Get(), &value);
	if (result == VK_SUCCESS && value == 0)
		return m_status.load();
	if (result == VK_ERROR_DEVICE_LOST)
		device->lost.store(true);
	const auto observed = result == VK_SUCCESS && value == 1 ? CompletionStatus::Complete : CompletionStatus::Failed;
	m_status.compare_exchange_strong(status, observed);
	return m_status.load();
}

bool VulkanExternalDevice::FenceCommandsEnabled() const noexcept
{
	return m_getFenceHandle && m_importFenceHandle;
}
std::shared_ptr<VulkanExternalFence> VulkanExternalDevice::CreateExportableFence() const
{
	RequireLive(*m_device);
	if (!m_getFenceHandle || !m_importFenceHandle)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT,
						  "Logical device did not enable external fence native-handle commands");
	const auto capabilities = QueryFence(m_device->binding.physicalDevice, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external fence support");
	if (!capabilities.CanExport())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Fence native-handle export unsupported");
	auto result =
		std::shared_ptr<VulkanExternalFence>(new VulkanExternalFence(m_device, m_identity, m_getFenceHandle, true));
	VkExportFenceCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
	external.handleTypes = s_fenceHandle;
	VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, &external};
	CheckVulkan(vkCreateFence(m_device->binding.device, &info, nullptr, &result->m_fence), "Create exportable fence");
	return result;
}
std::shared_ptr<VulkanExternalFence> VulkanExternalDevice::ImportFence(const VulkanExternalIdentity& identity,
																	   VulkanExternalHandle& handle) const
{
	RequireLive(*m_device);
	if (!handle || identity != m_identity)
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "External fence identity mismatch");
	if (!m_getFenceHandle || !m_importFenceHandle)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT,
						  "Logical device did not enable external fence native-handle commands");
	const auto capabilities = QueryFence(m_device->binding.physicalDevice, m_instanceApiVersion);
	CheckVulkan(capabilities.result, "Query external fence support");
	if (!capabilities.CanImport())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Fence native-handle import unsupported");
	auto result = std::shared_ptr<VulkanExternalFence>(new VulkanExternalFence(m_device, m_identity, nullptr, false));
	VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(m_device->binding.device, &create, nullptr, &result->m_fence), "Create imported fence");
	auto imported = handle.Duplicate();
#ifdef _WIN32
	VkImportFenceWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_FENCE_WIN32_HANDLE_INFO_KHR};
#else
	VkImportFenceFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
#endif
	info.fence = result->m_fence;
	info.handleType = s_fenceHandle;
#ifdef _WIN32
	info.handle = imported.Get();
#else
	info.fd = imported.Get();
#endif
	CheckVulkan(m_importFenceHandle(m_device->binding.device, &info), "Import fence native handle");
#ifndef _WIN32
	imported.Release(); // Successful FD import transfers ownership to Vulkan.
#endif
	handle.Reset();
	return result;
}

} // namespace simxr
