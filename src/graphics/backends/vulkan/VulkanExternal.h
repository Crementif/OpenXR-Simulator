#pragma once

#include "graphics/GpuCompletion.h"
#include "graphics/backends/vulkan/VulkanResources.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#ifdef _WIN32
#include <windows.h>

#include <vulkan/vulkan_win32.h>
#endif

namespace simxr
{
#ifdef _WIN32
using VulkanNativeHandle = HANDLE;
inline constexpr VulkanNativeHandle InvalidVulkanHandle = nullptr;
inline constexpr auto VulkanExternalMemoryExtension = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
inline constexpr auto VulkanExternalSemaphoreExtension = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
inline constexpr auto VulkanExternalFenceExtension = VK_KHR_EXTERNAL_FENCE_WIN32_EXTENSION_NAME;
using VulkanGetMemoryHandle = PFN_vkGetMemoryWin32HandleKHR;
using VulkanGetSemaphoreHandle = PFN_vkGetSemaphoreWin32HandleKHR;
using VulkanImportSemaphoreHandle = PFN_vkImportSemaphoreWin32HandleKHR;
using VulkanGetFenceHandle = PFN_vkGetFenceWin32HandleKHR;
using VulkanImportFenceHandle = PFN_vkImportFenceWin32HandleKHR;
#else
using VulkanNativeHandle = int;
inline constexpr VulkanNativeHandle InvalidVulkanHandle = -1;
inline constexpr auto VulkanExternalMemoryExtension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
inline constexpr auto VulkanExternalSemaphoreExtension = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
inline constexpr auto VulkanExternalFenceExtension = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME;
using VulkanGetMemoryHandle = PFN_vkGetMemoryFdKHR;
using VulkanGetSemaphoreHandle = PFN_vkGetSemaphoreFdKHR;
using VulkanImportSemaphoreHandle = PFN_vkImportSemaphoreFdKHR;
using VulkanGetFenceHandle = PFN_vkGetFenceFdKHR;
using VulkanImportFenceHandle = PFN_vkImportFenceFdKHR;
#endif
class VulkanExternalHandle
{
  public:
	explicit VulkanExternalHandle(VulkanNativeHandle descriptor = InvalidVulkanHandle) noexcept;
	~VulkanExternalHandle();
	VulkanExternalHandle(VulkanExternalHandle&& other) noexcept;
	VulkanExternalHandle& operator=(VulkanExternalHandle&& other) noexcept;
	VulkanExternalHandle(const VulkanExternalHandle&) = delete;
	VulkanExternalHandle& operator=(const VulkanExternalHandle&) = delete;
	VulkanNativeHandle Get() const noexcept;
	explicit operator bool() const noexcept;
	VulkanNativeHandle Release() noexcept;
	void Reset(VulkanNativeHandle descriptor = InvalidVulkanHandle) noexcept;
	VulkanExternalHandle Duplicate() const;

  private:
	VulkanNativeHandle m_descriptor;
};

struct VulkanExternalIdentity
{
	std::array<std::uint8_t, VK_UUID_SIZE> deviceUuid{};
	std::array<std::uint8_t, VK_UUID_SIZE> driverUuid{};
	bool operator==(const VulkanExternalIdentity&) const = default;
};

// Native transfers preserve RGBA8/BGRA8 UNORM/sRGB, packed 10-bit UNORM and RGBA
// 16/32-bit float, with one or two slices, optimal 2D tiling, valid mip levels,
// one sample and no image-create flags. Transport metadata retains creation fields.
struct VulkanExternalImageSpec
{
	VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
	VkExtent3D extent{0, 0, 1};
	std::uint32_t arrayLayers = 2;
	std::uint32_t mipLevels = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
							  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
	VkImageType imageType = VK_IMAGE_TYPE_2D;
	VkImageCreateFlags flags = 0;
};

struct VulkanExternalImageDescriptor
{
	VulkanExternalIdentity identity;
	VulkanExternalImageSpec image;
	VkDeviceSize allocationSize = 0;
	std::uint32_t memoryTypeIndex = 0;
	bool dedicated = true;
	std::uint32_t producerQueueFamily = VK_QUEUE_FAMILY_IGNORED;
	VkImageLayout producerLayout = VK_IMAGE_LAYOUT_GENERAL;
};

struct VulkanExternalCapabilities
{
	VkResult result = VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkImageFormatProperties imageLimits{};
	VkExternalMemoryProperties memory{};
	VkExternalSemaphoreProperties semaphore{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	bool CanExportImage() const noexcept;
	bool CanImportImage() const noexcept;
	bool CanExportSemaphore() const noexcept;
	bool CanImportSemaphore() const noexcept;
};

struct VulkanExternalFenceCapabilities
{
	VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
	VkExternalFenceProperties fence{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
	bool CanExport() const noexcept;
	bool CanImport() const noexcept;
};

class VulkanExternalDevice;

// This owns an allocation, not a GPU operation. Submission/inspection owners
// must retain it until every producer and consumer use actually completes.
// The descriptor describes the agreed handoff layout, never readiness or idle.
class VulkanExternalImage final
{
  public:
	std::shared_ptr<VulkanImage> Image() const noexcept;
	const VulkanExternalImageDescriptor& Descriptor() const noexcept;
	VulkanExternalHandle ExportMemory() const;

  private:
	friend class VulkanExternalDevice;
	VulkanExternalImage(std::shared_ptr<VulkanImage> image, VulkanExternalImageDescriptor descriptor,
						VulkanGetMemoryHandle getMemoryHandle, bool exportable);
	const std::shared_ptr<VulkanImage> m_image;
	const VulkanExternalImageDescriptor m_descriptor;
	const VulkanGetMemoryHandle m_getMemoryHandle;
	const bool m_exportable;
	mutable std::mutex m_exportMutex;
#ifdef _WIN32
	mutable VulkanExternalHandle m_memoryHandle;
#endif
};

// A binary semaphore is exported at most once by this owner. Users retain this
// owner through submitted GPU use; destruction neither waits nor infers idle.
class VulkanExternalSemaphore final
{
  public:
	~VulkanExternalSemaphore();
	VkSemaphore Get() const noexcept;
	std::shared_ptr<VulkanDevice> Device() const noexcept;
	const VulkanExternalIdentity& Identity() const noexcept;
	VulkanExternalHandle ExportOnce();

  private:
	friend class VulkanExternalDevice;
	VulkanExternalSemaphore(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
							VulkanGetSemaphoreHandle getSemaphoreHandle, bool exportable);
	const std::shared_ptr<VulkanDevice> m_device;
	const VulkanExternalIdentity m_identity;
	const VulkanGetSemaphoreHandle m_getSemaphoreHandle;
	const bool m_exportable;
	VkSemaphore m_semaphore = VK_NULL_HANDLE;
	bool m_exported = false;
	std::mutex m_exportMutex;
};

// A one-shot timeline payload: zero is pending, one is consumer completion.
// Native submissions must retain it through their own retirement fence.
class VulkanExternalTimeline final : public GpuCompletion
{
  public:
	VkSemaphore Get() const noexcept;
	VulkanExternalHandle ExportOnce();
	CompletionStatus PollCompletion() const noexcept override;

  private:
	friend class VulkanExternalDevice;
	VulkanExternalTimeline(std::shared_ptr<VulkanExternalSemaphore> semaphore,
						   PFN_vkGetSemaphoreCounterValueKHR counter);
	const std::shared_ptr<VulkanExternalSemaphore> m_semaphore;
	const PFN_vkGetSemaphoreCounterValueKHR m_counter;
	mutable std::atomic<CompletionStatus> m_status{CompletionStatus::Pending};
};

// One-shot opaque native fence, permanently imported/exported. Never reset the
// native fence. Polling observes real fence status without waiting or queue
// access. The submitted-use owner must retain this object until actual native
// completion; Failed does not authorize destruction of pending GPU resources.
class VulkanExternalFence final : public GpuCompletion
{
  public:
	~VulkanExternalFence() override;
	VkFence Get() const noexcept;
	std::shared_ptr<VulkanDevice> Device() const noexcept;
	const VulkanExternalIdentity& Identity() const noexcept;
	VulkanExternalHandle ExportOnce();
	CompletionStatus PollCompletion() const noexcept override;

  private:
	friend class VulkanExternalDevice;
	VulkanExternalFence(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
						VulkanGetFenceHandle getFenceHandle, bool exportable);
	const std::shared_ptr<VulkanDevice> m_device;
	const VulkanExternalIdentity m_identity;
	const VulkanGetFenceHandle m_getFenceHandle;
	const bool m_exportable;
	VkFence m_fence = VK_NULL_HANDLE;
	bool m_exported = false;
	std::mutex m_exportMutex;
	mutable std::atomic<CompletionStatus> m_status{CompletionStatus::Pending};
};

class VulkanExternalDevice
{
  public:
	// Supply the actual instance API version and enabled logical-device names.
	// Physical extension support alone never enables this sharing path.
	// timelineEnabled must reflect the feature enabled during logical-device creation.
	VulkanExternalDevice(std::shared_ptr<VulkanDevice> device, std::span<const char* const> enabledExtensions,
						 std::uint32_t instanceApiVersion, bool timelineEnabled = false);
	static VulkanExternalCapabilities Query(VkPhysicalDevice physicalDevice, const VulkanExternalImageSpec& image,
											std::uint32_t instanceApiVersion);
	static VulkanExternalFenceCapabilities QueryFence(VkPhysicalDevice physicalDevice,
													  std::uint32_t instanceApiVersion);
	static VulkanExternalCapabilities QueryTimeline(VkPhysicalDevice physicalDevice, std::uint32_t instanceApiVersion);
	bool TimelineCommandsEnabled() const noexcept;
	std::shared_ptr<VulkanExternalTimeline> CreateExportableTimeline() const;
	std::shared_ptr<VulkanExternalTimeline> ImportTimeline(const VulkanExternalIdentity& identity,
														   VulkanExternalHandle& handle) const;
	const VulkanExternalIdentity& Identity() const noexcept;
	std::shared_ptr<VulkanDevice> Device() const noexcept;
	std::shared_ptr<VulkanExternalImage> CreateExportable(const VulkanExternalImageSpec& image,
														  VkImageLayout producerLayout = VK_IMAGE_LAYOUT_GENERAL) const;
	// Imports duplicate the supplied native handle for Vulkan. Every failure preserves the
	// original; only complete helper success resets the caller's handle ownership.
	std::shared_ptr<VulkanExternalImage> ImportImage(const VulkanExternalImageDescriptor& descriptor,
													 VulkanExternalHandle& handle) const;
	std::shared_ptr<VulkanExternalSemaphore> CreateExportableSemaphore() const;
	std::shared_ptr<VulkanExternalSemaphore> ImportSemaphore(const VulkanExternalIdentity& identity,
															 VulkanExternalHandle& handle) const;

	bool FenceCommandsEnabled() const noexcept;
	std::shared_ptr<VulkanExternalFence> CreateExportableFence() const;
	std::shared_ptr<VulkanExternalFence> ImportFence(const VulkanExternalIdentity& identity,
													 VulkanExternalHandle& handle) const;

  private:
	std::shared_ptr<VulkanExternalSemaphore> CreateSemaphore(bool timeline) const;
	std::shared_ptr<VulkanExternalSemaphore> ImportSemaphore(const VulkanExternalIdentity& identity,
															 VulkanExternalHandle& handle, bool timeline) const;
	PFN_vkGetSemaphoreCounterValueKHR m_counter = nullptr;
	std::shared_ptr<VulkanImage> CreateImage(const VulkanExternalImageSpec& image) const;
	VulkanExternalCapabilities ImageCapabilities(const VulkanExternalImageSpec& image) const;
	struct ImageQuery
	{
		VulkanExternalImageSpec image;
		VulkanExternalCapabilities capabilities;
	};
	mutable std::mutex m_imageQueryMutex;
	mutable std::array<ImageQuery, 16> m_imageQueries{};
	mutable std::size_t m_nextImageQuery = 0;
	const std::shared_ptr<VulkanDevice> m_device;
	const std::uint32_t m_instanceApiVersion;
	VulkanExternalIdentity m_identity;
	VulkanGetMemoryHandle m_getMemoryHandle = nullptr;
	VulkanGetSemaphoreHandle m_getSemaphoreHandle = nullptr;
	VulkanImportSemaphoreHandle m_importSemaphoreHandle = nullptr;
	VulkanGetFenceHandle m_getFenceHandle = nullptr;
	VulkanImportFenceHandle m_importFenceHandle = nullptr;
};
} // namespace simxr
