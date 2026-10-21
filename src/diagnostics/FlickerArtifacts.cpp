#include "diagnostics/FlickerArtifacts.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace flicker::detail
{
namespace
{
constexpr size_t MaxArtifactJobs = 128;
constexpr size_t MaxArtifactSamples = 64;
constexpr size_t MaxArtifactBytes = 16 * 1024 * 1024;
constexpr size_t KeepRecentSessions = 5;
constexpr size_t KeepRecentLogs = 10;

size_t PathBytes(const std::filesystem::path& path)
{
	return path.native().size() * sizeof(std::filesystem::path::value_type);
}

ArtifactCharge Charge(const Artifact& artifact)
{
	return std::visit(
		[](const auto& value) -> ArtifactCharge
		{
			using Value = std::decay_t<decltype(value)>;
			ArtifactCharge charge;
			if constexpr (std::is_same_v<Value, SampleArtifact>)
			{
				charge.bytes = PathBytes(value.directory) + value.sample.pixels->capacity();
				charge.samples = 1;
			}
			else
			{
				charge.bytes = PathBytes(value.packet.directory) + PathBytes(value.statistics.incidents.lastDirectory) +
							   value.statistics.incidents.lastReason.capacity();
				for (const auto& sample : value.packet.history)
					charge.bytes += sample.pixels->capacity();
				charge.samples = value.packet.history.size();
			}
			return charge;
		},
		artifact);
}

uint64_t UnixTimeMs()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count());
}

std::string JsonEscape(std::string_view value)
{
	std::ostringstream escaped;
	for (const unsigned char character : value)
	{
		switch (character)
		{
		case '\\':
			escaped << "\\\\";
			break;
		case '"':
			escaped << "\\\"";
			break;
		case '\n':
			escaped << "\\n";
			break;
		case '\r':
			escaped << "\\r";
			break;
		case '\t':
			escaped << "\\t";
			break;
		default:
			if (character < 32)
				escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character);
			else
				escaped << character;
		}
	}
	return escaped.str();
}

std::string Utf8Path(const std::filesystem::path& path)
{
	const auto utf8 = path.u8string();
	return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

class AtomicFile
{
  public:
	explicit AtomicFile(const std::filesystem::path& path) : m_path(path), m_temporary(path)
	{
		m_temporary += ".tmp." + std::to_string(ProcessId());
		std::filesystem::create_directories(path.parent_path());
		m_output.exceptions(std::ios::badbit | std::ios::failbit);
		try
		{
			m_output.open(m_temporary, std::ios::binary | std::ios::trunc);
		}
		catch (...)
		{
			std::error_code ignored;
			std::filesystem::remove(m_temporary, ignored);
			throw;
		}
	}
	~AtomicFile()
	{
		if (!m_committed)
		{
			m_output.exceptions(std::ios::goodbit);
			if (m_output.is_open())
				m_output.close();
			std::error_code ignored;
			std::filesystem::remove(m_temporary, ignored);
		}
	}
	void Write(const void* bytes, size_t size)
	{
		m_output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
	}
	void Commit()
	{
		m_output.close();
#ifdef _WIN32
		if (!MoveFileExW(m_temporary.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
									"Publish diagnostic artifact");
#else
		std::filesystem::rename(m_temporary, m_path);
#endif
		m_committed = true;
	}

  private:
	const std::filesystem::path m_path;
	std::filesystem::path m_temporary;
	std::ofstream m_output;
	bool m_committed = false;
};

void WriteText(const std::filesystem::path& path, const std::string& contents)
{
	AtomicFile file(path);
	file.Write(contents.data(), contents.size());
	file.Commit();
}

#pragma pack(push, 1)
struct BmpHeader
{
	uint16_t magic = 0x4D42;
	uint32_t fileSize = 0;
	uint16_t reserved1 = 0;
	uint16_t reserved2 = 0;
	uint32_t pixelOffset = 54;
	uint32_t infoSize = 40;
	int32_t width = 0;
	int32_t height = 0;
	uint16_t planes = 1;
	uint16_t bitsPerPixel = 24;
	uint32_t compression = 0;
	uint32_t imageSize = 0;
	int32_t ppmX = 2835;
	int32_t ppmY = 2835;
	uint32_t colors = 0;
	uint32_t importantColors = 0;
};
#pragma pack(pop)
static_assert(sizeof(BmpHeader) == 54);

void WriteBmpRegion(const std::filesystem::path& path, const Sample& sample, uint32_t startX, uint32_t regionWidth)
{
	const auto rowSize = (regionWidth * 3 + 3) & ~3u;
	BmpHeader header;
	header.width = static_cast<int32_t>(regionWidth);
	header.height = static_cast<int32_t>(sample.height);
	header.imageSize = rowSize * sample.height;
	header.fileSize = header.pixelOffset + header.imageSize;
	AtomicFile file(path);
	file.Write(&header, sizeof(header));
	std::vector<uint8_t> row(rowSize, 0);
	for (uint32_t y = sample.height; y-- > 0;)
	{
		for (uint32_t x = 0; x < regionWidth; ++x)
		{
			const auto* pixel = sample.pixels->data() + (static_cast<size_t>(y) * sample.width + startX + x) * 4;
			std::copy_n(pixel, 3, row.data() + x * 3);
		}
		file.Write(row.data(), row.size());
	}
	file.Commit();
}

void SaveSample(const SampleArtifact& artifact)
{
	const auto& sample = artifact.sample;
	const auto prefix = "frame" + std::to_string(sample.frame);
	const auto leftWidth = sample.width / 2;
	WriteBmpRegion(artifact.directory / (prefix + "_color_L.bmp"), sample, 0, leftWidth);
	WriteBmpRegion(artifact.directory / (prefix + "_color_R.bmp"), sample, leftWidth, sample.width - leftWidth);
	WriteBmpRegion(artifact.directory / (prefix + "_preview.bmp"), sample, 0, sample.width);
}

std::string BuildStatusJson(const PreviewStatistics& state, const DiagnosticsStatus* diagnostics,
							std::optional<uint64_t> triggerFrame = {})
{
	std::ostringstream json;
	json.imbue(std::locale::classic());
	json << std::fixed << std::setprecision(6) << "{\n  \"schemaVersion\": 1,\n  \"timestampUnixMs\": " << UnixTimeMs()
		 << ",\n  \"pid\": " << ProcessId() << ",\n  \"frame\": " << state.lastFrame
		 << ",\n  \"totalSubmissions\": " << state.totalSubmissions
		 << ",\n  \"projectionSubmissions\": " << state.projectionSubmissions
		 << ",\n  \"missingProjectionSubmissions\": " << state.missingProjectionSubmissions
		 << ",\n  \"projectionPresenceTransitions\": " << state.projectionPresenceTransitions
		 << ",\n  \"consecutiveMissingProjection\": " << state.consecutiveMissingProjection
		 << ",\n  \"maxConsecutiveMissingProjection\": " << state.maxConsecutiveMissingProjection
		 << ",\n  \"lastLayerCount\": " << state.lastLayerCount << ",\n  \"previewSamples\": " << state.previewSamples
		 << ",\n  \"visiblePreviewSamples\": " << state.visiblePreviewSamples
		 << ",\n  \"paintAttempts\": " << state.paintAttempts << ",\n  \"successfulPaints\": " << state.successfulPaints
		 << ",\n  \"failedPaints\": " << state.failedPaints
		 << ",\n  \"duplicateGenerationPaints\": " << state.duplicateGenerationPaints
		 << ",\n  \"asymmetricBrightSamples\": " << state.asymmetricBrightSamples
		 << ",\n  \"consecutiveAsymmetricBright\": " << state.consecutiveAsymmetricBright
		 << ",\n  \"maxConsecutiveAsymmetricBright\": " << state.maxConsecutiveAsymmetricBright
		 << ",\n  \"renderDocTriggerAttempts\": " << (diagnostics ? diagnostics->renderDocTriggerAttempts : 0)
		 << ",\n  \"renderDocTriggerSuccesses\": " << (diagnostics ? diagnostics->renderDocTriggerSuccesses : 0)
		 << ",\n  \"lastRenderDocTriggerFrame\": " << (diagnostics ? diagnostics->lastRenderDocTriggerFrame : 0)
		 << ",\n  \"anomalyCount\": " << state.incidents.anomalyCount
		 << ",\n  \"incidentCount\": " << state.incidents.incidentCount << ",\n  \"lastReason\": \""
		 << JsonEscape(state.incidents.lastReason) << "\""
		 << ",\n  \"lastIncidentDirectory\": \"" << JsonEscape(Utf8Path(state.incidents.lastDirectory)) << "\"";
	if (state.hasMetrics)
	{
		json << ",\n  \"preview\": { \"mean\": " << state.metrics.mean << ", \"meanLeft\": " << state.metrics.meanLeft
			 << ", \"meanRight\": " << state.metrics.meanRight << ", \"temporal\": " << state.metrics.temporal
			 << ", \"temporalLeft\": " << state.metrics.temporalLeft
			 << ", \"temporalRight\": " << state.metrics.temporalRight
			 << ", \"brightFractionLeft\": " << state.metrics.brightFractionLeft
			 << ", \"brightFractionRight\": " << state.metrics.brightFractionRight << " }";
	}
	if (triggerFrame)
	{
		json << ",\n  \"triggerFrame\": " << *triggerFrame << ",\n  \"reason\": \""
			 << JsonEscape(state.incidents.lastReason) << "\"";
	}
	if (diagnostics)
	{
		json << ",\n  \"closed\": " << (diagnostics->closed ? "true" : "false")
			 << ",\n  \"fileErrors\": " << diagnostics->fileErrors
			 << ",\n  \"observationErrors\": " << diagnostics->observationErrors
			 << ",\n  \"droppedArtifacts\": " << diagnostics->droppedArtifacts
			 << ",\n  \"outstandingArtifacts\": " << diagnostics->outstandingArtifacts
			 << ",\n  \"outstandingBytes\": " << diagnostics->outstandingBytes << ",\n  \"lastError\": \""
			 << JsonEscape(diagnostics->lastError) << "\"";
	}
	json << "\n}\n";
	return json.str();
}

std::string BuildUiStatusJson(const UiStatistics& state, const DiagnosticsStatus* diagnostics,
							  std::optional<uint64_t> triggerFrame = {})
{
	std::ostringstream json;
	json.imbue(std::locale::classic());
	json << std::fixed << std::setprecision(6)
		 << "{\n  \"schemaVersion\": 1,\n  \"captureSource\": \"openxr-simulator-ui-quad\""
		 << ",\n  \"timestampUnixMs\": " << UnixTimeMs() << ",\n  \"pid\": " << ProcessId()
		 << ",\n  \"frame\": " << state.lastFrame << ",\n  \"observedFrames\": " << state.observedFrames
		 << ",\n  \"quadSubmittedFrames\": " << state.quadSubmittedFrames
		 << ",\n  \"projectionRefreshFrames\": " << state.projectionRefreshFrames
		 << ",\n  \"freshReadbacks\": " << state.freshReadbacks
		 << ",\n  \"freshCompositions\": " << state.freshCompositions
		 << ",\n  \"cachedCompositions\": " << state.cachedCompositions
		 << ",\n  \"missingAfterProjection\": " << state.missingAfterProjection
		 << ",\n  \"compositionFailures\": " << state.compositionFailures
		 << ",\n  \"consecutiveMissingAfterProjection\": " << state.consecutiveMissingAfterProjection
		 << ",\n  \"maxConsecutiveMissingAfterProjection\": " << state.maxConsecutiveMissingAfterProjection
		 << ",\n  \"uiSamples\": " << state.uiSamples << ",\n  \"lastQuadLayers\": " << state.lastQuadLayers
		 << ",\n  \"cacheValid\": " << (state.lastCacheValid ? "true" : "false")
		 << ",\n  \"composedLastFrame\": " << (state.lastComposed ? "true" : "false")
		 << ",\n  \"sourceAlphaCoverage\": " << state.lastSourceAlphaCoverage
		 << ",\n  \"anomalyCount\": " << state.incidents.anomalyCount
		 << ",\n  \"incidentCount\": " << state.incidents.incidentCount << ",\n  \"lastReason\": \""
		 << JsonEscape(state.incidents.lastReason) << "\""
		 << ",\n  \"lastIncidentDirectory\": \"" << JsonEscape(Utf8Path(state.incidents.lastDirectory)) << "\"";
	if (state.hasMetrics)
	{
		json << ",\n  \"ui\": { \"mean\": " << state.metrics.mean << ", \"meanLeft\": " << state.metrics.meanLeft
			 << ", \"meanRight\": " << state.metrics.meanRight << ", \"temporal\": " << state.metrics.temporal
			 << ", \"temporalLeft\": " << state.metrics.temporalLeft
			 << ", \"temporalRight\": " << state.metrics.temporalRight << " }";
	}
	if (triggerFrame)
	{
		json << ",\n  \"triggerFrame\": " << *triggerFrame << ",\n  \"reason\": \""
			 << JsonEscape(state.incidents.lastReason) << "\"";
	}
	if (diagnostics)
	{
		json << ",\n  \"closed\": " << (diagnostics->closed ? "true" : "false")
			 << ",\n  \"fileErrors\": " << diagnostics->fileErrors
			 << ",\n  \"observationErrors\": " << diagnostics->observationErrors
			 << ",\n  \"droppedArtifacts\": " << diagnostics->droppedArtifacts
			 << ",\n  \"outstandingArtifacts\": " << diagnostics->outstandingArtifacts
			 << ",\n  \"outstandingBytes\": " << diagnostics->outstandingBytes << ",\n  \"lastError\": \""
			 << JsonEscape(diagnostics->lastError) << "\"";
	}
	json << "\n}\n";
	return json.str();
}

} // namespace

void ArtifactQueue::Add(IncidentUpdate update, const PreviewMonitor& monitor)
{
	m_renderDocRequested |= update.anomaly;
	if (update.incident)
		Admit(PreviewIncident{std::move(*update.incident), monitor.Snapshot()});
	if (update.sample)
		Admit(std::move(*update.sample));
}

void ArtifactQueue::Add(IncidentUpdate update, const UiMonitor& monitor)
{
	if (update.incident)
		Admit(UiIncident{std::move(*update.incident), monitor.Snapshot()});
	if (update.sample)
		Admit(std::move(*update.sample));
}

void ArtifactQueue::Admit(Artifact artifact)
{
	const auto charge = Charge(artifact);
	if (m_outstanding >= MaxArtifactJobs || charge.bytes > MaxArtifactBytes - m_bytes ||
		charge.samples > MaxArtifactSamples - m_samples)
	{
		++m_dropped;
		return;
	}
	m_jobs.push_back({std::move(artifact), charge});
	++m_outstanding;
	m_bytes += charge.bytes;
	m_samples += charge.samples;
}

bool ArtifactQueue::Empty() const
{
	return m_jobs.empty();
}

std::optional<ArtifactJob> ArtifactQueue::Take()
{
	if (m_jobs.empty())
		return {};
	auto job = std::move(m_jobs.front());
	m_jobs.pop_front();
	return job;
}

void ArtifactQueue::Complete(ArtifactCharge charge)
{
	--m_outstanding;
	m_bytes -= charge.bytes;
	m_samples -= charge.samples;
}

size_t ArtifactQueue::Outstanding() const
{
	return m_outstanding;
}

size_t ArtifactQueue::OutstandingBytes() const
{
	return m_bytes;
}

uint64_t ArtifactQueue::Dropped() const
{
	return m_dropped;
}

bool ArtifactQueue::TakeRenderDocRequest()
{
	return std::exchange(m_renderDocRequested, false);
}

uint64_t ProcessId()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return static_cast<uint64_t>(getpid());
#endif
}

void WriteArtifact(const Artifact& artifact)
{
	std::visit(
		[](const auto& value)
		{
			using Value = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<Value, SampleArtifact>)
				SaveSample(value);
			else
			{
				const auto& packet = value.packet;
				if constexpr (std::is_same_v<Value, PreviewIncident>)
				{
					WriteText(packet.directory / "incident.json",
							  BuildStatusJson(value.statistics, nullptr, packet.triggerFrame));
					WriteText(
						packet.directory / "LLM_REVIEW.md",
						"# OpenXR Simulator visible-preview flicker incident\n\n"
						"The packet contains the composed CPU preview surface. Review paired `color_L` and `color_R` "
						"frames for flashes, missing-eye frames, alternation and frozen output. "
						"Submission context is in `incident.json` and `flicker_status.json`.\n");
				}
				else
				{
					WriteText(packet.directory / "incident.json",
							  BuildUiStatusJson(value.statistics, nullptr, packet.triggerFrame));
					WriteText(
						packet.directory / "LLM_REVIEW.md",
						"# OpenXR Simulator UI-only flicker incident\n\n"
						"Images are cropped to the submitted quad in each eye. Review `color_L` and `color_R` for UI "
						"presence/absence alternation. Structural and composition counters are in `incident.json` "
						"and `ui_flicker_status.json`.\n");
				}
				for (const auto& sample : packet.history)
					SaveSample({packet.directory, sample});
			}
		},
		artifact);
}

void WritePreviewStatus(const std::filesystem::path& directory, const DiagnosticsStatus& status)
{
	WriteText(directory / "flicker_status.json", BuildStatusJson(status.preview, &status));
}

void WriteUiStatus(const std::filesystem::path& directory, const DiagnosticsStatus& status)
{
	WriteText(directory / "ui_flicker_status.json", BuildUiStatusJson(status.ui, &status));
}

bool ConsumeRequest(const std::filesystem::path& path)
{
	return std::filesystem::remove(path);
}

void PruneArtifacts(const std::filesystem::path& directory, std::string_view activeSession)
{
	using Entry = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
	for (const auto* rootName : {"flicker_incidents", "ui_flicker_incidents"})
	{
		std::vector<Entry> sessions;
		std::error_code error;
		for (std::filesystem::directory_iterator entry(directory / rootName, error), end; !error && entry != end;
			 entry.increment(error))
		{
			const auto name = Utf8Path(entry->path().filename());
			if (entry->is_directory(error) && name.starts_with("session_") && name != activeSession)
				sessions.emplace_back(entry->last_write_time(error), entry->path());
		}
		std::sort(sessions.begin(), sessions.end(),
				  [](const auto& first, const auto& second)
				  {
					  return first.first > second.first;
				  });
		for (size_t index = KeepRecentSessions; index < sessions.size(); ++index)
			std::filesystem::remove_all(sessions[index].second);
	}

	const auto ownLog = "openxr_simulator." + std::to_string(ProcessId()) + ".log";
	std::vector<Entry> logs;
	std::error_code error;
	for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
		 entry.increment(error))
	{
		const auto name = Utf8Path(entry->path().filename());
		if (entry->is_regular_file(error) && name.starts_with("openxr_simulator.") && name.ends_with(".log") &&
			name != ownLog)
			logs.emplace_back(entry->last_write_time(error), entry->path());
	}
	std::sort(logs.begin(), logs.end(),
			  [](const auto& first, const auto& second)
			  {
				  return first.first > second.first;
			  });
	for (size_t index = KeepRecentLogs; index < logs.size(); ++index)
		std::filesystem::remove(logs[index].second);
}

namespace
{
#ifdef _WIN32
#define SIMXR_CAPTURE_CALL __cdecl
#else
#define SIMXR_CAPTURE_CALL
#endif
using RenderDocSetCapturePath = void(SIMXR_CAPTURE_CALL*)(const char*);
using RenderDocTriggerCapture = void(SIMXR_CAPTURE_CALL*)(uint32_t);
// RenderDoc's append-only 1.1.0 layout through TriggerMultiFrameCapture.
struct RenderDocApi110
{
	void* getApiVersion;
	void* setCaptureOptionU32;
	void* setCaptureOptionF32;
	void* getCaptureOptionU32;
	void* getCaptureOptionF32;
	void* setFocusToggleKeys;
	void* setCaptureKeys;
	void* getOverlayBits;
	void* maskOverlayBits;
	void* removeHooks;
	void* unloadCrashHandler;
	RenderDocSetCapturePath setCaptureFilePathTemplate;
	void* getCaptureFilePathTemplate;
	void* getNumCaptures;
	void* getCapture;
	void* triggerCapture;
	void* isTargetControlConnected;
	void* launchReplayUi;
	void* setActiveWindow;
	void* startFrameCapture;
	void* isFrameCapturing;
	void* endFrameCapture;
	RenderDocTriggerCapture triggerMultiFrameCapture;
};
using RenderDocGetApi = int(SIMXR_CAPTURE_CALL*)(int, void**);
#undef SIMXR_CAPTURE_CALL

class CaptureModule
{
  public:
	CaptureModule()
	{
#ifdef _WIN32
		GetModuleHandleExW(0, L"renderdoc.dll", &m_module);
#else
		// Resolve an already injected API, then retain its actual module while calling it.
		Dl_info information{};
		if (const auto* symbol = dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"); symbol && dladdr(symbol, &information))
			m_module = dlopen(information.dli_fname, RTLD_NOW | RTLD_NOLOAD);
#endif
	}
	CaptureModule(const CaptureModule&) = delete;
	CaptureModule& operator=(const CaptureModule&) = delete;
	~CaptureModule()
	{
		if (m_module)
		{
#ifdef _WIN32
			FreeLibrary(m_module);
#else
			dlclose(m_module);
#endif
		}
	}
	RenderDocGetApi Entry() const
	{
		if (!m_module)
			return nullptr;
#ifdef _WIN32
		return reinterpret_cast<RenderDocGetApi>(GetProcAddress(m_module, "RENDERDOC_GetAPI"));
#else
		return reinterpret_cast<RenderDocGetApi>(dlsym(m_module, "RENDERDOC_GetAPI"));
#endif
	}

  private:
#ifdef _WIN32
	HMODULE m_module = nullptr;
#else
	void* m_module = nullptr;
#endif
};
} // namespace

RenderDocCapture::RenderDocCapture()
{
	if (const auto* enabled = std::getenv("BVR_RENDERDOC_AUTO_CAPTURE"))
		m_enabled = enabled[0] != '\0' && enabled[0] != '0';
	if (const auto* path = std::getenv("BVR_RENDERDOC_CAPTURE_TEMPLATE"))
		m_captureTemplate = path;
	if (const auto* frames = std::getenv("BVR_RENDERDOC_CAPTURE_FRAMES"))
		m_frameCount = static_cast<uint32_t>(std::clamp(std::strtoul(frames, nullptr, 10), 1ul, 10ul));
}

RenderDocResult RenderDocCapture::Trigger(uint64_t frame)
{
	if (!m_enabled || (m_lastTriggerFrame && frame >= *m_lastTriggerFrame && frame - *m_lastTriggerFrame < 60))
		return {};
	CaptureModule module;
	const auto getApi = module.Entry();
	if (!getApi)
		return {true, false};
	RenderDocApi110* api = nullptr;
	if (getApi(10100, reinterpret_cast<void**>(&api)) != 1 || !api || !api->triggerMultiFrameCapture)
		return {true, false};
	if (!m_captureTemplate.empty() && api->setCaptureFilePathTemplate)
		api->setCaptureFilePathTemplate(m_captureTemplate.c_str());
	api->triggerMultiFrameCapture(m_frameCount);
	m_lastTriggerFrame = frame;
	return {true, true};
}
} // namespace flicker::detail
