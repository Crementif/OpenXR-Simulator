#include "graphics/backends/vulkan/VulkanFrameSubmission.h"
#include "Pch.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanReprojection.h"
#include "runtime/Session.h"

namespace simxr
{
VulkanFrameSubmission::VulkanFrameSubmission(Session& session, VulkanBackend& backend, VulkanInspection* inspection,
											 const VulkanFrameBindings& bindings, bool retainLayers)
	: m_session(session), m_backend(backend), m_inspection(inspection), m_bindings(bindings),
	  m_retainLayers(retainLayers)
{}

VulkanFrameSubmission::~VulkanFrameSubmission() = default;

FramePool VulkanFrameSubmission::LiveReservations() const
{
	return m_backend.LiveReservations();
}

FramePool VulkanFrameSubmission::HistoryReservations() const
{
	return m_backend.HistoryReservations();
}

bool VulkanFrameSubmission::Submitted() const noexcept
{
	return m_composed && m_composed->Submitted();
}

XrResult VulkanFrameSubmission::PrepareOutputs(std::shared_ptr<FrameReservation> reservation,
											   std::span<const FrameCaptureRequest> captures)
{
	auto result = m_backend.Prepare(
		m_composition, [this](XrSwapchain swapchain) { return m_bindings.ReleasedImage(swapchain); }, m_composed,
		std::move(reservation));
	if (XR_FAILED(result))
		return result;
	m_output = m_composed;
	if (m_inspection)
	{
		m_composedTransfer = PrepareInspection(m_composed, {}, VulkanInspectionKind::Required);
		m_output = m_composedTransfer->Output();
		result = PrepareCaptures(captures);
		if (XR_FAILED(result))
			return result;
	}
	m_batch.reserve(m_captures.size() + 1);
	m_batch.push_back(m_composed);
	for (const auto& capture : m_captures)
		m_batch.push_back(capture.producer);
	return m_retainLayers ? PrepareReprojection() : XR_SUCCESS;
}

XrResult VulkanFrameSubmission::PrepareReprojection()
{
	if (!m_inspection)
	{
		m_reprojection.status = XR_ERROR_FEATURE_UNSUPPORTED;
		return XR_SUCCESS;
	}
	try
	{
		const auto capacity = m_batch.size() < 64 ? 64 - m_batch.size() : 0;
		m_reprojection.status = VulkanReprojectionPreparation::Prepare(
			m_backend, *m_inspection, *m_layers, m_target, m_head,
			[this](XrSwapchain swapchain) { return m_bindings.ReleasedImage(swapchain); }, capacity, m_retention,
			DeliveryRetirementObserver());
		if (m_reprojection.status == XR_SUCCESS)
		{
			const auto frames = m_retention->Frames();
			m_batch.insert(m_batch.end(), frames.begin(), frames.end());
			m_reprojection.source = m_retention->Source();
		}
	}
	catch (const std::bad_alloc&)
	{
		m_retention.reset();
		m_reprojection = {{}, XR_ERROR_OUT_OF_MEMORY};
	}
	switch (m_reprojection.status)
	{
	case XR_SUCCESS:
	case XR_ERROR_LIMIT_REACHED:
	case XR_ERROR_OUT_OF_MEMORY:
	case XR_ERROR_FEATURE_UNSUPPORTED:
	case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED:
	case XR_ERROR_POSE_INVALID:
		// Optional preparation failure preserves the baseline and its captures.
		return XR_SUCCESS;
	default:
		return m_reprojection.status;
	}
}

XrResult VulkanFrameSubmission::PrepareCapture(const LayerImageDescription& image, const LayerDepthDescription* depth,
											   Capture& capture, std::shared_ptr<FrameReservation> reservation)
{
	try
	{
		if (depth)
			return m_backend.PrepareDepthCapture(
				*depth, capture.target.kind == CaptureKind::LayerDepthLinear,
				[this](XrSwapchain swapchain) { return m_bindings.ReleasedImage(swapchain); }, capture.producer,
				std::move(reservation));
		return m_backend.PrepareCapture(
			image, [this](XrSwapchain swapchain) { return m_bindings.ReleasedImage(swapchain); }, capture.producer,
			std::move(reservation));
	}
	catch (const VulkanError& error)
	{
		switch (error.Result())
		{
		case VK_ERROR_FORMAT_NOT_SUPPORTED:
		case VK_ERROR_FEATURE_NOT_PRESENT:
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			return XR_ERROR_FEATURE_UNSUPPORTED;
		default:
			throw;
		}
	}
}

XrResult VulkanFrameSubmission::PrepareCaptures(std::span<const FrameCaptureRequest> requests)
{
	const auto required =
		std::count_if(requests.begin(), requests.end(), [](const auto& request) { return !request.history; });
	m_captures.reserve(required);
	m_captureOutputs.reserve(required);
	for (const auto& request : requests)
	{
		try
		{
			CaptureOutput output{request.target, {}, request.quadBounds};
			if (request.history)
			{
				m_captures.reserve(m_captures.size() + 1);
				if (!request.preview)
					m_captureOutputs.push_back(output);
			}
			if (!request.reservation)
			{
				continue;
			}
			Capture capture{request.target, request.image ? request.image->source : XR_NULL_HANDLE, {}, {}};
			const auto result = request.composition
									? m_backend.Prepare(
										  *request.composition,
										  [this](XrSwapchain swapchain) { return m_bindings.ReleasedImage(swapchain); },
										  capture.producer, request.reservation)
									: PrepareCapture(*request.image, request.depth ? &*request.depth : nullptr, capture,
													 request.reservation);
			if (request.IgnoreFailure(result))
				continue;
			if (XR_FAILED(result))
				return result;
			capture.transfer =
				PrepareInspection(capture.producer, request.target,
								  request.history ? VulkanInspectionKind::History : VulkanInspectionKind::Required);
			if (request.preview && capture.transfer->Output()->Image().allocationSize > FramePreviewHistory::SlotBytes)
				throw std::runtime_error("Preview allocation exceeds its history reservation");
			if (request.history &&
				!capture.transfer->Output()->PromoteCaptureBudget(*request.historyPool, request.historyReservation))
			{
				continue;
			}
			output.output = capture.transfer->Output();
			if (request.preview)
				m_preview = output.output;
			else if (request.history)
				m_captureOutputs.back() = std::move(output);
			else
				m_captureOutputs.push_back(std::move(output));
			m_captures.push_back(std::move(capture));
		}
		catch (const VulkanError& error)
		{
			if (!request.IgnoreFailure(VulkanGraphicsFailure(error.Result())))
				throw;
		}
		catch (const std::bad_alloc&)
		{
			if (!request.IgnoreFailure(XR_ERROR_OUT_OF_MEMORY))
				throw;
		}
	}
	return XR_SUCCESS;
}

std::unique_ptr<VulkanInspectionTransfer> VulkanFrameSubmission::PrepareInspection(std::shared_ptr<VulkanFrame> source,
																				   CaptureTarget target,
																				   VulkanInspectionKind kind)
{
	const auto journal = m_session.History().DiagnosticsJournal();
	const auto key = m_key;
	const auto time = m_layers->displayTime;
	const auto label = target.kind == CaptureKind::LayerDepth		  ? "owned_depth_view:"
					   : target.kind == CaptureKind::LayerDepthLinear ? "owned_linear_depth_view:"
																	  : "owned_layer_view:";
	const auto detail = target.kind == CaptureKind::ComposedEyes
							? std::string{kind == VulkanInspectionKind::History ? "owned_preview" : "owned_composed"}
						: target.kind == CaptureKind::ProjectionLayers ? std::string{"owned_projection_layers"}
						: target.kind == CaptureKind::QuadLayers
							? std::string{"owned_quad_layers"}
							: label + std::to_string(target.layer) + ":" + std::to_string(target.view);
	return m_inspection->Prepare(
		std::move(source),
		[journal, key, time, detail, producers = m_backend.LiveReservations()](CompletionStatus status) {
			journal->Trace(status == CompletionStatus::Complete ? FrameTraceKind::GpuComplete
																: FrameTraceKind::GpuFailed,
						   key, time, status == CompletionStatus::Complete ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE,
						   FrameDiagnosticCategory::Timing, 0, detail);
			// Collect completed producers on the inspection worker before the next application frame.
			producers.Collect();
		},
		[journal, key, time, detail] {
			journal->Trace(FrameTraceKind::ImageRetired, key, time, XR_SUCCESS, FrameDiagnosticCategory::Timing, 0,
						   detail);
		},
		kind, DeliveryRetirementObserver());
}

std::function<void(SharedImageKey)> VulkanFrameSubmission::DeliveryRetirementObserver() const
{
	return [journal = m_session.History().DiagnosticsJournal(), key = m_key,
			time = m_layers->displayTime](SharedImageKey image) {
		journal->Trace(FrameTraceKind::ImageRetired, key, time, XR_SUCCESS, FrameDiagnosticCategory::Timing,
					   image.image.value, "delivery_pool:" + std::to_string(image.pool.value));
	};
}

XrResult VulkanFrameSubmission::Submit()
{
	const auto result = m_backend.Submit(m_batch);
	if (m_composed->Submitted())
	{
		const auto completion = m_composed->Completion();
		if (m_retention)
			for (const auto swapchain : m_retention->Swapchains())
				m_bindings.MarkReleasedUsed(swapchain, completion);
		for (const auto& draw : m_composition.draws)
			m_bindings.MarkReleasedUsed(draw.source.source, completion);
		// Filtered draws are a subset of the composed draws and share this batch completion.
		for (const auto& capture : m_captures)
			if (capture.source != XR_NULL_HANDLE)
				m_bindings.MarkReleasedUsed(capture.source, completion);
	}
	if (XR_SUCCEEDED(result))
	{
		if (m_retention)
			m_retention->Commit();
		if (m_composedTransfer)
			m_composedTransfer->Commit();
		for (const auto& capture : m_captures)
			capture.transfer->Commit();
	}
	return result;
}

XrResult VulkanFrameSubmission::Prepare(FrameKey key, const SessionComposition& composition,
										std::shared_ptr<FrameReservation> reservation,
										std::span<const FrameCaptureRequest> captures, PreparedFrameSubmission& output)
{
	m_key = key;
	m_layers = composition.layers;
	m_target = composition.target;
	m_head = composition.head;
	m_composition = composition.plan;
	try
	{
		const auto result = PrepareOutputs(std::move(reservation), captures);
		if (XR_FAILED(result))
			return result;
		output = {m_composed, m_output, m_captureOutputs, m_reprojection, m_composed->Timing(), m_preview};
		return XR_SUCCESS;
	}
	catch (const VulkanError& error)
	{
		if (error.Result() == VK_ERROR_DEVICE_LOST)
			m_backend.Device()->lost.store(true);
		return VulkanGraphicsFailure(error.Result());
	}
}
} // namespace simxr
