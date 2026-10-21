#pragma once

#include "graphics/FrameOutput.h"
#include "graphics/FramePool.h"
#include "graphics/SharedImageTypes.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace simxr
{
enum class VulkanInspectionKind
{
	Required,
	History,
	Reprojection
};
struct CompositionPlan;
class VulkanFrame;
class VulkanInspection;
class VulkanInspectionTransfer;
class VulkanInspectionQueue;
class VulkanInspectionWork;

// Initialize before the runtime/control worker graph so its bounded observer is
// joined after those workers at process teardown. Creates no Vulkan resources.
void InitializeVulkanImageDelivery();

struct VulkanInspectionValidation
{
	std::atomic<std::uint32_t> errors{0};
	std::atomic<std::uint32_t> liveOwnedDevices{0};
};

struct VulkanImagePixels
{
	VkFormat format = VK_FORMAT_UNDEFINED;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t layers = 0;
	std::vector<std::uint8_t> rgba;
};

class VulkanImageReadback final : public GpuCompletion
{
  public:
	CompletionStatus PollCompletion() const noexcept override;
	// Explicit CPU export only. Returns empty while the real copy is pending.
	std::shared_ptr<const VulkanImagePixels> Pixels();

  private:
	friend class VulkanInspectionFrame;
	VulkanImageReadback(std::shared_ptr<VulkanInspectionQueue> queue, std::shared_ptr<VulkanInspectionWork> work);
	std::shared_ptr<VulkanInspectionQueue> m_queue;
	std::shared_ptr<VulkanInspectionWork> m_work;
	std::mutex m_mutex;
	std::shared_ptr<const VulkanImagePixels> m_pixels;
};

struct VulkanDeliveryHandles
{
	UniqueFd memory;
	UniqueFd ready;
};

// Immutable one-consumer GPU allocation. The producer never reuses its pixels.
// Consumer completion is an imported external fence, observed without queuing
// any consumer-dependent wait. Callers must finish all sampling before signaling
// that one-shot fence. FD duplicates are retries for that same consumer only.
class VulkanImageDelivery final : public GpuCompletion
{
  public:
	~VulkanImageDelivery();
	SharedImageKey Key() const noexcept;
	const VulkanExternalImageDescriptor& Descriptor() const noexcept;
	CompletionStatus PollCompletion() const noexcept override;
	CompletionStatus PollConsumerCompletion() const noexcept;
	// Requires an unsignaled, compatible opaque-FD fence. Failure preserves fd.
	void RegisterConsumer(const VulkanExternalIdentity& identity, UniqueFd& fd);
	// Registration precedes exposure. Ready is a binary GPU semaphore; it must
	// be imported/waited once, even when the producing CPU fence is complete.
	VulkanDeliveryHandles ExportHandles(bool expose = true);
	void MarkExposed() noexcept;

  private:
	friend class VulkanInspectionFrame;
	struct Resources;
	explicit VulkanImageDelivery(std::shared_ptr<Resources> resources);
	std::shared_ptr<Resources> m_resources;
};

class VulkanInspectionFrame final : public FrameOutput
{
  public:
	~VulkanInspectionFrame();
	// One retained source per draw; crop/slice coordinates address those images.
	// All sources must belong to the same owned queue. Pending input returns
	// empty; failed input, invalid bindings or exhausted capacity throw.
	static std::shared_ptr<VulkanInspectionFrame> Compose(const CompositionPlan& plan,
		std::span<const std::shared_ptr<const VulkanInspectionFrame>> sources);
	CompletionStatus PollCompletion() const noexcept override;
	const VulkanImage& Image() const;
	VkImageView View(std::uint32_t eye) const;
	// CPU budget transfer only; never collects, submits, waits, or changes pixels.
	bool PromoteCaptureBudget(const FramePool& captures, std::shared_ptr<FrameReservation> reservation = {}) const;
	// This submits on the owned inspection queue. No host XR call is required,
	// including after the borrowed application device has been destroyed.
	std::shared_ptr<VulkanImageReadback> Export(std::optional<std::uint32_t> layer = {},
												std::shared_ptr<const void> retirementOwner = {}) const;
	// Weak queue callback: collection keeps no queue alive and submits no work.
	std::function<void()> Collector() const;
	bool CanShare() const noexcept;
	// A private GPU copy on the owned queue, independent of host calls/device
	// lifetime. Delivery has its own bounded budget, separate from captures.
	std::shared_ptr<VulkanImageDelivery> Share() const;

  private:
	friend class VulkanInspection;
	friend class VulkanInspectionTransfer;
	class Resources;
	explicit VulkanInspectionFrame(std::shared_ptr<Resources> resources, std::span<const FrameView> views);
	std::shared_ptr<Resources> m_resources;
};

// Prepared before application queue acceptance; Commit is allocation-free and
// merely opens the independent worker's gate after that acceptance succeeds.
class VulkanInspectionTransfer
{
  public:
	~VulkanInspectionTransfer();
	void Commit() noexcept;
	std::shared_ptr<VulkanInspectionFrame> Output() const;

  private:
	friend class VulkanInspection;
	class State;
	explicit VulkanInspectionTransfer(std::shared_ptr<State> state);
	std::shared_ptr<State> m_state;
};

class VulkanInspection
{
  public:
	explicit VulkanInspection(const VulkanExternalIdentity& identity, std::size_t capacity = 8,
							  std::shared_ptr<VulkanInspectionValidation> validation = {});
	~VulkanInspection();
	VulkanInspection(const VulkanInspection&) = delete;
	VulkanInspection& operator=(const VulkanInspection&) = delete;
	// Observers are installed before worker publication and never retain source images.
	std::unique_ptr<VulkanInspectionTransfer> Prepare(std::shared_ptr<VulkanFrame> source,
		std::function<void(CompletionStatus)> completed = {}, std::function<void()> retired = {},
		VulkanInspectionKind kind = VulkanInspectionKind::Required);
	VkResult Drain(std::uint64_t timeout = 5000000000ULL);
	VkResult Close(std::uint64_t timeout = 5000000000ULL);
	bool CanShare() const noexcept;

  private:
	class State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr
