#include "Pch.h"
#include "app/mcp/McpIncidentAnalysis.h"

#include <cstring>

namespace simxr::app::mcp
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
		++m_gaps;
		Reset();
	}
	m_lastFrame = frame;
}

void IncidentAnalysis::Skip(std::uint64_t frame)
{
	Track(frame);
	++m_skipped;
	m_skippedFrames.push_back({{"frame_id", std::to_string(frame)}, {"reason", "image_unavailable"}});
	Reset();
}

void IncidentAnalysis::Observe(std::uint64_t frame, std::span<const std::byte> cell,
	const std::array<std::array<std::int32_t, 4>, 2>& rectangles)
{
	constexpr unsigned width = 320, height = 90, header = 14;
	if (cell.size() != std::size_t(width) * (header + height) * 4)
		throw std::invalid_argument("Invalid incident analysis cell");
	Track(frame);
	if (m_ui && (rectangles[0][2] <= 0 || rectangles[0][3] <= 0 ||
		rectangles[1][2] <= 0 || rectangles[1][3] <= 0))
	{
		++m_skipped;
		m_skippedFrames.push_back({{"frame_id", std::to_string(frame)}, {"reason", "missing_eye"}});
		Reset();
		return;
	}
	std::vector<std::uint8_t> bgra(std::size_t(width) * height * 4);
	for (std::size_t i = 0; i < bgra.size(); i += 4)
	{
		const auto source = cell.data() + std::size_t(header) * width * 4 + i;
		bgra[i] = std::to_integer<std::uint8_t>(source[2]);
		bgra[i + 1] = std::to_integer<std::uint8_t>(source[1]);
		bgra[i + 2] = std::to_integer<std::uint8_t>(source[0]);
		bgra[i + 3] = std::to_integer<std::uint8_t>(source[3]);
	}
	++m_observations;
	if (m_segmentSamples++)
		++m_temporalPairs;
	flicker::IncidentUpdate update;
	std::string reason;
	if (m_ui)
	{
		flicker::UiFrameInfo info;
		for (std::size_t eye = 0; eye < 2; ++eye)
			std::copy(rectangles[eye].begin(), rectangles[eye].end(), info.rects[eye]);
		update = m_uiMonitor->Observe(bgra.data(), width, height, width * 4, m_observations, frame, info);
		reason = m_uiMonitor->Snapshot().incidents.lastReason;
	}
	else
	{
		auto sample = flicker::Downsample(bgra.data(), width, height, width * 4, frame);
		update = m_preview->Preview(std::move(sample), m_observations);
		reason = m_preview->Snapshot().incidents.lastReason;
	}
	if (update.anomaly)
		m_events.push_back({{"frame_id", std::to_string(frame)}, {"reason", std::move(reason)}});
}

nlohmann::json IncidentAnalysis::Report() const
{
	return {{"mode", "on_demand_window"}, {"frames_analyzed", m_observations}, {"frames_skipped", m_skipped},
		{"temporal_pairs", m_temporalPairs}, {"frame_gaps", m_gaps}, {"skipped_frames", m_skippedFrames},
		{"events", m_events}, {"detected", !m_events.empty()},
		{"verdict", !m_events.empty() ? "POSSIBLE_FLICKER" : m_temporalPairs ? "NO_ANOMALY_OBSERVED" : "INCONCLUSIVE"},
		{"limitations", m_ui ? "Pixel heuristics on cropped RGB8 thumbnails; frames missing either eye are skipped. No composition or continuous-monitor status is measured."
			: "Pixel heuristics on reduced RGB8 thumbnails; no continuous-monitor or layer/paint status is measured."}};
}
}
