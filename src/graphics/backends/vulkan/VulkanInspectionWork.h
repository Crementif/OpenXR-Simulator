#pragma once

#include "graphics/backends/vulkan/VulkanInspection.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace simxr
{
// One recorded operation and its native dependencies. Only the owned queue can
// construct or submit it. Completion observation never submits additional work.
class VulkanInspectionWork final : public GpuCompletion
{
public:
	~VulkanInspectionWork() override;
	CompletionStatus PollCompletion() const noexcept override;
	bool NativeIdle() const noexcept;
	bool Submitted() const noexcept;
	VkResult Wait(std::uint64_t timeout) const noexcept;
	VkResult WaitForProgress(std::uint64_t timeout) const noexcept;
	void ReleaseSources();
	std::shared_ptr<const VulkanImagePixels> ReadPixels() const;
	CompletionStatus ProducerCompletion() const noexcept;
	void ImportProducerReady(const VulkanExternalDevice& external);
	void CommitProducer() noexcept;
	void CancelProducer() noexcept;
	const std::shared_ptr<VulkanImage>& Image() const noexcept;
	const std::shared_ptr<VulkanExternalImage>& ExternalOutput() const noexcept;
	const std::shared_ptr<VulkanExternalSemaphore>& Signal() const noexcept;

private:
	friend class VulkanInspectionQueue;
	struct Native;
	explicit VulkanInspectionWork(std::shared_ptr<VulkanDevice> device, std::shared_ptr<FrameReservation> admission);
	static std::shared_ptr<VulkanInspectionWork> Import(const std::shared_ptr<VulkanDevice>& device,
														const VulkanExternalDevice& external,
														std::shared_ptr<FrameReservation> admission,
														const std::shared_ptr<VulkanFrame>& producer);
	static std::shared_ptr<VulkanInspectionWork>
	Readback(std::shared_ptr<VulkanDevice> device, std::shared_ptr<FrameReservation> admission,
			 const std::shared_ptr<VulkanImage>& source, std::shared_ptr<FrameReservation> sourceBudget,
			 std::optional<std::uint32_t> layer, std::shared_ptr<const void> retirementOwner,
			 std::optional<ImageRegion> region);
	static std::shared_ptr<VulkanInspectionWork>
	Delivery(const std::shared_ptr<VulkanDevice>& device, const VulkanExternalDevice& external,
			 std::shared_ptr<FrameReservation> admission, const std::shared_ptr<VulkanImage>& source,
			 std::shared_ptr<FrameReservation> sourceBudget, const VulkanExternalImageSpec& spec);
	VkResult Submit() noexcept;
	std::unique_ptr<Native> m_native;
	std::atomic<bool> m_submitted{false};
	std::atomic<bool> m_ready{false};
	mutable std::atomic<CompletionStatus> m_status{CompletionStatus::Pending};
	mutable std::mutex m_sourceMutex;
};
} // namespace simxr
