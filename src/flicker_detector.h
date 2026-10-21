#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

namespace detail
{
class PreviewMonitor;
class UiMonitor;
class ArtifactQueue;
class RenderDocCapture;
} // namespace detail

// Observations borrow CPU pixels only for the duration of the call. The worker owns
// sampled pixels, writes diagnostic artifacts and drains accepted work on Close.
class Diagnostics
{
  public:
	explicit Diagnostics(std::filesystem::path directory);
	~Diagnostics();
	Diagnostics(const Diagnostics&) = delete;
	Diagnostics& operator=(const Diagnostics&) = delete;

	void ObserveSubmission(uint64_t frame, uint32_t projectionLayers, uint32_t totalLayers) noexcept;
	void ObservePreview(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes, uint64_t generation,
						uint64_t frame) noexcept;
	void ObservePaint(uint64_t generation, bool paintedPreview) noexcept;
	void ObserveUi(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes, uint64_t generation,
				   uint64_t frame, const UiFrameInfo& info) noexcept;
	DiagnosticsStatus Status() const;
	bool NeedsPixels() const noexcept;
	void Close();

  private:
	enum class ErrorKind
	{
		File,
		Observation
	};
	template <typename Observation> void ApplyObservation(Observation&& observation) noexcept;
	DiagnosticsStatus Snapshot() const;
	void Run() noexcept;
	void PollRequests();
	void TriggerCapture(uint64_t frame);
	void FlushStatus(const DiagnosticsStatus& status);
	void RecordError(const char* message, ErrorKind kind = ErrorKind::File) noexcept;

	const std::filesystem::path m_directory;
	const std::string m_runName;
	mutable std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::condition_variable m_wake;
	std::unique_ptr<detail::PreviewMonitor> m_preview;
	std::unique_ptr<detail::UiMonitor> m_ui;
	std::unique_ptr<detail::ArtifactQueue> m_artifacts;
	std::unique_ptr<detail::RenderDocCapture> m_renderDoc;
	bool m_closed = false;
	uint64_t m_fileErrors = 0;
	uint64_t m_observationErrors = 0;
	uint64_t m_renderDocTriggerAttempts = 0;
	uint64_t m_renderDocTriggerSuccesses = 0;
	uint64_t m_lastRenderDocTriggerFrame = 0;
	std::string m_lastError;
	std::thread m_worker;
};
} // namespace flicker
