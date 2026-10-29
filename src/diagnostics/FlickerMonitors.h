#pragma once

#include "diagnostics/FlickerSample.h"

#include <deque>
#include <optional>
#include <string_view>

namespace flicker
{
struct IncidentPacket
{
	std::filesystem::path directory;
	uint64_t triggerFrame = 0;
	std::vector<Sample> history;
};

struct SampleArtifact
{
	std::filesystem::path directory;
	Sample sample;
};

struct IncidentUpdate
{
	std::optional<IncidentPacket> incident;
	std::optional<SampleArtifact> sample;
	bool anomaly = false;
};

class IncidentCapture
{
public:
	explicit IncidentCapture(std::filesystem::path sessionDirectory);
	IncidentUpdate Begin(std::string_view reason, uint64_t frame, const std::deque<Sample>& history);
	std::optional<SampleArtifact> Continue(const Sample& sample);
	const IncidentStatistics& Statistics() const;

private:
	void Expire(uint64_t frame);

	const std::filesystem::path m_sessionDirectory;
	IncidentStatistics m_statistics;
	std::filesystem::path m_activeDirectory;
	uint64_t m_lastIncidentFrame = 0;
	std::optional<uint64_t> m_lastProcessedFrame;
	uint32_t m_postFramesRemaining = 0;
};

class PreviewMonitor
{
public:
	explicit PreviewMonitor(std::filesystem::path sessionDirectory);
	IncidentUpdate Submission(uint64_t frame, uint32_t projectionLayers, uint32_t totalLayers);
	IncidentUpdate Preview(Sample sample, uint64_t generation);
	IncidentUpdate Paint(uint64_t generation, bool paintedPreview);
	PreviewStatistics Snapshot() const;
	bool AcceptsGeneration(uint64_t generation) const;
	uint64_t Frame() const;
	bool TakeDirty();

private:
	PreviewStatistics m_statistics;
	IncidentCapture m_capture;
	std::deque<Sample> m_history;
	uint64_t m_lastGeneration = 0;
	uint64_t m_lastPaintGeneration = 0;
	bool m_projectionInitialized = false;
	bool m_lastHadProjection = false;
	bool m_dirty = false;
};

class UiMonitor
{
public:
	explicit UiMonitor(std::filesystem::path sessionDirectory);
	IncidentUpdate Observe(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t pitchBytes,
						   uint64_t generation, uint64_t frame, const UiFrameInfo& info,
						   PixelOrder order = PixelOrder::Bgra);
	UiStatistics Snapshot() const;
	bool TakeDirty();

private:
	UiStatistics m_statistics;
	IncidentCapture m_capture;
	std::deque<Sample> m_history;
	uint64_t m_lastGeneration = 0;
	bool m_dirty = false;
};
} // namespace flicker
