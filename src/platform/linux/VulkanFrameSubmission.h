#pragma once

#include "graphics/Compositor.h"
#include "presentation/SnapshotStore.h"

#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr
{
class Session;
class VulkanBackend;
class VulkanFrame;
class VulkanImage;
class VulkanInspection;
class VulkanInspectionTransfer;

class VulkanFrameBindings
{
  public:
	virtual ~VulkanFrameBindings() = default;
	virtual std::optional<SwapchainDescription> Describe(XrSwapchain swapchain) const = 0;
	virtual std::shared_ptr<VulkanImage> ReleasedImage(XrSwapchain swapchain) const = 0;
	virtual void MarkReleasedUsed(XrSwapchain swapchain,
								  const std::shared_ptr<const GpuCompletion>& completion) const noexcept = 0;
};

// Caller serializes queue access and released-image selection through Execute.
// Dependencies outlive this synchronous scope; prepared work keeps its own
// source readers after submission. No GPU waits occur.
class VulkanFrameSubmission final
{
  public:
	VulkanFrameSubmission(Session& session, VulkanBackend& backend, VulkanInspection* inspection,
						  const VulkanFrameBindings& bindings, FrameKey key);
	~VulkanFrameSubmission();
	VulkanFrameSubmission(const VulkanFrameSubmission&) = delete;
	VulkanFrameSubmission& operator=(const VulkanFrameSubmission&) = delete;
	XrResult Execute(const XrFrameEndInfo& info);
	const CompositionPlan& Composition() const noexcept;

  private:
	struct Capture
	{
		CaptureTarget target;
		XrSwapchain source;
		std::shared_ptr<VulkanFrame> producer;
		std::unique_ptr<VulkanInspectionTransfer> transfer;
	};

	XrResult PrepareLayers(const XrFrameEndInfo& info);
	XrResult PrepareOutputs();
	XrResult PrepareCaptures();
	XrResult PrepareCapture(const LayerImageDescription& image, Capture& capture,
							std::shared_ptr<FrameReservation> reservation);
	XrResult Submit();
	void DropLiveOutput();
	void AbandonUnsubmitted(const std::shared_ptr<VulkanFrame>& producer, CaptureTarget target = {});

	Session& m_session;
	VulkanBackend& m_backend;
	VulkanInspection* const m_inspection;
	const VulkanFrameBindings& m_bindings;
	const FrameKey m_key;
	std::shared_ptr<const FrameLayers> m_layers;
	CompositionPlan m_composition;
	std::shared_ptr<VulkanFrame> m_composed;
	std::unique_ptr<VulkanInspectionTransfer> m_composedTransfer;
	std::shared_ptr<const FrameOutput> m_output;
	std::vector<Capture> m_captures;
	std::vector<CaptureOutput> m_captureOutputs;
	std::vector<std::shared_ptr<VulkanFrame>> m_batch;
	bool m_live = true;
	bool m_executed = false;
};
} // namespace simxr
