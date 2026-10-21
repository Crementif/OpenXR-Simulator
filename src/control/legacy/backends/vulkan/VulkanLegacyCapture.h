#pragma once

#include "control/legacy/LegacyCaptureImage.h"
#include "control/legacy/LegacyFileChannel.h"
#include "graphics/backends/vulkan/VulkanReadbackBatch.h"
#include "runtime/Session.h"

#include <array>
#include <deque>
#include <mutex>

namespace flicker
{
class Diagnostics;
}

namespace simxr
{
struct VulkanCaptureSource
{
	VulkanReadbackRegion region;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::uint32_t imageIndex = 0;
};

// A synchronous native binding view. Capture preparation copies the resolved
// values; neither this interface nor application structure pointers are retained.
class VulkanCaptureBindings
{
  public:
	virtual ~VulkanCaptureBindings() = default;
	virtual std::optional<VulkanCaptureSource> ResolveSource(const LayerImageDescription& image) const = 0;
	virtual void MarkSourceUsed(const VulkanCaptureSource& source,
								const std::shared_ptr<VulkanReadbackBatch>& completion) const noexcept = 0;
};

class VulkanLegacyCapture
{
  public:
	VulkanLegacyCapture(std::shared_ptr<VulkanDevice> device, std::shared_ptr<Session> session,
						std::shared_ptr<LegacyFileChannel> files, std::shared_ptr<flicker::Diagnostics> diagnostics);
	~VulkanLegacyCapture();
	void Request(LegacyCaptureRequest request, FrameId frame);
	// Caller holds the application's binding-queue access. No GPU wait occurs.
	void Submit(const XrFrameEndInfo& frame, const CompositionPlan& composition, const VulkanCaptureBindings& bindings);
	void Poll();
	VkResult Close(std::uint64_t timeout);

  private:
	struct RequestState
	{
		LegacyCaptureRequest selection;
		FrameId frame;
	};
	struct Job;
	struct ProjectionRecord
	{
		FrameId frame;
		XrTime time = 0;
		XrPosef pose;
		std::array<XrFovf, 2> fov;
		std::array<XrRect2Di, 2> rect;
	};
	std::unique_ptr<Job> Prepare(const CompositionPlan& composition, XrExtent2Di outputExtent,
								 const VulkanCaptureBindings& bindings);
	std::optional<std::size_t> AddSource(Job& job, const LayerImageDescription& image,
										 const VulkanCaptureBindings& bindings);
	void RecordProjection(const XrCompositionLayerProjection& projection, XrTime time);
	void DumpProjection();
	void PollJobs();
	void CompleteImage(Job& job);
	void Log(std::string_view message) noexcept;

	const std::shared_ptr<VulkanDevice> m_device;
	const std::shared_ptr<Session> m_session;
	const std::shared_ptr<LegacyFileChannel> m_files;
	const std::shared_ptr<flicker::Diagnostics> m_diagnostics;
	std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::optional<RequestState> m_request;
	std::deque<std::unique_ptr<Job>> m_jobs;
	std::array<ProjectionRecord, 64> m_projectionHistory{};
	std::size_t m_projectionHead = 0;
	std::size_t m_projectionCount = 0;
	bool m_closed = false;
};
} // namespace simxr
