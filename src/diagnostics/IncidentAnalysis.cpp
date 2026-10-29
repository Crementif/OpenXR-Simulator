#include "Pch.h"
#include "diagnostics/IncidentAnalysis.h"

namespace flicker
{
IncidentAnalysis::IncidentAnalysis(bool ui) : m_ui(ui)
{
	if (m_ui)
		m_uiMonitor.emplace(std::filesystem::path{});
	else
		m_preview.emplace(std::filesystem::path{});
}

void IncidentAnalysis::Reset()
{
	m_segmentSamples = 0;
	if (m_ui)
		m_uiMonitor.emplace(std::filesystem::path{});
	else
		m_preview.emplace(std::filesystem::path{});
}

void IncidentAnalysis::Track(std::uint64_t frame)
{
	if (m_lastFrame && (*m_lastFrame == UINT64_MAX || frame != *m_lastFrame + 1))
	{
		++m_counts.gaps;
		Reset();
	}
	m_lastFrame = frame;
}

void IncidentAnalysis::Skip(std::uint64_t frame)
{
	Track(frame);
	++m_counts.skipped;
	Reset();
}

AnalysisObservation IncidentAnalysis::Observe(std::uint64_t frame, std::span<const std::byte> pixels,
	std::uint32_t width, std::uint32_t height, std::uint32_t stride, PixelOrder order,
	const std::array<std::array<std::int32_t, 4>, 2>& rectangles)
{
	const auto* source = reinterpret_cast<const std::uint8_t*>(pixels.data());
	if (!stride && width <= UINT32_MAX / 4)
		stride = width * 4;
	if (!ValidPixels(source, width, height, stride) || pixels.size() < std::size_t(stride) * height)
		throw std::invalid_argument("Invalid incident analysis pixels");
	Track(frame);
	if (m_ui && (rectangles[0][2] <= 0 || rectangles[0][3] <= 0 ||
		rectangles[1][2] <= 0 || rectangles[1][3] <= 0))
	{
		++m_counts.skipped;
		Reset();
		return {true, "missing_eye"};
	}
	++m_counts.observations;
	if (m_segmentSamples++)
		++m_counts.temporalPairs;
	flicker::IncidentUpdate update;
	std::string reason;
	if (m_ui)
	{
		flicker::UiFrameInfo info;
		for (std::size_t eye = 0; eye < 2; ++eye)
			std::copy(rectangles[eye].begin(), rectangles[eye].end(), info.rects[eye]);
		update = m_uiMonitor->Observe(source, width, height, stride, m_counts.observations, frame, info, order);
		if (update.anomaly)
			reason = m_uiMonitor->Snapshot().incidents.lastReason;
	}
	else
	{
		auto sample = flicker::Downsample(source, width, height, stride, frame, order);
		update = m_preview->Preview(std::move(sample), m_counts.observations);
		if (update.anomaly)
			reason = m_preview->Snapshot().incidents.lastReason;
	}
	return {false, std::move(reason)};
}

}
