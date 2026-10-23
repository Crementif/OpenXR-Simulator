#include "diagnostics/FlickerMonitors.h"
#include "Pch.h"

#include <cmath>

namespace flicker
{
static constexpr size_t s_historyFrames = 10;
static constexpr uint32_t s_postIncidentFrames = 12;
static constexpr uint64_t s_incidentCooldownFrames = 300;
static constexpr uint64_t s_maxIncidentsPerSession = 40;

static void Remember(std::deque<Sample>& history, Sample sample)
{
	history.push_back(std::move(sample));
	while (history.size() > s_historyFrames)
		history.pop_front();
}

IncidentCapture::IncidentCapture(std::filesystem::path sessionDirectory)
	: m_sessionDirectory(std::move(sessionDirectory))
{
}

IncidentUpdate IncidentCapture::Begin(std::string_view reason, uint64_t frame, bool manual,
									  const std::deque<Sample>& history)
{
	if (!manual)
		++m_statistics.anomalyCount;
	m_statistics.lastReason = reason;
	IncidentUpdate update;
	update.anomaly = !manual;
	const bool coolingDown = m_statistics.incidentCount != 0 && frame >= m_lastIncidentFrame &&
							 frame - m_lastIncidentFrame < s_incidentCooldownFrames;
	if (!m_activeDirectory.empty())
	{
		m_manual |= manual;
		return update;
	}
	if ((!manual && coolingDown) || m_statistics.incidentCount >= s_maxIncidentsPerSession)
		return update;

	IncidentPacket packet;
	packet.directory = m_sessionDirectory / ("incident_" + std::to_string(frame));
	packet.triggerFrame = frame;
	packet.history.assign(history.begin(), history.end());
	m_activeDirectory = packet.directory;
	m_statistics.lastDirectory = packet.directory;
	m_lastIncidentFrame = frame;
	++m_statistics.incidentCount;
	m_postFramesRemaining = s_postIncidentFrames;
	m_manual = manual;
	m_lastProcessedFrame.reset();
	if (!history.empty())
		m_lastProcessedFrame = history.back().frame;
	update.incident = std::move(packet);
	return update;
}

std::optional<SampleArtifact> IncidentCapture::Continue(const Sample& sample)
{
	if (m_activeDirectory.empty() || m_lastProcessedFrame == sample.frame)
		return {};
	SampleArtifact artifact{m_activeDirectory, sample};
	m_lastProcessedFrame = sample.frame;
	if (--m_postFramesRemaining == 0)
		m_activeDirectory.clear();
	return artifact;
}

const IncidentStatistics& IncidentCapture::Statistics() const
{
	return m_statistics;
}

void IncidentCapture::SkipSample(uint64_t frame)
{
	if (!NeedsPixels() || m_lastProcessedFrame == frame)
		return;
	m_lastProcessedFrame = frame;
	if (--m_postFramesRemaining == 0)
		m_activeDirectory.clear();
}

bool IncidentCapture::NeedsPixels() const
{
	return m_manual && !m_activeDirectory.empty();
}

PreviewMonitor::PreviewMonitor(std::filesystem::path sessionDirectory) : m_capture(std::move(sessionDirectory)) {}

IncidentUpdate PreviewMonitor::Submission(uint64_t frame, uint32_t projectionLayers, uint32_t totalLayers)
{
	auto& statistics = m_statistics;
	statistics.lastFrame = frame;
	++statistics.totalSubmissions;
	statistics.lastLayerCount = totalLayers;
	const bool hasProjection = projectionLayers > 0;
	if (hasProjection)
	{
		++statistics.projectionSubmissions;
		statistics.consecutiveMissingProjection = 0;
	}
	else
	{
		++statistics.missingProjectionSubmissions;
		++statistics.consecutiveMissingProjection;
		statistics.maxConsecutiveMissingProjection =
			std::max(statistics.maxConsecutiveMissingProjection, statistics.consecutiveMissingProjection);
	}
	IncidentUpdate update;
	if (m_projectionInitialized && m_lastHadProjection != hasProjection)
	{
		++statistics.projectionPresenceTransitions;
		if (!hasProjection && statistics.projectionSubmissions >= 5)
			update = m_capture.Begin("MISSING_PROJECTION_LAYER", frame, false, m_history);
	}
	m_projectionInitialized = true;
	m_lastHadProjection = hasProjection;
	m_dirty |= statistics.totalSubmissions % 15 == 0 || !hasProjection;
	return update;
}

IncidentUpdate PreviewMonitor::Preview(Sample current, uint64_t generation)
{
	if (generation == 0 || generation == m_lastGeneration)
		return {};
	m_lastGeneration = generation;
	auto& statistics = m_statistics;
	if (!m_history.empty())
		CalculateTemporal(current, m_history.back());
	++statistics.previewSamples;

	std::string_view reason;
	const auto& metrics = current.metrics;
	const auto brightMaximum = std::max(metrics.brightFractionLeft, metrics.brightFractionRight);
	const auto brightMinimum = std::min(metrics.brightFractionLeft, metrics.brightFractionRight);
	const bool asymmetricBright =
		brightMaximum >= 0.001f && brightMaximum - brightMinimum >= 0.0009f && brightMinimum <= brightMaximum * 0.35f;
	if (asymmetricBright)
	{
		++statistics.asymmetricBrightSamples;
		++statistics.consecutiveAsymmetricBright;
		statistics.maxConsecutiveAsymmetricBright =
			std::max(statistics.maxConsecutiveAsymmetricBright, statistics.consecutiveAsymmetricBright);
		if (statistics.consecutiveAsymmetricBright == 2)
			reason = "ASYMMETRIC_BRIGHT_BREAKTHROUGH";
	}
	else
		statistics.consecutiveAsymmetricBright = 0;

	if (!m_history.empty())
	{
		const auto previousMean = m_history.back().metrics.mean;
		const bool blankNow = metrics.mean < 0.015f || metrics.mean > 0.985f;
		const bool blankBefore = previousMean < 0.015f || previousMean > 0.985f;
		const auto eyeAsymmetry = std::abs(metrics.temporalLeft - metrics.temporalRight);
		if (reason.empty() && blankNow && !blankBefore && statistics.visiblePreviewSamples >= 5)
			reason = "VISIBLE_TO_BLANK_FRAME";
		else if (reason.empty() && std::max(metrics.temporalLeft, metrics.temporalRight) > 0.12f &&
				 eyeAsymmetry > 0.08f)
			reason = "ASYMMETRIC_EYE_FLASH";
		else if (reason.empty() && m_history.size() >= 2 && metrics.temporal > 0.08f &&
				 TemporalBetween(current, m_history[m_history.size() - 2]) < 0.03f)
			reason = "ALTERNATING_VISIBLE_FRAMES";
	}
	if (metrics.mean >= 0.015f && metrics.mean <= 0.985f)
		++statistics.visiblePreviewSamples;
	IncidentUpdate update;
	if (!reason.empty())
		update = m_capture.Begin(reason, current.frame, false, m_history);
	update.sample = m_capture.Continue(current);
	m_dirty |= statistics.previewSamples % 15 == 0 || !reason.empty();
	statistics.metrics = metrics;
	statistics.hasMetrics = true;
	Remember(m_history, std::move(current));
	return update;
}

IncidentUpdate PreviewMonitor::Paint(uint64_t generation, bool paintedPreview)
{
	auto& statistics = m_statistics;
	++statistics.paintAttempts;
	IncidentUpdate update;
	if (paintedPreview)
	{
		++statistics.successfulPaints;
		if (generation != 0 && generation == m_lastPaintGeneration)
			++statistics.duplicateGenerationPaints;
		m_lastPaintGeneration = generation;
	}
	else
	{
		++statistics.failedPaints;
		if (statistics.successfulPaints >= 5)
			update = m_capture.Begin("PREVIEW_PAINT_FAILURE", statistics.lastFrame, false, m_history);
	}
	m_dirty |= !paintedPreview || statistics.paintAttempts % 15 == 0;
	return update;
}

IncidentUpdate PreviewMonitor::Capture()
{
	m_dirty = true;
	return m_capture.Begin("MANUAL_CAPTURE", m_statistics.lastFrame, true, m_history);
}

bool PreviewMonitor::AcceptsGeneration(uint64_t generation) const
{
	return generation != 0 && generation != m_lastGeneration;
}

bool PreviewMonitor::NeedsPixels() const
{
	return m_capture.NeedsPixels();
}

uint64_t PreviewMonitor::Frame() const
{
	return m_statistics.lastFrame;
}

PreviewStatistics PreviewMonitor::Snapshot() const
{
	auto snapshot = m_statistics;
	snapshot.incidents = m_capture.Statistics();
	return snapshot;
}

bool PreviewMonitor::TakeDirty()
{
	return std::exchange(m_dirty, false);
}

UiMonitor::UiMonitor(std::filesystem::path sessionDirectory) : m_capture(std::move(sessionDirectory)) {}

IncidentUpdate UiMonitor::Observe(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes,
								  uint64_t generation, uint64_t frame, const UiFrameInfo& info)
{
	auto& statistics = m_statistics;
	statistics.lastFrame = frame;
	++statistics.observedFrames;
	statistics.lastQuadLayers = info.quadLayers;
	statistics.lastCacheValid = info.cacheValid;
	statistics.lastComposed = info.composed;
	statistics.lastSourceAlphaCoverage = std::isfinite(info.sourceAlphaCoverage) ? info.sourceAlphaCoverage : 0.0f;
	if (info.quadLayers > 0)
		++statistics.quadSubmittedFrames;
	if (info.projectionRefreshed && info.quadLayers > 0)
		++statistics.projectionRefreshFrames;
	if (info.freshReadback)
		++statistics.freshReadbacks;
	if (info.composed && info.freshReadback)
		++statistics.freshCompositions;
	if (info.composed && info.cachedPixelsUsed)
		++statistics.cachedCompositions;

	std::string_view reason;
	if (info.quadLayers > 0 && info.projectionRefreshed && !info.composed)
	{
		++statistics.missingAfterProjection;
		++statistics.consecutiveMissingAfterProjection;
		statistics.maxConsecutiveMissingAfterProjection =
			std::max(statistics.maxConsecutiveMissingAfterProjection, statistics.consecutiveMissingAfterProjection);
		reason = info.cacheValid ? "UI_NOT_RECOMPOSED_AFTER_PROJECTION" : "UI_CACHE_UNAVAILABLE_AFTER_PROJECTION";
	}
	else
		statistics.consecutiveMissingAfterProjection = 0;
	if (info.quadLayers > 0 && info.projectionRefreshed && info.cacheValid && !info.composed && info.freshReadback)
	{
		++statistics.compositionFailures;
		reason = "UI_COMPOSITION_FAILED";
	}

	std::optional<Sample> current;
	const bool validRects =
		info.rects[0][2] > 0 && info.rects[0][3] > 0 && info.rects[1][2] > 0 && info.rects[1][3] > 0;
	if (ValidPixels(bgra, width, height, pitchBytes) && validRects && generation != 0 && generation != m_lastGeneration)
	{
		m_lastGeneration = generation;
		current = CropUi(bgra, width, height, pitchBytes ? pitchBytes : width * 4, info.rects, frame);
		if (!m_history.empty())
			CalculateTemporal(*current, m_history.back());
		++statistics.uiSamples;
		if (reason.empty() && !m_history.empty())
		{
			const auto& metrics = current->metrics;
			const auto asymmetry = std::abs(metrics.temporalLeft - metrics.temporalRight);
			if (m_history.size() >= 2 && metrics.temporal > 0.055f &&
				TemporalBetween(*current, m_history[m_history.size() - 2]) < 0.025f)
				reason = "ALTERNATING_UI_REGION";
			else if (std::max(metrics.temporalLeft, metrics.temporalRight) > 0.12f && asymmetry > 0.08f)
				reason = "ASYMMETRIC_UI_EYE_FLASH";
		}
	}

	IncidentUpdate update;
	if (!reason.empty())
		update = m_capture.Begin(reason, frame, false, m_history);
	if (current)
	{
		update.sample = m_capture.Continue(*current);
		statistics.metrics = current->metrics;
		statistics.hasMetrics = true;
		Remember(m_history, std::move(*current));
	}
	else if (ValidPixels(bgra, width, height, pitchBytes) && generation != 0 && generation != m_lastGeneration)
	{
		m_lastGeneration = generation;
		m_capture.SkipSample(frame);
	}
	m_dirty |= statistics.observedFrames % 15 == 0 || !reason.empty();
	return update;
}

IncidentUpdate UiMonitor::Capture()
{
	m_dirty = true;
	return m_capture.Begin("MANUAL_UI_CAPTURE", m_statistics.lastFrame, true, m_history);
}

UiStatistics UiMonitor::Snapshot() const
{
	auto snapshot = m_statistics;
	snapshot.incidents = m_capture.Statistics();
	return snapshot;
}

bool UiMonitor::NeedsPixels() const
{
	return m_capture.NeedsPixels();
}

bool UiMonitor::TakeDirty()
{
	return std::exchange(m_dirty, false);
}
} // namespace flicker
