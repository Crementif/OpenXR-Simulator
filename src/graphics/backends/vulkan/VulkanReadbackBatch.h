#pragma once

#include "graphics/GpuCompletion.h"
#include "graphics/backends/vulkan/VulkanResources.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace simxr
{
struct VulkanReadbackRegion
{
	std::shared_ptr<VulkanImage> image;
	VkOffset2D offset{};
	VkExtent2D extent{};
	std::uint32_t layer = 0;
};

// Explicit readback from the application's binding queue. The caller holds its
// queue-access permit during Submit and must close borrowed work before device teardown.
class VulkanReadbackBatch final : public GpuCompletion
{
public:
	static std::shared_ptr<VulkanReadbackBatch> Prepare(std::shared_ptr<VulkanDevice> device,
														std::span<const VulkanReadbackRegion> regions);
	~VulkanReadbackBatch() override;

	VkResult Submit() noexcept;
	CompletionStatus PollCompletion() const noexcept override;
	bool NativeIdle() const noexcept;
	VkResult Wait(std::uint64_t timeout) const noexcept;
	std::vector<std::vector<std::uint8_t>> ReadPixels() const;

private:
	enum class Phase
	{
		Prepared,
		Submitting,
		Submitted,
		Rejected,
		Failed
	};
	struct Native;
	explicit VulkanReadbackBatch(std::shared_ptr<VulkanDevice> device);
	void Record(std::span<const VulkanReadbackRegion> regions);

	std::unique_ptr<Native> m_native;
	std::atomic<Phase> m_phase{Phase::Prepared};
	std::atomic<VkResult> m_submitResult{VK_NOT_READY};
	mutable std::atomic<CompletionStatus> m_status{CompletionStatus::Pending};
	mutable std::mutex m_readMutex;
};
} // namespace simxr
