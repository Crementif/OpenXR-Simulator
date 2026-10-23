#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace flicker
{
struct UiFrameInfo
{
	uint32_t quadLayers = 0;
	bool projectionRefreshed = false;
	bool freshReadback = false;
	bool cachedPixelsUsed = false;
	bool cacheValid = false;
	bool composed = false;
	int32_t rects[2][4] = {};
	float sourceAlphaCoverage = 0.0f;
};

struct SampleMetrics
{
	float mean = 0.0f;
	float meanLeft = 0.0f;
	float meanRight = 0.0f;
	float temporal = 0.0f;
	float temporalLeft = 0.0f;
	float temporalRight = 0.0f;
	float brightFractionLeft = 0.0f;
	float brightFractionRight = 0.0f;
};

struct IncidentStatistics
{
	uint64_t anomalyCount = 0;
	uint64_t incidentCount = 0;
	std::string lastReason = "NONE";
	std::filesystem::path lastDirectory;
};

struct PreviewStatistics
{
	uint64_t lastFrame = 0;
	uint64_t totalSubmissions = 0;
	uint64_t projectionSubmissions = 0;
	uint64_t missingProjectionSubmissions = 0;
	uint64_t projectionPresenceTransitions = 0;
	uint64_t previewSamples = 0;
	uint64_t visiblePreviewSamples = 0;
	uint64_t paintAttempts = 0;
	uint64_t successfulPaints = 0;
	uint64_t failedPaints = 0;
	uint64_t duplicateGenerationPaints = 0;
	uint64_t asymmetricBrightSamples = 0;
	uint32_t consecutiveMissingProjection = 0;
	uint32_t maxConsecutiveMissingProjection = 0;
	uint32_t consecutiveAsymmetricBright = 0;
	uint32_t maxConsecutiveAsymmetricBright = 0;
	uint32_t lastLayerCount = 0;
	bool hasMetrics = false;
	SampleMetrics metrics;
	IncidentStatistics incidents;
};

struct UiStatistics
{
	uint64_t lastFrame = 0;
	uint64_t observedFrames = 0;
	uint64_t quadSubmittedFrames = 0;
	uint64_t projectionRefreshFrames = 0;
	uint64_t freshReadbacks = 0;
	uint64_t freshCompositions = 0;
	uint64_t cachedCompositions = 0;
	uint64_t missingAfterProjection = 0;
	uint64_t compositionFailures = 0;
	uint64_t uiSamples = 0;
	uint32_t consecutiveMissingAfterProjection = 0;
	uint32_t maxConsecutiveMissingAfterProjection = 0;
	uint32_t lastQuadLayers = 0;
	bool lastCacheValid = false;
	bool lastComposed = false;
	bool hasMetrics = false;
	float lastSourceAlphaCoverage = 0.0f;
	SampleMetrics metrics;
	IncidentStatistics incidents;
};

struct DiagnosticsStatus
{
	bool closed = false;
	uint64_t fileErrors = 0;
	uint64_t observationErrors = 0;
	uint64_t droppedArtifacts = 0;
	uint64_t renderDocTriggerAttempts = 0;
	uint64_t renderDocTriggerSuccesses = 0;
	uint64_t lastRenderDocTriggerFrame = 0;
	size_t outstandingArtifacts = 0;
	size_t outstandingBytes = 0;
	std::string lastError;
	PreviewStatistics preview;
	UiStatistics ui;
};

} // namespace flicker
