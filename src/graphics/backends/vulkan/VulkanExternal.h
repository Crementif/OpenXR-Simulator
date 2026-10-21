#pragma once

#include "graphics/GpuCompletion.h"
#include "graphics/backends/vulkan/VulkanResources.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace simxr
{
class UniqueFd
{
  public:
	explicit UniqueFd(int descriptor = -1) noexcept;
	~UniqueFd();
	UniqueFd(UniqueFd&& other) noexcept;
	UniqueFd& operator=(UniqueFd&& other) noexcept;
	UniqueFd(const UniqueFd&) = delete;
	UniqueFd& operator=(const UniqueFd&) = delete;
	int Get() const noexcept;
	explicit operator bool() const noexcept;
	int Release() noexcept;
	void Reset(int descriptor = -1) noexcept;
	UniqueFd Duplicate() const;

  private:
	int m_descriptor;
};

struct VulkanExternalIdentity
{
	std::array<std::uint8_t, VK_UUID_SIZE> deviceUuid{};
	std::array<std::uint8_t, VK_UUID_SIZE> driverUuid{};
	bool operator==(const VulkanExternalIdentity&) const = default;
};

// Native transfers preserve RGBA8/BGRA8 UNORM/sRGB, one or two slices, optimal
// 2D tiling, one mip, one sample and no image-create flags. All creation fields
// remain explicit in transport metadata; unsupported variants are rejected.
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
	UniqueFd ExportMemory() const;

  private:
	friend class VulkanExternalDevice;
	VulkanExternalImage(std::shared_ptr<VulkanImage> image, VulkanExternalImageDescriptor descriptor,
						PFN_vkGetMemoryFdKHR getMemoryFd, bool exportable);
	const std::shared_ptr<VulkanImage> m_image;
	const VulkanExternalImageDescriptor m_descriptor;
	const PFN_vkGetMemoryFdKHR m_getMemoryFd;
	const bool m_exportable;
	mutable std::mutex m_exportMutex;
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
	UniqueFd ExportOnce();

  private:
	friend class VulkanExternalDevice;
	VulkanExternalSemaphore(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
							PFN_vkGetSemaphoreFdKHR getSemaphoreFd, bool exportable);
	const std::shared_ptr<VulkanDevice> m_device;
	const VulkanExternalIdentity m_identity;
	const PFN_vkGetSemaphoreFdKHR m_getSemaphoreFd;
	const bool m_exportable;
	VkSemaphore m_semaphore = VK_NULL_HANDLE;
	bool m_exported = false;
	std::mutex m_exportMutex;
};

// One-shot OPAQUE_FD fence, permanently imported/exported. Never reset the
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
	UniqueFd ExportOnce();
	CompletionStatus PollCompletion() const noexcept override;

  private:
	friend class VulkanExternalDevice;
	VulkanExternalFence(std::shared_ptr<VulkanDevice> device, VulkanExternalIdentity identity,
						PFN_vkGetFenceFdKHR getFenceFd, bool exportable);
	const std::shared_ptr<VulkanDevice> m_device;
	const VulkanExternalIdentity m_identity;
	const PFN_vkGetFenceFdKHR m_getFenceFd;
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
	VulkanExternalDevice(std::shared_ptr<VulkanDevice> device, std::span<const char* const> enabledExtensions,
						 std::uint32_t instanceApiVersion);
	static VulkanExternalCapabilities Query(VkPhysicalDevice physicalDevice, const VulkanExternalImageSpec& image,
											std::uint32_t instanceApiVersion);
	static VulkanExternalFenceCapabilities QueryFence(VkPhysicalDevice physicalDevice,
													  std::uint32_t instanceApiVersion);
	const VulkanExternalIdentity& Identity() const noexcept;
	std::shared_ptr<VulkanDevice> Device() const noexcept;
	std::shared_ptr<VulkanExternalImage> CreateExportable(const VulkanExternalImageSpec& image,
														  VkImageLayout producerLayout = VK_IMAGE_LAYOUT_GENERAL) const;
	// Imports duplicate the supplied FD for Vulkan. Every failure preserves the
	// original; only complete helper success resets the caller's FD ownership.
	std::shared_ptr<VulkanExternalImage> ImportImage(const VulkanExternalImageDescriptor& descriptor,
													 UniqueFd& fd) const;
	std::shared_ptr<VulkanExternalSemaphore> CreateExportableSemaphore() const;
	std::shared_ptr<VulkanExternalSemaphore> ImportSemaphore(const VulkanExternalIdentity& identity,
															 UniqueFd& fd) const;

	bool FenceCommandsEnabled() const noexcept;
	std::shared_ptr<VulkanExternalFence> CreateExportableFence() const;
	std::shared_ptr<VulkanExternalFence> ImportFence(const VulkanExternalIdentity& identity, UniqueFd& fd) const;

  private:
	std::shared_ptr<VulkanImage> CreateImage(const VulkanExternalImageSpec& image) const;
	const std::shared_ptr<VulkanDevice> m_device;
	const std::uint32_t m_instanceApiVersion;
	VulkanExternalIdentity m_identity;
	PFN_vkGetMemoryFdKHR m_getMemoryFd = nullptr;
	PFN_vkGetSemaphoreFdKHR m_getSemaphoreFd = nullptr;
	PFN_vkImportSemaphoreFdKHR m_importSemaphoreFd = nullptr;
	PFN_vkGetFenceFdKHR m_getFenceFd = nullptr;
	PFN_vkImportFenceFdKHR m_importFenceFd = nullptr;
};
} // namespace simxr
