#include "platform/linux/VulkanFrameSubmission.h"
#include "platform/linux/VulkanInstanceGraphics.h"

#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "runtime/Session.h"
#include "runtime/validation/LayerValidation.h"

#include <utility>

namespace simxr
{
VulkanFrameSubmission::VulkanFrameSubmission(Session& session, VulkanBackend& backend, VulkanInspection* inspection,
											 const VulkanFrameBindings& bindings, FrameKey key)
	: m_session(session), m_backend(backend), m_inspection(inspection), m_bindings(bindings), m_key(key)
{
}

VulkanFrameSubmission::~VulkanFrameSubmission()
{
	AbandonUnsubmitted(m_composed);
	for (const auto& capture : m_captures)
		AbandonUnsubmitted(capture.producer, capture.target);
}

void VulkanFrameSubmission::AbandonUnsubmitted(const std::shared_ptr<VulkanFrame>& producer, CaptureTarget target)
{
	if (producer && !producer->Submitted() &&
		m_session.Captures().ProducerReservation(m_key, target) == producer->Reservation())
		m_session.Captures().AbandonProducer(m_key, target);
}

const CompositionPlan& VulkanFrameSubmission::Composition() const noexcept
{
	return m_composition;
}

XrResult VulkanFrameSubmission::PrepareLayers(const XrFrameEndInfo& info)
{
	LayerValidationContext validation;
	validation.viewCount = m_session.ActiveViewCount();
	validation.depthEnabled = m_session.Extensions().Contains(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	validation.validSpace = [this](XrSpace space)
	{
		return m_session.Spaces().Owns(space);
	};
	validation.swapchain = [this](XrSwapchain swapchain)
	{
		return m_bindings.Describe(swapchain);
	};
	const auto result = BuildLayers(
		info, validation,
		[this](XrSpace space, RuntimeTime time, PoseSample& pose)
		{
			return m_session.Spaces().WorldPose(space, time, pose);
		},
		m_layers);
	if (XR_FAILED(result) || info.layerCount == 0)
		return result;
	const auto views = m_session.ViewsAt({Duration{info.displayTime}});
	if (views.views.size() < 2)
		return XR_ERROR_RUNTIME_FAILURE;
	const CompositionTarget target{
		views.views[0].description.width,
		views.views[0].description.height,
		{{{views.views[0].pose, views.views[0].fov}, {views.views[1].pose, views.views[1].fov}}}};
	return PrepareComposition(
		*m_layers, target,
		[this](XrSwapchain swapchain) -> std::optional<XrExtent2Di>
		{
			const auto description = m_bindings.Describe(swapchain);
			if (!description)
				return {};
			return XrExtent2Di{static_cast<std::int32_t>(description->width),
							   static_cast<std::int32_t>(description->height)};
		},
		m_composition);
}

XrResult VulkanFrameSubmission::PrepareOutputs()
{
	std::shared_ptr<FrameReservation> reservation;
	auto result = m_session.PrepareCaptureProducer(m_key, reservation);
	if (XR_FAILED(result))
		return result;
	m_live = !reservation;
	if (m_live)
	{
		reservation = m_backend.LiveReservations().Reserve();
		if (!reservation)
		{
			DropLiveOutput();
			return XR_SUCCESS;
		}
	}
	result = m_backend.Prepare(
		m_composition,
		[this](XrSwapchain swapchain)
		{
			return m_bindings.ReleasedImage(swapchain);
		},
		m_composed, std::move(reservation));
	if (m_live && result == XR_ERROR_LIMIT_REACHED)
	{
		DropLiveOutput();
		return XR_SUCCESS;
	}
	if (XR_FAILED(result))
		return result;
	m_output = m_composed;
	if (m_inspection)
	{
		m_composedTransfer = PrepareInspection(m_composed);
		m_output = m_composedTransfer->Output();
		result = PrepareCaptures();
		if (XR_FAILED(result))
			return result;
	}
	m_batch.reserve(m_captures.size() + 1);
	m_batch.push_back(m_composed);
	for (const auto& capture : m_captures)
		m_batch.push_back(capture.producer);
	return XR_SUCCESS;
}

XrResult VulkanFrameSubmission::PrepareCapture(const LayerImageDescription& image, const LayerDepthDescription* depth, Capture& capture,
											   std::shared_ptr<FrameReservation> reservation)
{
	try
	{
		if (depth)
			return m_backend.PrepareDepthCapture(*depth, capture.target.kind == CaptureKind::LayerDepthLinear,
				[this](XrSwapchain swapchain) { return m_bindings.ReleasedImage(swapchain); },
				capture.producer, std::move(reservation));
		return m_backend.PrepareCapture(
			image,
			[this](XrSwapchain swapchain)
			{
				return m_bindings.ReleasedImage(swapchain);
			},
			capture.producer, std::move(reservation));
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

XrResult VulkanFrameSubmission::PrepareCaptures()
{
	const auto targets = m_session.Captures().RequestedTargets(m_key);
	m_captures.reserve(targets.size());
	m_captureOutputs.reserve(targets.size());
	for (const auto target : targets)
	{
		if (!target.IsLayerImage())
			continue;
		const auto* depth = target.IsDepth() ? FindLayerDepth(*m_layers, target.layer, target.view) : nullptr;
		const auto image = target.IsDepth() ? (depth ? std::optional{depth->image} : std::nullopt)
			: FindLayerImage(*m_layers, target.layer, target.view);
		if (!image || !image->rectangle.extent.width || !image->rectangle.extent.height)
			continue;
		m_captures.push_back({target, image->source, {}, {}});
		auto& capture = m_captures.back();
		std::shared_ptr<FrameReservation> reservation;
		auto result = m_session.PrepareCaptureProducer(m_key, reservation, target);
		if (XR_FAILED(result))
			return result;
		if (!reservation)
		{
			m_captures.pop_back();
			continue;
		}
		result = PrepareCapture(*image, depth, capture, std::move(reservation));
		if (result == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED || result == XR_ERROR_FEATURE_UNSUPPORTED)
		{
			AbandonUnsubmitted(capture.producer, target);
			m_captures.pop_back();
			continue;
		}
		if (XR_FAILED(result))
			return result;
		capture.transfer = PrepareInspection(capture.producer, target);
		m_captureOutputs.push_back({target, capture.transfer->Output()});
	}
	return XR_SUCCESS;
}

void VulkanFrameSubmission::DropLiveOutput()
{
	m_composedTransfer.reset();
	m_output.reset();
	m_captureOutputs.clear();
	m_batch.clear();
}

std::unique_ptr<VulkanInspectionTransfer>
VulkanFrameSubmission::PrepareInspection(std::shared_ptr<VulkanFrame> source, CaptureTarget target)
{
	const auto journal = m_session.History().DiagnosticsJournal();
	const auto key = m_key;
	const auto time = m_layers->displayTime;
	const auto label = target.kind == CaptureKind::LayerDepth ? "owned_depth_view:"
		: target.kind == CaptureKind::LayerDepthLinear ? "owned_linear_depth_view:" : "owned_layer_view:";
	const auto detail = target.kind == CaptureKind::ComposedEyes ? std::string{"owned_composed"}
		: label + std::to_string(target.layer) + ":" + std::to_string(target.view);
	return m_inspection->Prepare(std::move(source),
		[journal, key, time, detail](CompletionStatus status)
		{
			journal->Trace(status == CompletionStatus::Complete ? FrameTraceKind::GpuComplete : FrameTraceKind::GpuFailed,
				key, time, status == CompletionStatus::Complete ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE,
				FrameDiagnosticCategory::Timing, 0, detail);
		},
		[journal, key, time, detail]
		{
			journal->Trace(FrameTraceKind::ImageRetired, key, time, XR_SUCCESS,
				FrameDiagnosticCategory::Timing, 0, detail);
		});
}

XrResult VulkanFrameSubmission::Submit()
{
	const auto result = m_backend.Submit(m_batch);
	if (m_composed->Submitted())
	{
		const auto completion = m_composed->Completion();
		for (const auto& draw : m_composition.draws)
			m_bindings.MarkReleasedUsed(draw.source.source, completion);
		for (const auto& capture : m_captures)
			m_bindings.MarkReleasedUsed(capture.source, completion);
	}
	if (XR_SUCCEEDED(result))
	{
		m_session.History().Trace(FrameTraceKind::NativeSubmitted, m_key, m_layers->displayTime,
			XR_SUCCESS, FrameDiagnosticCategory::Timing, 0, "application_queue");
		if (m_composedTransfer)
			m_composedTransfer->Commit();
		for (const auto& capture : m_captures)
			capture.transfer->Commit();
	}
	else
		m_session.Captures().Fail(m_key, result, m_composed);
	return result;
}

XrResult VulkanFrameSubmission::Execute(const XrFrameEndInfo& info)
{
	if (std::exchange(m_executed, true))
		return XR_ERROR_CALL_ORDER_INVALID;
	auto result = PrepareLayers(info);
	if (XR_FAILED(result))
	{
		m_session.History().Trace(FrameTraceKind::Validation, m_key, {Duration{info.displayTime}}, result,
			FrameDiagnosticCategory::Validation, 0, "submitted_layers");
		return result;
	}
	if (info.layerCount)
	{
		try
		{
			result = PrepareOutputs();
		}
		catch (const VulkanError& error)
		{
			if (m_live &&
				(error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY || error.Result() == VK_ERROR_TOO_MANY_OBJECTS))
			{
				DropLiveOutput();
				result = XR_SUCCESS;
			}
			else
			{
				if (error.Result() == VK_ERROR_DEVICE_LOST)
					m_backend.Device()->lost.store(true);
				return VulkanGraphicsFailure(error.Result());
			}
		}
		if (XR_FAILED(result))
			return result;
	}
	FramePrediction submitted;
	std::function<XrResult()> submit;
	if (m_composed && m_output)
		submit = [this]
		{
			return Submit();
		};
	return m_session.EndFrame(submitted, &info, m_layers, m_output, submit, m_captureOutputs,
							  m_live ? FrameSubmissionPolicy::Live : FrameSubmissionPolicy::Exact);
}
} // namespace simxr
