#pragma once

#include "control/legacy/LegacyCaptureImage.h"
#include "control/legacy/LegacyFileChannel.h"
#include "graphics/backends/vulkan/VulkanFrameObserver.h"
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
class VulkanLegacyCapture final : public VulkanFrameObserver
{
  public:
	VulkanLegacyCapture(std::shared_ptr<VulkanDevice> device, std::shared_ptr<Session> session,
						std::shared_ptr<LegacyCaptureChannel> files, std::shared_ptr<flicker::Diagnostics> diagnostics);
	~VulkanLegacyCapture() override;
	void Request(LegacyCaptureRequest request, FrameId frame) override;
	void Observe(const XrFrameEndInfo& frame, const CompositionPlan& composition,
				 const VulkanCaptureBindings& bindings) noexcept override;
	// Caller holds the application's binding-queue access. No GPU wait occurs.
	void Submit(const XrFrameEndInfo& frame, const CompositionPlan& composition, const VulkanCaptureBindings& bindings);
	void Poll() override;
	VkResult Close(std::uint64_t timeout) override;

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
	const std::shared_ptr<LegacyCaptureChannel> m_files;
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
