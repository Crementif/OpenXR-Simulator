#include "graphics/backends/vulkan/VulkanInspection.h"
#include "Pch.h"
#include "graphics/SnapshotBudget.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include "graphics/backends/vulkan/VulkanInspectionWork.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"
#include "graphics/backends/vulkan/VulkanSubmission.h"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace simxr
{
static FramePool& DeliveryBudget()
{
	// Includes abandoned readers/native blocks from every retired generation.
	static FramePool s_budget{8};
	return s_budget;
}
static std::uint64_t Identity(std::atomic<std::uint64_t>& next)
{
	auto value = next.load();
	for (;;)
	{
		if (value == std::numeric_limits<std::uint64_t>::max())
			throw VulkanError(VK_ERROR_TOO_MANY_OBJECTS, "Shared image identity exhausted");
		if (next.compare_exchange_weak(value, value + 1))
			return value;
	}
}
static std::atomic<std::uint64_t> s_nextPool{1};

class VulkanDeliveryNative
{
  public:
	static std::shared_ptr<VulkanDeliveryNative> Prepare(VulkanInspectionQueue& queue,
														 std::shared_ptr<VulkanImage> source,
														 std::shared_ptr<FrameReservation> sourceBudget,
														 std::shared_ptr<FrameReservation> admission,
														 std::function<void(SharedImageKey)> retired);
	~VulkanDeliveryNative()
	{
		if (!Safe())
		{
			std::fputs("SimXR: shared image completion unknown; preserving bounded native owners\n", stderr);
			m_payload->preservation.Preserve(std::move(m_payload));
		}
	}
	SharedImageKey Key() const noexcept
	{
		return m_key;
	}
	const VulkanExternalImageDescriptor& Descriptor() const noexcept
	{
		return m_payload->image->Descriptor();
	}
	CompletionStatus Poll() const noexcept
	{
		return m_payload->work->PollCompletion();
	}
	CompletionStatus ConsumerCompletion() const noexcept
	{
		const auto consumer = m_payload->consumer.load();
		return consumer ? consumer->PollCompletion() : CompletionStatus::Pending;
	}
	void RegisterConsumer(const VulkanExternalDevice& external, const VulkanExternalIdentity& identity,
						  VulkanExternalHandle& fd, ImageCompletionKind kind)
	{
		std::lock_guard lock(m_mutex);
		if (m_payload->consumer.load())
			throw std::logic_error("Shared image already has a consumer");
		auto duplicate = fd.Duplicate();
		std::shared_ptr<GpuCompletion> consumer;
		if (kind == ImageCompletionKind::Timeline)
			consumer = external.ImportTimeline(identity, duplicate);
		else
			consumer = external.ImportFence(identity, duplicate);
		if (consumer->PollCompletion() != CompletionStatus::Pending)
			throw std::invalid_argument("Shared image requires an unsignaled consumer completion");
		m_payload->consumer.store(std::move(consumer));
		fd.Reset();
	}
	VulkanDeliveryHandles ExportHandles(bool expose)
	{
		std::lock_guard lock(m_mutex);
		const auto consumer = m_payload->consumer.load();
		if (!consumer || consumer->PollCompletion() != CompletionStatus::Pending)
			throw std::logic_error("Shared image needs its pending one-shot consumer before exposure");
		auto memory = m_payload->image->ExportMemory();
		if (!m_payload->readyFd)
			m_payload->readyFd = m_payload->ready->ExportOnce();
		auto ready = m_payload->readyFd.Duplicate();
		if (expose)
			MarkExposed();
		return {std::move(memory), std::move(ready)};
	}
	void MarkExposed() noexcept
	{
		m_offered.store(true);
	}
	void Release() noexcept
	{
		m_released.store(true);
	}
	bool ObserveForRetirement()
	{
		if (!m_published.load() && !m_released.load())
			return false;
		m_payload->work->ReleaseSources();
		return m_released.load() && Safe();
	}
	VkResult Submit(VulkanInspectionQueue& queue);

  private:
	struct Payload
	{
		VulkanNativePreservation<Payload, 8>::Reservation preservation{
			"Eight native delivery payload owners are retained"};
		std::shared_ptr<VulkanDevice> device;
		std::shared_ptr<FrameReservation> slot;
		std::shared_ptr<VulkanInspectionWork> work;
		std::shared_ptr<VulkanExternalImage> image;
		std::shared_ptr<VulkanExternalSemaphore> ready;
		std::atomic<std::shared_ptr<GpuCompletion>> consumer;
		VulkanExternalHandle readyFd;
	};
	VulkanDeliveryNative() = default;
	bool Safe() const noexcept
	{
		if (!m_payload->work)
			return true;
		const auto consumer = m_payload->consumer.load();
		return m_payload->work->NativeIdle() &&
			   (!m_offered.load() || (consumer && consumer->PollCompletion() == CompletionStatus::Complete));
	}
	std::unique_ptr<Payload> m_payload = std::make_unique<Payload>();
	SharedImageKey m_key;
	std::mutex m_mutex;
	std::atomic<bool> m_offered{false};
	std::atomic<bool> m_published{false};
	std::atomic<bool> m_released{false};
};

// One bounded observer owns abandoned deliveries independently of public frames,
// sessions and inspection-queue owners. It never submits or waits for a consumer.
class VulkanDeliveryRetirement
{
  public:
	~VulkanDeliveryRetirement()
	{
		Stop();
	}
	void Start()
	{
		std::lock_guard lifecycle(m_workerMutex);
		if (m_worker.joinable())
			return;
		std::lock_guard lock(m_mutex);
		m_stopping = false;
		m_worker = std::thread(
			[this]
			{
				Run();
			});
	}
	void Stop()
	{
		std::lock_guard lifecycle(m_workerMutex);
		{
			std::lock_guard lock(m_mutex);
			m_stopping = true;
		}
		m_changed.notify_all();
		if (m_worker.joinable())
			m_worker.join();
	}
	void Shutdown()
	{
		Stop();
		std::array<std::shared_ptr<VulkanDeliveryNative>, 8> retired;
		{
			std::lock_guard lock(m_mutex);
			retired.swap(m_deliveries);
		}
		// Destruction preserves unproved payloads; completed owners retire while
		// the driver is still alive, outside the observer lock.
	}
	void Add(const std::shared_ptr<VulkanDeliveryNative>& delivery)
	{
		std::lock_guard lock(m_mutex);
		for (auto& slot : m_deliveries)
			if (!slot)
			{
				slot = delivery;
				m_changed.notify_all();
				return;
			}
		throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Shared image retirement capacity exhausted");
	}

  private:
	std::mutex m_workerMutex;
	std::mutex m_mutex;
	std::condition_variable m_changed;
	std::array<std::shared_ptr<VulkanDeliveryNative>, 8> m_deliveries;
	bool m_stopping = false;
	std::thread m_worker;
	void Run() noexcept
	{
		for (;;)
		{
			std::array<std::shared_ptr<VulkanDeliveryNative>, 8> pending;
			{
				std::unique_lock lock(m_mutex);
				if (m_stopping)
					return;
				m_changed.wait(lock,
							   [&]
							   {
								   return m_stopping || std::any_of(m_deliveries.begin(), m_deliveries.end(),
																	[](const auto& delivery)
																	{
																		return delivery != nullptr;
																	});
							   });
				if (m_stopping)
					return;
				m_changed.wait_for(lock, std::chrono::milliseconds(1));
				if (m_stopping)
					return;
				pending = m_deliveries;
			}
			for (const auto& delivery : pending)
			{
				if (!delivery)
					continue;
				try
				{
					if (!delivery->ObserveForRetirement())
						continue;
					std::lock_guard lock(m_mutex);
					for (auto& slot : m_deliveries)
						if (slot == delivery)
						{
							slot.reset();
							break;
						}
				}
				catch (...)
				{
					std::fputs("SimXR: shared image retirement observation failed\n", stderr);
				}
			}
		}
	}
};
static VulkanDeliveryRetirement& DeliveryRetirement()
{
#ifdef _WIN32
	// DLL process detach cannot join workers. Normal runtime teardown stops the
	// worker explicitly; unproved native owners remain bounded until process exit.
	static auto* s_retirement = new VulkanDeliveryRetirement;
	return *s_retirement;
#else
	static VulkanDeliveryRetirement s_retirement;
	return s_retirement;
#endif
}

void ShutdownVulkanImageDelivery()
{
	DeliveryRetirement().Shutdown();
}

void StopVulkanImageDelivery()
{
	DeliveryRetirement().Stop();
}

void InitializeVulkanImageDelivery()
{
	(void)DeliveryBudget();
	(void)DeliveryRetirement();
}

class VulkanInspectionQueue
{
  public:
	explicit VulkanInspectionQueue(VulkanInspectionDevice owner)
		: m_device(owner.Device()),
		  m_external(m_device, owner.EnabledExtensions(), VK_API_VERSION_1_1, owner.ConsumerTimelineEnabled()),
		  m_fences(owner.ConsumerFencesEnabled()), m_timeline(owner.ConsumerTimelineEnabled())
	{
		m_active.reserve(m_capacity.Capacity());
	}
	~VulkanInspectionQueue()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (const auto& work : m_active)
		{
			const auto now = std::chrono::steady_clock::now();
			const auto timeout =
				now < deadline ? std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() : 0;
			work->Wait(timeout);
		}
		Collect();
		// Native work preserves its charged dependencies if completion is unknown.
	}
	const std::shared_ptr<VulkanDevice>& Device() const noexcept
	{
		return m_device;
	}
	const VulkanExternalDevice& External() const noexcept
	{
		return m_external;
	}
	bool CanShare() const noexcept
	{
		return (m_fences && m_external.FenceCommandsEnabled()) || (m_timeline && m_external.TimelineCommandsEnabled());
	}
	ImageCompletionKind CompletionKind() const noexcept
	{
		return m_timeline ? ImageCompletionKind::Timeline : ImageCompletionKind::Fence;
	}
	void Collect()
	{
		std::array<std::shared_ptr<VulkanInspectionWork>, 64> retired;
		std::size_t count = 0;
		{
			std::lock_guard lock(m_mutex);
			if (m_compositor)
				m_compositor->RetireCompleted();
			for (auto& work : m_active)
				if (work->NativeIdle())
					retired.at(count++) = std::move(work);
			std::erase(m_active, nullptr);
		}
		for (std::size_t index = 0; index < count; ++index)
			retired[index]->ReleaseSources();
	}
	std::shared_ptr<VulkanFrame> PrepareComposition(const CompositionPlan& plan,
													std::span<const VulkanRetainedSource> sources)
	{
		std::lock_guard lock(m_mutex);
		if (!m_compositor)
			m_compositor = std::make_unique<VulkanBackend>(m_device);
		std::shared_ptr<VulkanFrame> frame;
		const auto result = m_compositor->PrepareRetained(plan, sources, frame);
		if (result == XR_ERROR_LIMIT_REACHED)
			throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Owned composition capacity exhausted");
		if (XR_FAILED(result))
			throw VulkanError(VK_ERROR_INITIALIZATION_FAILED, "Invalid retained composition");
		return frame;
	}
	void SubmitComposition(const std::shared_ptr<VulkanFrame>& frame)
	{
		std::lock_guard lock(m_mutex);
		if (XR_FAILED(m_compositor->Submit(frame)))
			throw VulkanError(m_device->lost.load() ? VK_ERROR_DEVICE_LOST : VK_ERROR_INITIALIZATION_FAILED,
							  "Submit retained composition");
	}
	std::shared_ptr<VulkanInspectionWork> ImportCopy(std::shared_ptr<VulkanFrame> source)
	{
		return VulkanInspectionWork::Import(m_device, m_external, ReserveWork(), std::move(source));
	}
	std::shared_ptr<VulkanInspectionWork> Readback(std::shared_ptr<VulkanImage> source,
												   std::shared_ptr<FrameReservation> sourceBudget,
												   std::optional<std::uint32_t> layer,
												   std::shared_ptr<const void> owner)
	{
		return VulkanInspectionWork::Readback(m_device, ReserveWork(), std::move(source), std::move(sourceBudget),
											  layer, std::move(owner));
	}
	std::shared_ptr<VulkanInspectionWork> Delivery(std::shared_ptr<VulkanImage> source,
												   std::shared_ptr<FrameReservation> sourceBudget,
												   const VulkanExternalImageSpec& spec)
	{
		return VulkanInspectionWork::Delivery(m_device, m_external, ReserveWork(), std::move(source),
											  std::move(sourceBudget), spec);
	}
	SharedImageKey DeliveryKey(const VulkanExternalImageSpec& spec)
	{
		std::lock_guard lock(m_mutex);
		if (!m_deliverySpec || !SameShape(*m_deliverySpec, spec))
		{
			m_generation = {Identity(s_nextPool)};
			m_deliverySpec = spec;
		}
		return {m_generation, {Identity(m_nextImage)}};
	}
	VkResult Submit(const std::shared_ptr<VulkanInspectionWork>& work)
	{
		std::lock_guard lock(m_mutex);
		if (work->Submitted() || work->PollCompletion() != CompletionStatus::Pending)
			return VK_ERROR_INITIALIZATION_FAILED;
		// Capacity is reserved before native acquisition. Publication to this
		// preallocated list precedes the host queue call and cannot allocate.
		m_active.push_back(work);
		const auto result = work->Submit();
		if (!work->Submitted())
			m_active.pop_back();
		return result;
	}

  private:
	std::shared_ptr<FrameReservation> ReserveWork()
	{
		Collect();
		auto admission = m_capacity.Reserve();
		if (!admission)
			throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Inspection transfer capacity exhausted");
		return admission;
	}
	static bool SameShape(const VulkanExternalImageSpec& left, const VulkanExternalImageSpec& right) noexcept
	{
		return left.format == right.format && left.extent.width == right.extent.width &&
			   left.extent.height == right.extent.height && left.extent.depth == right.extent.depth &&
			   left.arrayLayers == right.arrayLayers && left.mipLevels == right.mipLevels &&
			   left.samples == right.samples && left.usage == right.usage && left.tiling == right.tiling &&
			   left.imageType == right.imageType && left.flags == right.flags;
	}
	std::shared_ptr<VulkanDevice> m_device;
	VulkanExternalDevice m_external;
	FramePool m_capacity{64};
	const bool m_fences;
	const bool m_timeline;
	ImagePoolGeneration m_generation;
	std::optional<VulkanExternalImageSpec> m_deliverySpec;
	std::atomic<std::uint64_t> m_nextImage{1};
	std::mutex m_mutex;
	std::vector<std::shared_ptr<VulkanInspectionWork>> m_active;
	std::unique_ptr<VulkanBackend> m_compositor;
};

std::shared_ptr<VulkanDeliveryNative> VulkanDeliveryNative::Prepare(VulkanInspectionQueue& queue,
																	std::shared_ptr<VulkanImage> source,
																	std::shared_ptr<FrameReservation> sourceBudget,
																	std::shared_ptr<FrameReservation> admission,
																	std::function<void(SharedImageKey)> retired)
{
	// The payload preservation cell is acquired before any delivery image.
	auto delivery = std::shared_ptr<VulkanDeliveryNative>(new VulkanDeliveryNative());
	auto& payload = *delivery->m_payload;
	payload.device = queue.Device();
	payload.slot = std::move(admission);
	VulkanExternalImageSpec spec;
	spec.format = source->format;
	spec.extent = {source->extent.width, source->extent.height, 1};
	spec.arrayLayers = source->layers;
	spec.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	delivery->m_key = queue.DeliveryKey(spec);
	payload.work = queue.Delivery(std::move(source), std::move(sourceBudget), spec);
	payload.image = payload.work->ExternalOutput();
	if (retired)
		payload.image->Image()->retired = [retired = std::move(retired), key = delivery->m_key]
		{
			retired(key);
		};
	payload.ready = payload.work->Signal();
	return delivery;
}
VkResult VulkanDeliveryNative::Submit(VulkanInspectionQueue& queue)
{
	const auto result = queue.Submit(m_payload->work);
	m_published.store(true);
	return result;
}

struct VulkanImageDelivery::Resources
{
	Resources(std::shared_ptr<VulkanInspectionQueue> queue, std::shared_ptr<VulkanDeliveryNative> native)
		: queue(std::move(queue)), native(std::move(native))
	{
	}
	const std::shared_ptr<VulkanInspectionQueue>& Queue() const noexcept
	{
		return queue;
	}
	const std::shared_ptr<VulkanInspectionQueue> queue;
	const std::shared_ptr<VulkanDeliveryNative> native;
	~Resources()
	{
		if (native)
			native->Release();
	}
};

class VulkanInspectionFrame::Resources
{
  public:
	static std::shared_ptr<Resources> Create(std::shared_ptr<VulkanInspectionQueue> queue,
											 std::shared_ptr<VulkanImage> image,
											 std::shared_ptr<FrameReservation> budget,
											 std::function<void(CompletionStatus)> completed,
											 std::shared_ptr<const GpuCompletion> completion = {},
											 std::function<void(SharedImageKey)> deliveryRetired = {})
	{
		// Construct the owner before any view acquisition. A later view failure
		// destroys all earlier views while the owned device/image remain alive.
		auto resources = std::shared_ptr<Resources>(
			new Resources(std::move(queue), std::move(image), std::move(budget), std::move(completion)));
		resources->m_completed = std::move(completed);
		resources->m_deliveryRetired = std::move(deliveryRetired);
		if (resources->m_image->layers > resources->m_views.size())
			throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Inspection supports at most two image slices");
		for (std::uint32_t eye = 0; eye < resources->m_image->layers; ++eye)
		{
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = resources->m_image->image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = resources->m_image->format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
			CheckVulkan(
				vkCreateImageView(resources->m_image->device->binding.device, &view, nullptr, &resources->m_views[eye]),
				"Create retained inspection view");
		}
		return resources;
	}
	~Resources()
	{
		for (auto view : m_views)
			if (view)
				vkDestroyImageView(m_image->device->binding.device, view, nullptr);
	}
	const std::shared_ptr<VulkanInspectionQueue>& Queue() const noexcept
	{
		return m_queue;
	}
	const std::shared_ptr<VulkanImage>& Image() const noexcept
	{
		return m_image;
	}
	const std::function<void(SharedImageKey)>& DeliveryRetired() const noexcept
	{
		return m_deliveryRetired;
	}
	VkImageView View(std::uint32_t eye) const
	{
		if (eye >= m_image->layers)
			throw std::out_of_range("Vulkan inspection slice");
		return m_views.at(eye);
	}
	CompletionStatus Poll() const noexcept
	{
		return m_completion ? m_completion->PollCompletion() : m_status.load();
	}
	void PublishTerminal(CompletionStatus completion, bool observed) noexcept
	{
		auto expected = CompletionStatus::Pending;
		if (m_status.compare_exchange_strong(expected, completion))
		{
			try
			{
				if (observed && m_completed)
					m_completed(completion);
			}
			catch (...)
			{
				// Observation cannot change the terminal GPU result.
			}
		}
	}
	std::shared_ptr<FrameReservation> Budget() const
	{
		std::lock_guard lock(m_retentionMutex);
		return m_budget;
	}
	bool Promote(const FramePool& captures, std::shared_ptr<FrameReservation> reservation)
	{
		std::shared_ptr<FrameReservation> previous;
		std::lock_guard lock(m_retentionMutex);
		if (captures.Owns(m_budget))
			return true;
		auto replacement = reservation ? std::move(reservation) : captures.Reserve(false);
		if (!replacement || !captures.Consume(replacement))
			return false;
		previous = std::exchange(m_budget, std::move(replacement));
		return true;
	}

  private:
	Resources(std::shared_ptr<VulkanInspectionQueue> queue, std::shared_ptr<VulkanImage> image,
			  std::shared_ptr<FrameReservation> budget, std::shared_ptr<const GpuCompletion> completion)
		: m_queue(std::move(queue)), m_image(std::move(image)), m_budget(std::move(budget)),
		  m_completion(std::move(completion))
	{
	}
	std::shared_ptr<VulkanInspectionQueue> m_queue;
	std::shared_ptr<VulkanImage> m_image;
	std::shared_ptr<FrameReservation> m_budget;
	std::array<VkImageView, 2> m_views{};
	mutable std::mutex m_retentionMutex;
	std::atomic<CompletionStatus> m_status{CompletionStatus::Pending};
	std::function<void(CompletionStatus)> m_completed;
	std::function<void(SharedImageKey)> m_deliveryRetired;
	const std::shared_ptr<const GpuCompletion> m_completion;
};

class VulkanInspectionTransfer::State
{
  public:
	State(std::shared_ptr<FrameReservation> admission, std::shared_ptr<VulkanInspectionWork> work,
		  std::shared_ptr<VulkanInspectionFrame> output)
		: m_admission(std::move(admission)), m_work(std::move(work)), m_output(std::move(output))
	{
	}
	std::shared_ptr<VulkanInspectionFrame> Output() const noexcept
	{
		return m_output;
	}
	void ImportReady(const VulkanExternalDevice& external)
	{
		m_work->ImportProducerReady(external);
	}
	void Cancel() noexcept
	{
		std::lock_guard lock(m_gateMutex);
		if (m_gate.load() != Gate::Prepared)
			return;
		m_work->CancelProducer();
		m_gate.store(Gate::Cancelled);
	}
	void Commit() noexcept
	{
		std::lock_guard lock(m_gateMutex);
		if (m_gate.load() != Gate::Prepared)
			return;
		m_work->CommitProducer();
		m_gate.store(Gate::Committed);
	}
	void Advance(VulkanInspectionQueue& queue)
	{
		CompletionStatus completion;
		{
			// Cancellation and submission share this boundary, including polling
			// by Drain after the worker has stopped. No cancelled gate can submit.
			std::lock_guard transition(m_gateMutex);
			const auto gate = m_gate.load();
			if (gate == Gate::Prepared)
				return;
			if (gate == Gate::Cancelled)
				completion = CompletionStatus::Failed;
			else
			{
				if (!m_work->Submitted())
				{
					const auto producer = m_work->ProducerCompletion();
					if (producer == CompletionStatus::Pending)
						return;
					if (producer == CompletionStatus::Failed || queue.Submit(m_work) != VK_SUCCESS)
						completion = CompletionStatus::Failed;
					else
						completion = m_work->PollCompletion();
				}
				else
					completion = m_work->PollCompletion();
			}
		}
		if (completion != CompletionStatus::Pending)
		{
			m_work->ReleaseSources();
			Publish(completion);
		}
	}
	bool Retired() const noexcept
	{
		return m_output->PollCompletion() != CompletionStatus::Pending && m_work->NativeIdle();
	}
	void AbandonUnfinished() noexcept
	{
		std::lock_guard lock(m_gateMutex);
		Publish(CompletionStatus::Failed);
		// A stopped worker cannot submit a producer that becomes ready later.
		// Already submitted native work remains charged until its actual fence.
		if (!m_work->Submitted())
		{
			if (m_gate.load() == Gate::Prepared)
				m_work->CancelProducer();
			m_gate.store(Gate::Cancelled);
		}
	}

  private:
	enum class Gate
	{
		Prepared,
		Committed,
		Cancelled
	};
	void Publish(CompletionStatus status) noexcept
	{
		m_output->m_resources->PublishTerminal(status, m_gate.load() == Gate::Committed);
	}
	std::atomic<Gate> m_gate{Gate::Prepared};
	std::mutex m_gateMutex;
	std::shared_ptr<FrameReservation> m_admission;
	std::shared_ptr<VulkanInspectionWork> m_work;
	std::shared_ptr<VulkanInspectionFrame> m_output;
};

std::shared_ptr<FrameReservation> VulkanInspection::Reserve(VulkanInspectionKind kind)
{
	std::lock_guard lock(m_mutex);
	CheckAdmission();
	const auto& pool = kind == VulkanInspectionKind::History		? m_historyAdmissions
					   : kind == VulkanInspectionKind::Reprojection ? m_reprojectionAdmissions
																	: m_admissions;
	auto admission = pool.Reserve();
	if (!admission)
		throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Inspection preparation capacity exhausted");
	return admission;
}
void VulkanInspection::Publish(const std::shared_ptr<VulkanInspectionTransfer::State>& job)
{
	std::lock_guard lock(m_mutex);
	CheckAdmission();
	if (m_jobs.size() == m_capacity)
		throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Inspection preparation capacity exhausted");
	// The final admission check precedes one-shot semaphore export/import.
	// Publication cannot allocate: the bounded list is fully reserved.
	job->ImportReady(m_queue->External());
	m_jobs.push_back(job);
	m_changed.notify_all();
}
VkResult VulkanInspection::Drain(std::uint64_t timeout)
{
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::nanoseconds(std::min<std::uint64_t>(timeout, 5000000000ULL));
	std::unique_lock lock(m_mutex);
	for (;;)
	{
		if (m_workerStopped.load())
		{
			lock.unlock();
			Poll();
			lock.lock();
		}
		if (m_jobs.empty())
			return m_queue->Device()->lost.load() ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
		if (std::chrono::steady_clock::now() >= deadline)
			return VK_TIMEOUT;
		m_changed.wait_for(lock, std::chrono::milliseconds(1));
	}
}
VkResult VulkanInspection::Close(std::uint64_t timeout)
{
	std::lock_guard lifecycle(m_lifecycle);
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		for (auto& job : m_jobs)
			job->Cancel();
	}
	const auto result = Drain(timeout);
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
	{
		std::lock_guard lock(m_mutex);
		for (auto& job : m_jobs)
			job->AbandonUnfinished();
	}
	return result;
}

void VulkanInspection::CheckAdmission() const
{
	if (m_closed)
		throw VulkanError(VK_ERROR_INITIALIZATION_FAILED, "Inspection admission closed");
}
void VulkanInspection::Poll()
{
	std::lock_guard polling(m_polling);
	std::array<std::shared_ptr<VulkanInspectionTransfer::State>, SnapshotBudget::MaxCapacity + 3 + 32> pending;
	std::size_t count = 0;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& job : m_jobs)
			pending.at(count++) = job;
	}
	for (std::size_t index = 0; index < count; ++index)
		pending[index]->Advance(*m_queue);
	std::array<std::shared_ptr<VulkanInspectionTransfer::State>, SnapshotBudget::MaxCapacity + 3 + 32> retired;
	count = 0;
	{
		std::lock_guard lock(m_mutex);
		for (auto& job : m_jobs)
			if (job->Retired())
				retired.at(count++) = std::move(job);
		std::erase(m_jobs, nullptr);
	}
	m_queue->Collect();
}
void VulkanInspection::Run() noexcept
{
	for (;;)
	{
		try
		{
			Poll();
		}
		catch (...)
		{
			std::fputs("SimXR: inspection worker observation failed\n", stderr);
		}
		std::unique_lock lock(m_mutex);
		if (m_stopping)
		{
			m_workerStopped.store(true);
			return;
		}
		m_changed.wait_for(lock, std::chrono::milliseconds(1));
		if (m_stopping)
		{
			m_workerStopped.store(true);
			return;
		}
	}
}

VulkanInspectionFrame::VulkanInspectionFrame(std::shared_ptr<Resources> resources, std::span<const FrameView> views)
	: FrameOutput(views), m_resources(std::move(resources))
{
}
std::shared_ptr<VulkanInspectionFrame>
VulkanInspectionFrame::Compose(const CompositionPlan& plan,
							   std::span<const std::shared_ptr<const VulkanInspectionFrame>> sources)
{
	if (sources.empty() || sources.size() != plan.draws.size() || !sources.front())
		throw std::invalid_argument("Retained composition requires one source per draw");
	const auto queue = sources.front()->m_resources->Queue();
	std::vector<VulkanRetainedSource> retained;
	retained.reserve(sources.size());
	bool pending = false;
	for (const auto& source : sources)
	{
		if (!source || source->m_resources->Queue() != queue)
			throw std::invalid_argument("Retained composition sources must share one owned queue");
		const auto status = source->PollCompletion();
		if (status == CompletionStatus::Failed)
			throw VulkanError(VK_ERROR_INITIALIZATION_FAILED, "Retained composition source failed");
		pending |= status == CompletionStatus::Pending;
		retained.push_back({source->m_resources->Image(), source->m_resources->Budget()});
	}
	if (pending)
		return {};
	auto frame = queue->PrepareComposition(plan, retained);
	auto resources = Resources::Create(queue, frame->RetainImage(), frame->Reservation(), {}, frame,
									   sources.front()->m_resources->DeliveryRetired());
	auto output =
		std::shared_ptr<VulkanInspectionFrame>(new VulkanInspectionFrame(std::move(resources), frame->Views()));
	queue->SubmitComposition(frame);
	return output;
}

VulkanInspectionFrame::~VulkanInspectionFrame() = default;
CompletionStatus VulkanInspectionFrame::PollCompletion() const noexcept
{
	return m_resources->Poll();
}
const VulkanImage& VulkanInspectionFrame::Image() const
{
	return *m_resources->Image();
}
VkImageView VulkanInspectionFrame::View(std::uint32_t eye) const
{
	return m_resources->View(eye);
}

bool VulkanInspectionFrame::PromoteCaptureBudget(const FramePool& captures,
												 std::shared_ptr<FrameReservation> reservation) const
{
	return m_resources->Promote(captures, std::move(reservation));
}

VulkanImageReadback::VulkanImageReadback(std::shared_ptr<VulkanInspectionQueue> queue,
										 std::shared_ptr<VulkanInspectionWork> work)
	: m_queue(std::move(queue)), m_work(std::move(work))
{
}
CompletionStatus VulkanImageReadback::PollCompletion() const noexcept
{
	try
	{
		m_queue->Collect();
	}
	catch (...)
	{
	}
	return m_work->PollCompletion();
}

std::shared_ptr<const VulkanImagePixels> VulkanImageReadback::Pixels()
{
	if (PollCompletion() != CompletionStatus::Complete)
		return {};
	std::lock_guard lock(m_mutex);
	if (m_pixels)
		return m_pixels;
	m_pixels = m_work->ReadPixels();
	return m_pixels;
}

std::shared_ptr<VulkanImageReadback> VulkanInspectionFrame::Export(std::optional<std::uint32_t> layer,
																   std::shared_ptr<const void> retirementOwner) const
{
	if (PollCompletion() != CompletionStatus::Complete)
		throw VulkanError(VK_NOT_READY, "Inspection image is not ready for explicit export");
	auto sourceBudget = m_resources->Budget();
	auto work = m_resources->Queue()->Readback(m_resources->Image(), std::move(sourceBudget), layer,
											   std::move(retirementOwner));
	auto result = std::shared_ptr<VulkanImageReadback>(new VulkanImageReadback(m_resources->Queue(), work));
	CheckVulkan(m_resources->Queue()->Submit(work), "Submit explicit export on owned inspection queue");
	return result;
}

VulkanImageDelivery::VulkanImageDelivery(std::shared_ptr<Resources> resources) : m_resources(std::move(resources)) {}
VulkanImageDelivery::~VulkanImageDelivery() = default;
SharedImageKey VulkanImageDelivery::Key() const noexcept
{
	return m_resources->native->Key();
}
const VulkanExternalImageDescriptor& VulkanImageDelivery::Descriptor() const noexcept
{
	return m_resources->native->Descriptor();
}
CompletionStatus VulkanImageDelivery::PollCompletion() const noexcept
{
	try
	{
		m_resources->Queue()->Collect();
	}
	catch (...)
	{
	}
	return m_resources->native->Poll();
}
CompletionStatus VulkanImageDelivery::PollConsumerCompletion() const noexcept
{
	return m_resources->native->ConsumerCompletion();
}
ImageCompletionKind VulkanImageDelivery::CompletionKind() const noexcept
{
	return m_resources->Queue()->CompletionKind();
}
void VulkanImageDelivery::RegisterConsumer(const VulkanExternalIdentity& identity, VulkanExternalHandle& fd)
{
	m_resources->native->RegisterConsumer(m_resources->Queue()->External(), identity, fd, CompletionKind());
}
VulkanDeliveryHandles VulkanImageDelivery::ExportHandles(bool expose)
{
	return m_resources->native->ExportHandles(expose);
}
void VulkanImageDelivery::MarkExposed() noexcept
{
	m_resources->native->MarkExposed();
}

std::function<void()> VulkanInspectionFrame::Collector() const
{
	return [weak = std::weak_ptr<VulkanInspectionQueue>{m_resources->Queue()}]
	{
		if (const auto queue = weak.lock())
			queue->Collect();
	};
}
ImageCompletionKind VulkanInspectionFrame::CompletionKind() const noexcept
{
	return m_resources->Queue()->CompletionKind();
}
bool VulkanInspectionFrame::CanShare() const noexcept
{
	return m_resources->Queue()->CanShare();
}
std::shared_ptr<VulkanImageDelivery> VulkanInspectionFrame::Share() const
{
	if (!CanShare())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Consumer external completion is unavailable");
	if (PollCompletion() != CompletionStatus::Complete)
		throw VulkanError(VK_NOT_READY, "Inspection image is not ready for sharing");
	auto& queue = *m_resources->Queue();
	queue.Collect();
	auto slot = DeliveryBudget().Reserve(false);
	if (!slot)
		throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Shared image delivery capacity exhausted");
	auto sourceBudget = m_resources->Budget();
	auto native = VulkanDeliveryNative::Prepare(queue, m_resources->Image(), std::move(sourceBudget), std::move(slot),
												m_resources->DeliveryRetired());
	auto resources = std::make_shared<VulkanImageDelivery::Resources>(m_resources->Queue(), native);
	auto result = std::shared_ptr<VulkanImageDelivery>(new VulkanImageDelivery(std::move(resources)));
	// Both retirement lists reserve their complete capacities before queue use.
	DeliveryRetirement().Add(native);
	const auto submitted = native->Submit(queue);
	CheckVulkan(submitted, "Submit shared image on owned inspection queue");
	return result;
}

VulkanInspectionTransfer::VulkanInspectionTransfer(std::shared_ptr<State> state) : m_state(std::move(state)) {}
VulkanInspectionTransfer::~VulkanInspectionTransfer()
{
	m_state->Cancel();
}
void VulkanInspectionTransfer::Commit() noexcept
{
	m_state->Commit();
}
std::shared_ptr<VulkanInspectionFrame> VulkanInspectionTransfer::Output() const
{
	return m_state->Output();
}

VulkanInspection::VulkanInspection(const VulkanExternalIdentity& identity, std::size_t capacity,
								   std::shared_ptr<VulkanInspectionValidation> validation)
	: m_capacity((capacity ? capacity : SnapshotBudget::MaxCapacity) + 3 + 32),
	  m_admissions(
		  [capacity]
		  {
			  return capacity ? capacity : SnapshotBudget::Capacity();
		  })
{
	if (capacity > SnapshotBudget::MaxCapacity)
		throw std::invalid_argument("Invalid inspection capacity");
	DeliveryRetirement().Start();
	m_queue = std::make_shared<VulkanInspectionQueue>(VulkanInspectionDevice(identity, std::move(validation)));
	m_jobs.reserve(m_capacity);
	m_worker = std::thread(&VulkanInspection::Run, this);
}
ImageCompletionKind VulkanInspection::CompletionKind() const noexcept
{
	return m_queue->CompletionKind();
}
bool VulkanInspection::CanShare() const noexcept
{
	return m_queue->CanShare();
}
VulkanInspection::~VulkanInspection()
{
	Close();
}

std::unique_ptr<VulkanInspectionTransfer> VulkanInspection::Prepare(std::shared_ptr<VulkanFrame> source,
																	std::function<void(CompletionStatus)> completed,
																	std::function<void()> retired,
																	VulkanInspectionKind kind,
																	std::function<void(SharedImageKey)> deliveryRetired)
{
	if (!source || !source->ExternalImage())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Frame is not externally shareable");
	auto admission = Reserve(kind);
	if (!source->ClaimInspection())
		throw VulkanError(VK_ERROR_TOO_MANY_OBJECTS, "Frame already has an inspection consumer");
	struct Claim
	{
		std::shared_ptr<VulkanFrame> source;
		bool retained = false;
		~Claim()
		{
			if (!retained)
				source->CancelInspection();
		}
	} claim{source};
	auto work = m_queue->ImportCopy(source);
	work->Image()->retired = std::move(retired);
	auto resources = VulkanInspectionFrame::Resources::Create(m_queue, work->Image(), source->Reservation(),
															  std::move(completed), {}, std::move(deliveryRetired));
	auto output =
		std::shared_ptr<VulkanInspectionFrame>(new VulkanInspectionFrame(std::move(resources), source->Views()));
	auto job =
		std::make_shared<VulkanInspectionTransfer::State>(std::move(admission), std::move(work), std::move(output));
	auto transfer = std::unique_ptr<VulkanInspectionTransfer>(new VulkanInspectionTransfer(job));
	claim.retained = true;
	Publish(job);
	return transfer;
}

} // namespace simxr
