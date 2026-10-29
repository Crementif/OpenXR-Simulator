#include "presentation/RotationalDisplay.h"
#include "Pch.h"

#include "math/Math.h"

#include <new>

namespace simxr
{
static bool SameOrientation(const PoseSample& left, const PoseSample& right)
{
	const auto& a = left.pose.orientation;
	const auto& b = right.pose.orientation;
	return (a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w) ||
		   (a.x == -b.x && a.y == -b.y && a.z == -b.z && a.w == -b.w);
}

void RotationalDisplay::Reset() noexcept
{
	m_source.reset();
	m_pending.reset();
	m_ready.reset();
	m_failure = XR_SUCCESS;
}

PresentationImage RotationalDisplay::Process(const PresentationTick& baseline, const PoseSample& head)
{
	if (m_source != baseline.source || !baseline.output)
	{
		Reset();
		m_source = baseline.source;
	}
	PresentationImage result{baseline.output};
	if (!baseline.output)
		return result;
	const auto& source = baseline.reprojection.source;
	if (!source)
	{
		result.result =
			baseline.reprojection.status == XR_SUCCESS ? XR_ERROR_FEATURE_UNSUPPORTED : baseline.reprojection.status;
		return result;
	}
	const auto submitted = source->SubmittedHead();
	result.head = submitted;
	if (m_pending)
	{
		const auto status = m_pending->output->PollCompletion();
		if (status == CompletionStatus::Complete)
		{
			result.completionLatency =
				std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - m_pending->submitted);
			m_ready = std::move(m_pending);
			m_pending.reset();
		}
		else if (status == CompletionStatus::Failed)
		{
			m_pending.reset();
			m_failure = XR_ERROR_RUNTIME_FAILURE;
		}
	}
	if (m_ready)
	{
		result.output = m_ready->output;
		result.head = m_ready->head;
		result.reprojected = true;
	}
	if (XR_FAILED(m_failure))
	{
		result.result = m_failure;
		return result;
	}
	const auto normalized = NormalizedPose({head.pose.orientation, submitted.pose.position});
	if (!(head.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) || !normalized ||
		(submitted.flags & ValidPoseFlags) != ValidPoseFlags)
	{
		result.result = XR_ERROR_POSE_INVALID;
		return result;
	}
	if (head.sampleTime < submitted.sampleTime || (m_ready && head.sampleTime < m_ready->head.sampleTime))
	{
		result.result = XR_TIMEOUT_EXPIRED;
		return result;
	}
	auto requested = head;
	requested.pose = *normalized;
	requested.flags =
		(head.flags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) |
		(submitted.flags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT));
	const auto original = NormalizedPose(submitted.pose);
	if (!original)
	{
		result.result = XR_ERROR_POSE_INVALID;
		return result;
	}
	auto normalizedSubmitted = submitted;
	normalizedSubmitted.pose = *original;
	if (SameOrientation(requested, normalizedSubmitted))
	{
		m_ready.reset();
		result.output = baseline.output;
		result.head = submitted;
		result.reprojected = false;
		return result;
	}
	if (m_pending)
	{
		result.result = XR_TIMEOUT_EXPIRED;
		return result;
	}
	if (m_ready && SameOrientation(requested, m_ready->head))
		return result;
	const auto start = std::chrono::steady_clock::now();
	std::shared_ptr<const FrameOutput> output;
	try
	{
		result.result = source->Reproject(requested, output);
	}
	catch (const std::bad_alloc&)
	{
		result.result = XR_ERROR_OUT_OF_MEMORY;
	}
	result.submissionTime = std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start);
	if (result.result == XR_SUCCESS)
	{
		if (!output)
			m_failure = result.result = XR_ERROR_RUNTIME_FAILURE;
		else
		{
			m_pending = Image{std::move(output), requested, start};
			result.result = XR_TIMEOUT_EXPIRED;
		}
	}
	else if (result.result != XR_TIMEOUT_EXPIRED && result.result != XR_ERROR_LIMIT_REACHED &&
			 result.result != XR_ERROR_OUT_OF_MEMORY)
		m_failure = result.result;
	return result;
}
} // namespace simxr
