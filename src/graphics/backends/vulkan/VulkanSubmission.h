#pragma once

#include "graphics/FrameOutput.h"
#include "graphics/FrameGpuTiming.h"
#include "graphics/FramePool.h"
#include "graphics/backends/vulkan/VulkanExternal.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"
#include "graphics/backends/vulkan/VulkanResources.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace simxr
{
class VulkanBackend;

class VulkanSubmission final : public GpuCompletion
{
  public:
	~VulkanSubmission();
	CompletionStatus PollCompletion() const noexcept override;
	VkResult Wait(std::uint64_t timeout) const noexcept;
	bool NativeIdle() const noexcept;

  private:
	friend class VulkanBackend;
	friend class VulkanFrame;
	struct Resources
	{
		VulkanNativePreservation<Resources, 256>::Reservation preservation{
			"Two hundred fifty-six native Vulkan submission owners are retained"};
		explicit Resources(std::shared_ptr<VulkanDevice> device);
		~Resources();
		std::shared_ptr<VulkanDevice> device;
		std::shared_ptr<FrameReservation> reservation;
		std::shared_ptr<VulkanSubmission> completion;
		std::shared_ptr<const void> pipeline;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer command = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkQueryPool timestamps = VK_NULL_HANDLE;
		std::shared_ptr<FrameGpuTiming> timing;
		VkDescriptorPool descriptors = VK_NULL_HANDLE;
		std::vector<VkImageView> sourceViews;
		std::vector<std::shared_ptr<VulkanImage>> sources;
		// Source CPU readers may disappear before this submission reaches its fence.
		std::vector<std::shared_ptr<FrameReservation>> sourceBudgets;
		std::shared_ptr<VulkanImage> output;
		std::shared_ptr<VulkanExternalImage> external;
		std::shared_ptr<VulkanExternalSemaphore> ready;
		std::array<VkImageView, 2> outputViews{};
		std::array<VkFramebuffer, 2> framebuffers{};
	};
	explicit VulkanSubmission(std::shared_ptr<VulkanDevice> device);
	void ReleaseSources();
	void BeginTiming();
	void EndTiming();
	void ObserveTiming() const noexcept;
	std::unique_ptr<Resources> m_resources;
	std::atomic<bool> m_attempted{false};
	std::atomic<bool> m_ready{false};
	std::atomic<VkResult> m_submitResult{VK_NOT_READY};
	mutable std::atomic<CompletionStatus> m_status{CompletionStatus::Pending};
};

class VulkanFrame final : public FrameOutput
{
  public:
	CompletionStatus PollCompletion() const noexcept override;
	const VulkanImage& Image() const;
	// Aliases the submission owner, retaining its budget and native dependencies.
	std::shared_ptr<VulkanImage> RetainImage() const;
	VkImageView View(std::uint32_t eye) const;
	std::shared_ptr<const GpuCompletion> Completion() const;
	std::shared_ptr<VulkanExternalImage> ExternalImage() const;
	std::shared_ptr<FrameReservation> Reservation() const;
	bool Submitted() const noexcept;
	std::shared_ptr<const FrameGpuTiming> Timing() const noexcept;

  private:
	friend class VulkanBackend;
	friend class VulkanInspection;
	friend class VulkanInspectionTransfer;
	friend class VulkanInspectionWork;
	explicit VulkanFrame(std::shared_ptr<VulkanSubmission> submission, std::span<const FrameView> views = {});
	VulkanExternalHandle ExportReady();
	bool ClaimInspection();
	void CancelInspection() noexcept;
	void CommitInspection() noexcept;
	std::shared_ptr<VulkanSubmission> m_submission;
	std::mutex m_inspectionMutex;
	VulkanExternalHandle m_readyFd;
	enum class InspectionClaim
	{
		Available,
		Prepared,
		Committed
	};
	InspectionClaim m_inspection = InspectionClaim::Available;
};
} // namespace simxr
