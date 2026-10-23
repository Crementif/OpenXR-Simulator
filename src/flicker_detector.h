#pragma once

#include "diagnostics/FlickerArtifacts.h"
#include "diagnostics/FlickerMonitors.h"
#include "diagnostics/platform/RenderDocCapture.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace flicker
{
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
	PreviewMonitor m_preview;
	UiMonitor m_ui;
	ArtifactQueue m_artifacts;
	RenderDocCapture m_renderDoc;
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
