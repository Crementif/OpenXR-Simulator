#include "flicker_detector.h"

#include "diagnostics/FlickerArtifacts.h"
#include "diagnostics/FlickerMonitors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace flicker
{
namespace
{
std::string RunName()
{
	static std::atomic<uint64_t> nextRun{0};
	const auto started = std::chrono::steady_clock::now().time_since_epoch().count();
	return "session_" + std::to_string(detail::ProcessId()) + "_" + std::to_string(started) + "_" +
		   std::to_string(nextRun.fetch_add(1, std::memory_order_relaxed));
}
} // namespace

Diagnostics::Diagnostics(std::filesystem::path directory)
	: m_directory(std::move(directory)), m_runName(RunName()),
	  m_preview(std::make_unique<detail::PreviewMonitor>(m_directory / "flicker_incidents" / m_runName)),
	  m_ui(std::make_unique<detail::UiMonitor>(m_directory / "ui_flicker_incidents" / m_runName)),
	  m_artifacts(std::make_unique<detail::ArtifactQueue>()), m_renderDoc(std::make_unique<detail::RenderDocCapture>()),
	  m_worker(&Diagnostics::Run, this)
{
}

Diagnostics::~Diagnostics()
{
	Close();
}

template <typename Observation> void Diagnostics::ApplyObservation(Observation&& observation) noexcept
{
	try
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			return;
		observation();
		m_wake.notify_one();
	}
	catch (const std::exception& error)
	{
		RecordError(error.what(), ErrorKind::Observation);
	}
	catch (...)
	{
		RecordError("Unexpected diagnostic observation failure", ErrorKind::Observation);
	}
}

void Diagnostics::ObserveSubmission(uint64_t frame, uint32_t projectionLayers, uint32_t totalLayers) noexcept
{
	ApplyObservation(
		[&]
		{
			auto update = m_preview->Submission(frame, projectionLayers, totalLayers);
			m_artifacts->Add(std::move(update), *m_preview);
		});
}

void Diagnostics::ObservePreview(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes,
								 uint64_t generation, uint64_t frame) noexcept
{
	if (!detail::ValidPixels(bgra, width, height, pitchBytes) || generation == 0)
		return;
	ApplyObservation(
		[&]
		{
			if (!m_preview->AcceptsGeneration(generation))
				return;
			auto sample = detail::Downsample(bgra, width, height, pitchBytes ? pitchBytes : width * 4, frame);
			auto update = m_preview->Preview(std::move(sample), generation);
			m_artifacts->Add(std::move(update), *m_preview);
		});
}

void Diagnostics::ObservePaint(uint64_t generation, bool paintedPreview) noexcept
{
	ApplyObservation(
		[&]
		{
			auto update = m_preview->Paint(generation, paintedPreview);
			m_artifacts->Add(std::move(update), *m_preview);
		});
}

void Diagnostics::ObserveUi(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes,
							uint64_t generation, uint64_t frame, const UiFrameInfo& info) noexcept
{
	ApplyObservation(
		[&]
		{
			auto update = m_ui->Observe(bgra, width, height, pitchBytes, generation, frame, info);
			m_artifacts->Add(std::move(update), *m_ui);
		});
}

DiagnosticsStatus Diagnostics::Snapshot() const
{
	DiagnosticsStatus status;
	status.closed = m_closed;
	status.fileErrors = m_fileErrors;
	status.observationErrors = m_observationErrors;
	status.lastError = m_lastError;
	status.outstandingArtifacts = m_artifacts->Outstanding();
	status.outstandingBytes = m_artifacts->OutstandingBytes();
	status.droppedArtifacts = m_artifacts->Dropped();
	status.renderDocTriggerAttempts = m_renderDocTriggerAttempts;
	status.renderDocTriggerSuccesses = m_renderDocTriggerSuccesses;
	status.lastRenderDocTriggerFrame = m_lastRenderDocTriggerFrame;
	status.preview = m_preview->Snapshot();
	status.ui = m_ui->Snapshot();
	return status;
}

DiagnosticsStatus Diagnostics::Status() const
{
	std::lock_guard lock(m_mutex);
	return Snapshot();
}

bool Diagnostics::NeedsPixels() const noexcept
{
	std::lock_guard lock(m_mutex);
	return !m_closed && (m_preview->NeedsPixels() || m_ui->NeedsPixels());
}

void Diagnostics::RecordError(const char* message, ErrorKind kind) noexcept
{
	std::lock_guard lock(m_mutex);
	if (kind == ErrorKind::Observation)
		++m_observationErrors;
	else
		++m_fileErrors;
	try
	{
		m_lastError.assign(message, std::min<size_t>(std::char_traits<char>::length(message), 2048));
	}
	catch (...)
	{
	}
}

void Diagnostics::Run() noexcept
{
	using Clock = std::chrono::steady_clock;
	auto lastStatus = Clock::now() - std::chrono::milliseconds(250);
	auto lastPoll = Clock::now() - std::chrono::milliseconds(500);
	bool pruned = false;
	for (;;)
	{
		std::optional<detail::ArtifactJob> job;
		std::optional<DiagnosticsStatus> status;
		bool finished = false;
		bool requestRenderDoc = false;
		uint64_t renderDocFrame = 0;
		try
		{
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait_for(lock, std::chrono::milliseconds(250),
								[this]
								{
									return m_closed || !m_artifacts->Empty();
								});
				finished = m_closed && m_artifacts->Empty();
				if (finished || Clock::now() - lastStatus >= std::chrono::milliseconds(250))
				{
					const bool previewDirty = m_preview->TakeDirty();
					const bool uiDirty = m_ui->TakeDirty();
					if (previewDirty || uiDirty || finished)
						status = Snapshot();
					lastStatus = Clock::now();
				}
				requestRenderDoc = m_artifacts->TakeRenderDocRequest();
				renderDocFrame = m_preview->Frame();
				job = m_artifacts->Take();
			}
			if (requestRenderDoc)
			{
				TriggerCapture(renderDocFrame);
				if (status)
				{
					std::lock_guard lock(m_mutex);
					status = Snapshot();
				}
			}
			if (status)
				FlushStatus(*status);
			if (job)
				detail::WriteArtifact(job->artifact);
		}
		catch (const std::exception& error)
		{
			RecordError(error.what());
		}
		catch (...)
		{
			RecordError("Unexpected diagnostic worker failure");
		}
		if (job)
		{
			const auto charge = job->charge;
			job.reset();
			std::lock_guard lock(m_mutex);
			m_artifacts->Complete(charge);
		}
		if (finished)
			return;

		try
		{
			bool closing = false;
			{
				std::lock_guard lock(m_mutex);
				closing = m_closed;
			}
			if (closing)
				continue;
			if (Clock::now() - lastPoll >= std::chrono::milliseconds(500))
			{
				lastPoll = Clock::now();
				PollRequests();
			}
			if (!pruned)
			{
				pruned = true;
				detail::PruneArtifacts(m_directory, m_runName);
			}
		}
		catch (const std::exception& error)
		{
			RecordError(error.what());
		}
		catch (...)
		{
			RecordError("Unexpected diagnostic request failure");
		}
	}
}

void Diagnostics::TriggerCapture(uint64_t frame)
{
	const auto result = m_renderDoc->Trigger(frame);
	std::lock_guard lock(m_mutex);
	m_renderDocTriggerAttempts += result.attempted;
	m_renderDocTriggerSuccesses += result.succeeded;
	if (result.succeeded)
		m_lastRenderDocTriggerFrame = frame;
}

void Diagnostics::FlushStatus(const DiagnosticsStatus& status)
{
	try
	{
		detail::WritePreviewStatus(m_directory, status);
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
	}
	try
	{
		detail::WriteUiStatus(m_directory, status);
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
	}
}

void Diagnostics::PollRequests()
{
	const bool previewRequested = detail::ConsumeRequest(m_directory / "flicker_capture_request.json");
	const bool uiRequested = detail::ConsumeRequest(m_directory / "ui_flicker_capture_request.json");
	const bool renderDocRequested = detail::ConsumeRequest(m_directory / "renderdoc_capture_request.json");
	uint64_t frame = 0;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			return;
		if (previewRequested)
		{
			auto update = m_preview->Capture();
			m_artifacts->Add(std::move(update), *m_preview);
		}
		if (uiRequested)
		{
			auto update = m_ui->Capture();
			m_artifacts->Add(std::move(update), *m_ui);
		}
		frame = m_preview->Frame();
	}
	if (renderDocRequested)
		TriggerCapture(frame);
}

void Diagnostics::Close()
{
	std::lock_guard closeLock(m_closeMutex);
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
	}
	m_wake.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}
} // namespace flicker
