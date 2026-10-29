#pragma once

#include "diagnostics/FlickerMonitors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace flicker
{
struct AnalysisObservation
{
	bool skipped = false;
	std::string reason;
};

struct AnalysisCounts
{
	std::uint64_t observations = 0;
	std::uint64_t skipped = 0;
	std::uint64_t temporalPairs = 0;
	std::uint64_t gaps = 0;
};

class IncidentAnalysis
{
public:
	explicit IncidentAnalysis(bool ui);
	AnalysisObservation Observe(std::uint64_t frame, std::span<const std::byte> pixels, std::uint32_t width,
								std::uint32_t height, std::uint32_t stride, PixelOrder order,
								const std::array<std::array<std::int32_t, 4>, 2>& rectangles = {});
	void Skip(std::uint64_t frame);

	const AnalysisCounts& Counts() const
	{
		return m_counts;
	}

private:
	void Track(std::uint64_t frame);
	void Reset();
	bool m_ui;
	AnalysisCounts m_counts;
	std::uint64_t m_segmentSamples = 0;
	std::optional<std::uint64_t> m_lastFrame;
	std::optional<PreviewMonitor> m_preview;
	std::optional<UiMonitor> m_uiMonitor;
};
}
