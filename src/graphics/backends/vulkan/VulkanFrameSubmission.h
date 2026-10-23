#pragma once

#include "graphics/FrameSubmission.h"
#include "graphics/RotationalSource.h"
#include "graphics/SharedImageTypes.h"
#include "presentation/SnapshotStore.h"
#include <functional>

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
enum class VulkanInspectionKind;
class VulkanInspectionTransfer;
class VulkanReprojectionPreparation;

class VulkanFrameBindings
{
  public:
	virtual ~VulkanFrameBindings() = default;
	virtual std::optional<SwapchainDescription> Describe(XrSwapchain swapchain) const = 0;
	virtual std::shared_ptr<VulkanImage> ReleasedImage(XrSwapchain swapchain) const = 0;
	virtual void MarkReleasedUsed(XrSwapchain swapchain,
								  const std::shared_ptr<const GpuCompletion>& completion) const noexcept = 0;
};

// Caller serializes queue access and released-image selection through submission.
// Dependencies outlive this synchronous scope; prepared work keeps its own
// source readers after submission. No GPU waits occur.
class VulkanFrameSubmission final : public FrameSubmissionBackend
{
  public:
	VulkanFrameSubmission(Session& session, VulkanBackend& backend, VulkanInspection* inspection,
						  const VulkanFrameBindings& bindings, bool retainLayers = false);
	~VulkanFrameSubmission();
	VulkanFrameSubmission(const VulkanFrameSubmission&) = delete;
	VulkanFrameSubmission& operator=(const VulkanFrameSubmission&) = delete;
	FramePool LiveReservations() const override;
	FramePool HistoryReservations() const override;
	XrResult Prepare(FrameKey key, const SessionComposition& composition,
		std::shared_ptr<FrameReservation> reservation, std::span<const FrameCaptureRequest> captures,
		PreparedFrameSubmission& output) override;
	XrResult Submit() override;
	bool Submitted() const noexcept override;

  private:
	struct Capture
	{
		CaptureTarget target;
		XrSwapchain source;
		std::shared_ptr<VulkanFrame> producer;
		std::unique_ptr<VulkanInspectionTransfer> transfer;
	};

	XrResult PrepareOutputs(std::shared_ptr<FrameReservation> reservation, std::span<const FrameCaptureRequest> captures);
	XrResult PrepareReprojection();
	XrResult PrepareCaptures(std::span<const FrameCaptureRequest> requests);
	XrResult PrepareCapture(const LayerImageDescription& image, const LayerDepthDescription* depth, Capture& capture,
							std::shared_ptr<FrameReservation> reservation);
	std::unique_ptr<VulkanInspectionTransfer> PrepareInspection(std::shared_ptr<VulkanFrame> source,
		CaptureTarget target, VulkanInspectionKind kind);
	std::function<void(SharedImageKey)> DeliveryRetirementObserver() const;

	Session& m_session;
	VulkanBackend& m_backend;
	VulkanInspection* const m_inspection;
	const VulkanFrameBindings& m_bindings;
	FrameKey m_key{};
	const bool m_retainLayers;
	CompositionTarget m_target{};
	PoseSample m_head;
	ReprojectionInput m_reprojection;
	std::unique_ptr<VulkanReprojectionPreparation> m_retention;
	std::shared_ptr<const FrameLayers> m_layers;
	CompositionPlan m_composition;
	std::shared_ptr<VulkanFrame> m_composed;
	std::unique_ptr<VulkanInspectionTransfer> m_composedTransfer;
	std::shared_ptr<const FrameOutput> m_output;
	std::vector<Capture> m_captures;
	std::vector<CaptureOutput> m_captureOutputs;
	std::vector<std::shared_ptr<VulkanFrame>> m_batch;
};
} // namespace simxr
