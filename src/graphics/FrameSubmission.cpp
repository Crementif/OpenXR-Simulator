#include "graphics/FrameSubmission.h"
#include "Pch.h"

#include "runtime/Session.h"

#include <functional>

namespace simxr
{
bool FrameCaptureRequest::IgnoreFailure(XrResult result) const noexcept
{
	return (history && (result == XR_ERROR_LIMIT_REACHED || result == XR_ERROR_OUT_OF_MEMORY)) ||
		   (image && (result == XR_ERROR_FEATURE_UNSUPPORTED || result == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED));
}

FrameSubmission::FrameSubmission(Session& session, FrameSubmissionBackend& backend)
	: m_session(session), m_backend(backend)
{
}

FrameSubmission::~FrameSubmission()
{
	if (m_reservation && !m_backend.Submitted() && m_session.Captures().ProducerReservation(m_key, {}) == m_reservation)
		m_session.Captures().AbandonProducer(m_key, {});
	for (const auto& request : m_captures)
	{
		const auto prepared = std::any_of(m_prepared.captures.begin(), m_prepared.captures.end(),
										  [&](const CaptureOutput& output)
										  {
											  return output.target == request.target && output.output;
										  });
		if (!request.history && request.reservation && (!m_backend.Submitted() || !prepared) &&
			m_session.Captures().ProducerReservation(m_key, request.target) == request.reservation)
			m_session.Captures().AbandonProducer(m_key, request.target);
	}
}

const CompositionPlan& FrameSubmission::Composition() const noexcept
{
	return m_composition.plan;
}

XrResult FrameSubmission::PrepareCaptures()
{
	const auto targets = m_session.Captures().RequestedTargets(m_key);
	m_captures.reserve(targets.size());
	for (const auto target : targets)
	{
		FrameCaptureRequest request;
		request.target = target;
		if (target.IsFilteredComposition())
		{
			request.composition = m_composition.plan;
			const bool projection = target.kind == CaptureKind::ProjectionLayers;
			std::erase_if(request.composition->draws,
						  [projection](const CompositionDraw& draw)
						  {
							  return (draw.constants.control[0] == 0) != projection;
						  });
			if (request.composition->draws.empty())
				continue;
			if (target.kind == CaptureKind::QuadLayers)
				request.quadBounds = CompositionQuadBounds(*request.composition);
		}
		else if (target.IsLayerImage())
		{
			if (target.IsDepth())
			{
				const auto* depth = FindLayerDepth(*m_composition.layers, target.layer, target.view);
				if (depth)
				{
					request.depth = *depth;
					request.image = depth->image;
				}
			}
			else
				request.image = FindLayerImage(*m_composition.layers, target.layer, target.view);
			if (!request.image || !request.image->rectangle.extent.width || !request.image->rectangle.extent.height)
				continue;
		}
		else
			continue;
		m_captures.push_back(std::move(request));
		auto& retained = m_captures.back();
		const auto result = m_session.PrepareCaptureProducer(m_key, retained.reservation, target);
		if (XR_FAILED(result))
			return result;
		if (!retained.reservation)
			m_captures.pop_back();
	}
	return XR_SUCCESS;
}

void FrameSubmission::PreparePreviewHistory()
{
	FrameCaptureRequest request;
	request.history = request.preview = true;
	request.composition = m_composition.plan;
	auto& plan = *request.composition;
	const auto largest = std::max(plan.width, plan.height);
	if (!largest)
		return;
	if (largest > FramePreviewHistory::Extent)
	{
		plan.width =
			std::max(1u, static_cast<std::uint32_t>(std::uint64_t{plan.width} * FramePreviewHistory::Extent / largest));
		plan.height = std::max(
			1u, static_cast<std::uint32_t>(std::uint64_t{plan.height} * FramePreviewHistory::Extent / largest));
	}
	request.historyPool = m_session.PreviewHistory().Budget();
	request.historyReservation = m_session.PreviewHistory().Reserve(m_key, false);
	if (request.historyReservation)
		request.reservation = m_backend.HistoryReservations().Reserve();
	m_captures.push_back(std::move(request));
}

void FrameSubmission::PrepareQuadHistory()
{
	const CaptureTarget target{CaptureKind::QuadLayers};
	if (!m_session.SupportsImageHistory() || !m_session.SupportsCapture(target))
		return;
	const auto requested = m_session.Captures().RequestedTargets(m_key);
	if (std::find(requested.begin(), requested.end(), target) != requested.end())
		return;
	try
	{
		FrameCaptureRequest request;
		request.target = target;
		request.history = true;
		request.composition = m_composition.plan;
		std::erase_if(request.composition->draws,
					  [](const CompositionDraw& draw)
					  {
						  return draw.constants.control[0] != 1;
					  });
		request.quadBounds = CompositionQuadBounds(*request.composition);
		m_captures.push_back(std::move(request));
		auto& history = m_captures.back();
		const auto& plan = *history.composition;
		if (plan.draws.empty() || std::none_of(history.quadBounds->begin(), history.quadBounds->end(),
											   [](const XrRect2Di& rectangle)
											   {
												   return rectangle.extent.width > 0 && rectangle.extent.height > 0;
											   }))
			return;
		const auto bytes = static_cast<std::uint64_t>(plan.width) * plan.height * 8;
		if (!bytes || bytes > FrameImageHistory::MaxImageBytes)
			return;
		history.historyPool = m_session.ImageHistory().Budget(ImageHistoryView::Quads);
		history.historyReservation = history.historyPool->Reserve();
		if (history.historyReservation)
			history.reservation = m_backend.HistoryReservations().Reserve();
	}
	catch (const std::bad_alloc&)
	{
		// Optional history leaves a gap without rejecting the application's frame.
	}
}

XrResult FrameSubmission::Execute(const XrFrameEndInfo& info, const SwapchainDescriptionResolver& describe)
{
	if (std::exchange(m_executed, true))
		return XR_ERROR_CALL_ORDER_INVALID;
	const auto snapshot = m_session.Snapshot();
	if (!snapshot.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	const auto begun = m_session.Scheduler().Snapshot().begun;
	if (!begun)
		return XR_ERROR_CALL_ORDER_INVALID;
	m_key = {snapshot.generation, begun->id};
	const auto trace = [&](std::string_view stage, XrResult result = XR_SUCCESS)
	{
		m_session.History().Trace(FrameTraceKind::RuntimeWork, m_key, {Duration{info.displayTime}}, result,
								  FrameDiagnosticCategory::Performance, 0, stage);
	};
	trace("end_started");
	try
	{
		auto result = PrepareSessionComposition(m_session, info, describe, m_composition);
		if (XR_FAILED(result))
		{
			m_session.History().Trace(FrameTraceKind::Validation, m_key, {Duration{info.displayTime}}, result,
									  FrameDiagnosticCategory::Validation, 0, "submitted_layers");
			return result;
		}
		trace("composition_prepared");
		auto dropLive = [&]
		{
			m_prepared = {};
			FramePrediction prediction;
			const auto result =
				m_session.EndFrame(prediction, &info, m_composition.layers, {}, {}, {}, FrameSubmissionPolicy::Live);
			if (XR_SUCCEEDED(result))
				m_session.PreviewHistory().Append(m_key, {}, false);
			return result;
		};
		bool live = true;
		if (info.layerCount)
		{
			result = m_session.PrepareCaptureProducer(m_key, m_reservation);
			if (XR_FAILED(result))
				return result;
			live = !m_reservation;
			if (live)
				m_reservation = m_backend.LiveReservations().Reserve();
			if (!m_reservation)
				return live && m_session.Captures().RequestedTargets(m_key).empty() ? dropLive()
																					: XR_ERROR_LIMIT_REACHED;
			trace("producers_reserved");
			result = PrepareCaptures();
			if (XR_FAILED(result))
				return result;
			trace("preview_started");
			PreparePreviewHistory();
			trace("preview_reserved");
			PrepareQuadHistory();
			result = m_backend.Prepare(m_key, m_composition, m_reservation, m_captures, m_prepared);
			if (result == XR_ERROR_LIMIT_REACHED && live && m_session.Captures().RequestedTargets(m_key).empty())
				return dropLive();
			if (XR_FAILED(result))
				return result;
		}
		trace("graphics_prepared");
		std::function<XrResult()> submit;
		if (m_prepared.producer && m_prepared.output)
			submit = [&]
			{
				const auto result = m_backend.Submit();
				if (XR_SUCCEEDED(result))
					m_session.History().Trace(FrameTraceKind::NativeSubmitted, m_key, m_composition.layers->displayTime,
											  XR_SUCCESS, FrameDiagnosticCategory::Timing, 0, "application_queue");
				else
					m_session.Captures().Fail(m_key, result, m_prepared.producer);
				return result;
			};
		FramePrediction prediction;
		result =
			m_session.EndFrame(prediction, &info, m_composition.layers, m_prepared.output, submit, m_prepared.captures,
							   live ? FrameSubmissionPolicy::Live : FrameSubmissionPolicy::Exact,
							   m_prepared.reprojection, m_prepared.timing);
		if (XR_SUCCEEDED(result))
		{
			const auto record = m_session.History().Find(m_key);
			const auto* tracking =
				record && record->submission && record->submission->tracking ? &*record->submission->tracking : nullptr;
			m_session.PreviewHistory().Append(m_key, m_prepared.preview, info.layerCount == 0, tracking,
											  {Duration{info.displayTime}}, prediction.displayPeriod);
		}
		trace("end_finished", result);
		return result;
	}
	catch (const std::bad_alloc&)
	{
		return XR_ERROR_OUT_OF_MEMORY;
	}
}
} // namespace simxr
