#pragma once

#include "diagnostics/FlickerMonitors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
class IncidentAnalysis
{
  public:
	explicit IncidentAnalysis(bool ui);
	void Observe(std::uint64_t frame, std::span<const std::byte> cell,
		const std::array<std::array<std::int32_t, 4>, 2>& rectangles);
	void Skip(std::uint64_t frame);
	nlohmann::json Report() const;

  private:
	void Track(std::uint64_t frame);
	void Reset();
	bool m_ui;
	std::uint64_t m_observations = 0, m_skipped = 0, m_temporalPairs = 0, m_segmentSamples = 0, m_gaps = 0;
	std::optional<std::uint64_t> m_lastFrame;
	std::optional<flicker::PreviewMonitor> m_preview;
	std::optional<flicker::UiMonitor> m_uiMonitor;
	nlohmann::json m_events = nlohmann::json::array();
	nlohmann::json m_skippedFrames = nlohmann::json::array();
};
}
