#include "../TestSupport.h"
#include "diagnostics/FlickerArtifacts.h"
#include "diagnostics/FlickerMonitors.h"
#include "diagnostics/IncidentAnalysis.h"
#include "diagnostics/platform/ArtifactFile.h"
#include "flicker_detector.h"
#include "json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

namespace flicker::tests
{
using namespace flicker;
namespace fs = std::filesystem;

class Directory
{
public:
	Directory()
		: m_path(fs::temp_directory_path() /
				 ("simxr-flicker-" + std::to_string(ProcessId()) + "-" +
				  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
	{
		fs::create_directories(m_path);
	}

	~Directory()
	{
		std::error_code ignored;
		fs::remove_all(m_path, ignored);
	}

	const fs::path& Path() const
	{
		return m_path;
	}

private:
	fs::path m_path;
};

std::string ReadJson(const fs::path& path)
{
	std::ifstream input(path);
	Require(input.good(), "Diagnostic JSON exists");
	return {std::istreambuf_iterator<char>(input), {}};
}

std::vector<uint8_t> Gray(uint32_t width, uint32_t height, uint8_t brightness, uint32_t pitch = 0)
{
	std::vector<uint8_t> pixels(static_cast<size_t>(pitch ? pitch : width * 4) * height, 0xFF);
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x)
		{
			auto* pixel = pixels.data() + static_cast<size_t>(y) * (pitch ? pitch : width * 4) + x * 4;
			std::fill_n(pixel, 3, brightness);
		}
	return pixels;
}

void SamplePixelOrders()
{
	constexpr uint32_t width = 640, height = 200, pitch = width * 4 + 16;
	std::vector<uint8_t> rgba(pitch * height, 0xBA), bgra(pitch * height, 0xCD);
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x)
		{
			const auto offset = size_t(y) * pitch + x * 4;
			rgba[offset] = bgra[offset + 2] = x < width / 2 ? 255 : 0;
			rgba[offset + 1] = bgra[offset + 1] = static_cast<uint8_t>(y);
			rgba[offset + 2] = bgra[offset] = x < width / 2 ? 0 : 255;
			rgba[offset + 3] = bgra[offset + 3] = static_cast<uint8_t>(x);
		}
	const auto compare = [](const Sample& first, const Sample& second) {
		Require(first.frame == second.frame && first.width == second.width && first.height == second.height &&
					*first.pixels == *second.pixels,
				"RGBA and BGRA produce identical retained samples including alpha");
		Require(first.metrics.mean == second.metrics.mean && first.metrics.meanLeft == second.metrics.meanLeft &&
					first.metrics.meanRight == second.metrics.meanRight &&
					first.metrics.brightFractionLeft == second.metrics.brightFractionLeft &&
					first.metrics.brightFractionRight == second.metrics.brightFractionRight,
				"Channel conversion preserves brightness metrics");
		Require(first.metrics.meanLeft > first.metrics.meanRight, "Red is brighter than blue in sampled luminance");
	};
	compare(Downsample(rgba.data(), width, height, pitch, 7, PixelOrder::Rgba),
			Downsample(bgra.data(), width, height, pitch, 7));
	const int32_t rectangles[2][4]{{-10, 20, 300, 160}, {330, 20, 320, 160}};
	compare(CropUi(rgba.data(), width, height, pitch, rectangles, 9, PixelOrder::Rgba),
			CropUi(bgra.data(), width, height, pitch, rectangles, 9));
}

void ClassificationAndDrain()
{
	Directory directory;
	Diagnostics diagnostics(directory.Path() / fs::path(u8"évidence"));
	auto pixels = Gray(8, 4, 128, 256);
	for (uint64_t frame = 1; frame <= 5; ++frame)
	{
		diagnostics.ObserveSubmission(frame, 1, 1);
		diagnostics.ObservePreview(pixels.data(), 8, 4, 256, frame, frame);
		diagnostics.ObservePaint(frame, true);
	}
	diagnostics.ObservePreview(reinterpret_cast<const uint8_t*>(1), 8, 4, 256, 5, 6);
	Require(diagnostics.Status().preview.previewSamples == 5, "Repeated generation is ignored before reading pixels");
	Require(std::abs(diagnostics.Status().preview.metrics.mean - 128.0f / 255.0f) < 0.00001f,
			"Row padding does not affect sampled luminance");
	pixels = Gray(8, 4, 0, 256);
	diagnostics.ObserveSubmission(6, 1, 1);
	diagnostics.ObservePreview(pixels.data(), 8, 4, 256, 6, 6);
	const auto incident = diagnostics.Status().preview.incidents;
	Require(incident.lastReason == "VISIBLE_TO_BLANK_FRAME" && incident.incidentCount == 1,
			"Visible-to-blank classification retains its threshold");
	std::fill(pixels.begin(), pixels.end(), 255);
	diagnostics.ObservePaint(5, false);
	diagnostics.Close();
	const auto status = diagnostics.Status();
	Require(status.closed && status.outstandingArtifacts == 0 && status.outstandingBytes == 0,
			"Close drains active and queued ownership");
	Require(status.fileErrors == 0, "Accepted incident files finish successfully");
	const auto incidentText = ReadJson(incident.lastDirectory / "incident.json");
	const json::Object document(incidentText.c_str());
	Require(document.String("reason") == "VISIBLE_TO_BLANK_FRAME" && document.Number("triggerFrame", 0) == 6,
			"Incident metadata describes the trigger snapshot");
	const auto image = incident.lastDirectory / "frame6_color_L.bmp";
	std::ifstream bitmap(image, std::ios::binary);
	bitmap.seekg(54);
	std::array<uint8_t, 3> pixel{};
	bitmap.read(reinterpret_cast<char*>(pixel.data()), pixel.size());
	Require(bitmap.good() && pixel == std::array<uint8_t, 3>{0, 0, 0}, "Worker owns immutable sampled CPU pixels");
	const auto finalText = ReadJson(directory.Path() / fs::path(u8"évidence") / "flicker_status.json");
	const json::Object final(finalText.c_str());
	Require(final.Boolean("closed", false) && final.Number("outstandingArtifacts", -1) == 0 &&
				final.Number("failedPaints", 0) == 1,
			"Final status is published after the complete drain");
	diagnostics.ObserveSubmission(99, 0, 0);
	diagnostics.ObservePaint(0, false);
	Require(diagnostics.Status().preview.totalSubmissions == 6, "Closed owner rejects late observers");
	diagnostics.Close();
}

void ProjectionAndUiSignals()
{
	PreviewMonitor preview("preview");
	preview.Submission(0, 0, 0);
	Require(preview.Snapshot().incidents.anomalyCount == 0, "Missing projection at startup is expected");
	for (uint64_t frame = 1; frame <= 5; ++frame)
		preview.Submission(frame, 1, 1);
	const auto update = preview.Submission(6, 0, 1);
	Require(update.incident && preview.Snapshot().incidents.lastReason == "MISSING_PROJECTION_LAYER",
			"Established projection disappearing creates an incident");
	preview.Submission(7, 0, 0);
	Require(preview.Snapshot().consecutiveMissingProjection == 2 &&
				preview.Snapshot().projectionPresenceTransitions == 2,
			"Projection transitions and streaks are distinct");

	UiMonitor ui("ui");
	UiFrameInfo info;
	info.quadLayers = 1;
	info.projectionRefreshed = true;
	info.freshReadback = true;
	info.cacheValid = true;
	info.sourceAlphaCoverage = std::numeric_limits<float>::quiet_NaN();
	const auto failure = ui.Observe(nullptr, 0, 0, 0, 0, 9, info);
	Require(failure.incident && ui.Snapshot().compositionFailures == 1 && ui.Snapshot().uiSamples == 0,
			"UI structural failures do not require CPU pixels");
	Require(ui.Snapshot().incidents.lastReason == "UI_COMPOSITION_FAILED" && ui.Snapshot().lastSourceAlphaCoverage == 0,
			"UI composition classification and finite JSON values are preserved");
	ui.Observe(nullptr, 0, 0, 0, 0, 10, info);
	Require(ui.Snapshot().consecutiveMissingAfterProjection == 2, "Adjacent UI failures did not form a streak");
	ui.Observe(nullptr, 0, 0, 0, 0, 11, {});
	Require(ui.Snapshot().consecutiveMissingAfterProjection == 0,
			"A frame without UI did not end the missing-UI streak");
	ui.Observe(nullptr, 0, 0, 0, 0, 12, info);
	Require(ui.Snapshot().consecutiveMissingAfterProjection == 1 &&
				ui.Snapshot().maxConsecutiveMissingAfterProjection == 2 && ui.Snapshot().missingAfterProjection == 3,
			"Separated UI failures inflated the consecutive maximum or lost the cumulative count");
	Require(!preview.Submission(306, 1, 1).incident, "Restored projection is not anomalous");
	Require(preview.Submission(307, 0, 1).incident.has_value(),
			"Projection monitoring recovers without CPU preview samples");
	Require(ui.Observe(nullptr, 0, 0, 0, 0, 309, info).incident.has_value(),
			"UI monitoring recovers without CPU samples");

	UiMonitor absent("absent");
	const auto fullFrame = Gray(8, 4, 80);
	for (uint64_t frame = 1; frame <= 12; ++frame)
		absent.Observe(fullFrame.data(), 8, 4, 32, frame, frame, {});
	Require(absent.Snapshot().uiSamples == 0, "Missing UI rectangles do not fabricate samples");

	UiMonitor cropped("cropped");
	info.composed = true;
	info.rects[0][0] = 0;
	info.rects[0][1] = 0;
	info.rects[0][2] = 2;
	info.rects[0][3] = 2;
	info.rects[1][0] = 6;
	info.rects[1][1] = 0;
	info.rects[1][2] = 2;
	info.rects[1][3] = 2;
	auto pixels = Gray(8, 4, 80);
	cropped.Observe(pixels.data(), 8, 4, 32, 1, 1, info);
	for (uint32_t y = 2; y < 4; ++y)
		std::fill_n(pixels.data() + y * 32, 32, 255);
	cropped.Observe(pixels.data(), 8, 4, 32, 2, 2, info);
	Require(cropped.Snapshot().metrics.temporal == 0 && cropped.Snapshot().uiSamples == 2,
			"World motion outside the quad rectangles is excluded");
	cropped.Observe(pixels.data(), 8, 4, 32, 2, 3, info);
	Require(cropped.Snapshot().uiSamples == 2 && cropped.Snapshot().observedFrames == 3,
			"Duplicate UI generation still records structural counters");
}

void StreamingAnalysis()
{
	IncidentAnalysis analysis(false);
	auto pixels = Gray(8, 4, 80);
	auto bytes = std::as_bytes(std::span(pixels));
	auto observe = [&](uint64_t frame) {
		return analysis.Observe(frame, bytes, 8, 4, 0, PixelOrder::Rgba);
	};
	observe(1);
	observe(2);
	analysis.Skip(3);
	observe(4);
	observe(6);
	observe(7);
	Require(analysis.Counts().observations == 5 && analysis.Counts().skipped == 1 &&
				analysis.Counts().temporalPairs == 2 && analysis.Counts().gaps == 1,
			"Unavailable frames and gaps must break temporal comparisons");
	bool rejected = false;
	try
	{
		analysis.Observe(8, bytes.first(bytes.size() - 1), 8, 4, 32, PixelOrder::Rgba);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && analysis.Counts().observations == 5, "Truncated images must not advance analysis");
	observe(8);
	Require(analysis.Counts().temporalPairs == 3 && analysis.Counts().gaps == 1,
			"Rejected input must not break frame continuity");
	bool detected = false;
	for (uint64_t frame = 9; frame < 80; ++frame)
	{
		std::fill(pixels.begin(), pixels.end(), frame % 2 ? 80 : 200);
		detected |= !observe(frame).reason.empty();
	}
	Require(detected, "Streaming analysis must return anomaly events without retaining a report");

	IncidentAnalysis ui(true);
	const auto missing = ui.Observe(1, bytes, 8, 4, 32, PixelOrder::Rgba);
	Require(missing.skipped && missing.reason == "missing_eye" && ui.Counts().observations == 0,
			"UI analysis must report absent eye rectangles");
	const std::array<std::array<int32_t, 4>, 2> rectangles{{{0, 0, 4, 4}, {4, 0, 4, 4}}};
	ui.Observe(2, bytes, 8, 4, 32, PixelOrder::Rgba, rectangles);
	Require(ui.Counts().observations == 1 && ui.Counts().temporalPairs == 0 && ui.Counts().skipped == 1,
			"A missing eye must not become a temporal sample");
}

void AnalysisWithoutArtifacts()
{
	PreviewMonitor preview({});
	UiMonitor ui({});
	UiFrameInfo info;
	info.rects[0][2] = info.rects[1][2] = 4;
	info.rects[0][3] = info.rects[1][3] = 4;
	info.rects[1][0] = 4;
	for (uint64_t frame = 1; frame <= 20; ++frame)
	{
		auto pixels = Gray(8, 4, frame % 2 ? 80 : 200);
		const auto composed = preview.Preview(Downsample(pixels.data(), 8, 4, 32, frame), frame);
		const auto overlay = ui.Observe(pixels.data(), 8, 4, 32, frame, frame, info);
		Require(!composed.incident && !composed.sample && !overlay.incident && !overlay.sample,
				"Analysis without an artifact directory does not construct discarded captures");
	}
	Require(preview.Snapshot().incidents.anomalyCount > 0 && ui.Snapshot().incidents.anomalyCount > 0,
			"Analysis still detects flicker without artifact capture");
	Require(preview.Snapshot().incidents.lastReason == "ALTERNATING_VISIBLE_FRAMES" &&
				ui.Snapshot().incidents.lastReason == "ALTERNATING_UI_REGION",
			"Analysis preserves anomaly reasons");
}

void BoundedArtifacts()
{
	PreviewMonitor monitor("queue");
	ArtifactQueue queue;
	Sample sample = Downsample(Gray(8, 4, 80).data(), 8, 4, 32, 1);
	for (size_t index = 0; index < 64; ++index)
	{
		IncidentUpdate update;
		update.sample = SampleArtifact{"queue", sample};
		queue.Add(std::move(update), monitor);
	}
	auto active = queue.Take();
	Require(active.has_value() && queue.Outstanding() == 64, "Dequeued work retains its capacity charge");
	IncidentUpdate rejected;
	rejected.sample = SampleArtifact{"queue", sample};
	queue.Add(std::move(rejected), monitor);
	Require(queue.Dropped() == 1 && queue.Outstanding() == 64, "Active and queued samples share one bound");
	const auto charge = active->charge;
	active.reset();
	queue.Complete(charge);
	IncidentUpdate accepted;
	accepted.sample = SampleArtifact{"queue", sample};
	queue.Add(std::move(accepted), monitor);
	Require(queue.Outstanding() == 64 && queue.Dropped() == 1, "Completed work returns capacity");
	while (auto job = queue.Take())
	{
		const auto completed = job->charge;
		job.reset();
		queue.Complete(completed);
	}
	Require(queue.OutstandingBytes() == 0, "All byte charges are returned after retirement");

	ArtifactQueue bytes;
	sample.pixels = std::make_shared<const std::vector<uint8_t>>(7 * 1024 * 1024);
	for (size_t index = 0; index < 3; ++index)
	{
		IncidentUpdate update;
		update.sample = SampleArtifact{"queue", sample};
		bytes.Add(std::move(update), monitor);
	}
	Require(bytes.Outstanding() == 2 && bytes.Dropped() == 1,
			"Aggregate bytes bound sample memory independently of count");
}

void IncidentLimits()
{
	IncidentCapture capture("capture");
	const auto sample = Downsample(Gray(8, 4, 80).data(), 8, 4, 32, 0);
	const auto first = capture.Begin("TEST", 0, {});
	Require(first.incident.has_value(), "Frame zero can start an incident");
	for (uint64_t frame = 1; frame <= 12; ++frame)
	{
		auto next = sample;
		next.frame = frame;
		Require(capture.Continue(next).has_value(), "Incident retains its twelve trailing samples");
	}
	Require(!capture.Begin("TEST", 20, {}).incident, "Frame zero incident participates in cooldown");
	for (uint64_t index = 1; index <= 40; ++index)
	{
		const auto incident = capture.Begin("TEST", index * 1000, {});
		Require(incident.incident.has_value() == (index < 40), "Automatic capture preserves the forty-incident limit");
		for (uint64_t after = 0; after < 12; ++after)
		{
			auto next = sample;
			next.frame = index * 1000 + after;
			capture.Continue(next);
		}
	}
	Require(capture.Statistics().incidentCount == 40, "Capture limit is retained across completed incidents");

	IncidentCapture sparse("sparse");
	Require(sparse.Begin("TEST", 1, {}).incident.has_value(), "Sparse incident begins");
	auto late = sample;
	late.frame = 14;
	Require(!sparse.Continue(late), "Pixels after the trailing frame window do not extend an incident");
	Require(!sparse.Begin("TEST", 20, {}).incident, "Missing pixels do not bypass incident cooldown");
	Require(sparse.Begin("TEST", 301, {}).incident.has_value(), "Sparse capture recovers after cooldown");
	Require(sparse.Begin("TEST", 601, {}).incident.has_value(), "No-pixel capture cannot block later incidents");
}

void IncidentRecovery()
{
	Directory directory;
	const auto capture = [&] {
		Diagnostics diagnostics(directory.Path());
		for (uint64_t frame = 1; frame <= 5; ++frame)
			diagnostics.ObserveSubmission(frame, 1, 1);
		diagnostics.ObserveSubmission(6, 0, 0);
		diagnostics.Close();
		const auto incident = diagnostics.Status().preview.incidents;
		Require(incident.incidentCount == 1 &&
					json::Object(ReadJson(incident.lastDirectory / "incident.json").c_str()).String("reason") ==
						"MISSING_PROJECTION_LAYER",
				"Close drains an automatic incident without pixel readback");
		return incident.lastDirectory;
	};
	const auto first = capture();
	Require(capture() != first, "Recreated diagnostics cannot overwrite an incident at the same frame");

	const auto blocked = directory.Path() / "blocked";
	{
		std::ofstream file(blocked);
		file << "not a directory";
	}
	Diagnostics failing(blocked);
	failing.ObserveSubmission(1, 0, 0);
	Eventually([&] { return failing.Status().fileErrors != 0; },
			   "I/O failure is observable and does not terminate the worker");
	fs::remove(blocked);
	fs::create_directory(blocked);
	for (uint64_t frame = 2; frame < 20; ++frame)
		failing.ObserveSubmission(frame, 0, 0);
	failing.Close();
	Require(json::Object(ReadJson(blocked / "flicker_status.json").c_str()).Number("fileErrors", 0) > 0,
			"Worker recovers from an unusable output directory");
	for (const auto& entry : fs::recursive_directory_iterator(directory.Path()))
		Require(entry.path().filename().string().find(".tmp.") == std::string::npos,
				"Failed writes leave no temporary artifacts");
}

void ArtifactFilesAndPruning()
{
	Directory directory;
	const auto destination = directory.Path() / "flicker_status.json";
	auto oldTemporary = destination;
	oldTemporary += ".tmp." + std::to_string(ProcessId());
	{
		std::ofstream output(oldTemporary);
		output << "another writer";
	}
	WritePreviewStatus(directory.Path(), {});
	Require(ReadJson(oldTemporary) == "another writer", "Status publication clobbered another writer's temporary file");
	fs::remove(oldTemporary);
	const auto first = CreateTemporaryArtifact(destination);
	const auto second = CreateTemporaryArtifact(destination);
	Require(first != second && fs::exists(first) && fs::exists(second),
			"Concurrent artifact writers did not reserve distinct files");
	{
		std::ofstream output(first);
		output << "first";
	}
	{
		std::ofstream output(second);
		output << "second";
	}
	CommitArtifact(second, destination);
	Require(ReadJson(destination) == "second" && ReadJson(first) == "first",
			"Publishing an artifact damaged another reserved write");
	CommitArtifact(first, destination);
	Require(ReadJson(destination) == "first" && !fs::exists(first) && !fs::exists(second),
			"Atomic replacement did not consume only its own temporary file");

	const auto blocked = directory.Path() / "flicker_incidents";
	{
		std::ofstream output(blocked);
		output << "not a directory";
	}
	std::array<fs::path, 12> logs;
	const auto now = fs::file_time_type::clock::now();
	for (size_t index = 0; index < logs.size(); ++index)
	{
		logs[index] = directory.Path() / ("openxr_simulator.prune_" + std::to_string(index) + ".log");
		std::ofstream(logs[index]).put('x');
		fs::last_write_time(logs[index], now - std::chrono::seconds(index));
	}
	Diagnostics diagnostics(directory.Path());
	Eventually([&] { return diagnostics.Status().fileErrors != 0; }, "Prune enumeration failure was silently ignored");
	diagnostics.Close();
	for (const auto& log : logs)
		Require(fs::exists(log), "Pruning continued after a failed artifact enumeration");
	Require(!diagnostics.Status().lastError.empty(), "Prune failure omitted its diagnostic detail");
	fs::remove(blocked);
	PruneArtifacts(directory.Path(), "session_active");
	for (size_t index = 0; index < logs.size(); ++index)
		Require(fs::exists(logs[index]) == (index < 10), "Recovered pruning did not keep the ten newest logs");
}

void ConcurrentCloseAndIndependentLifetime()
{
	Directory directory;
	Diagnostics first(directory.Path() / "first");
	Diagnostics second(directory.Path() / "second");
	std::atomic<bool> running{true};
	std::thread observer([&] {
		for (uint64_t frame = 1; running; ++frame)
			first.ObserveSubmission(frame, 1, 1);
	});
	std::thread close([&] { first.Close(); });
	first.Close();
	close.join();
	running = false;
	observer.join();
	second.ObserveSubmission(1, 0, 0);
	second.Close();
	Require(first.Status().closed && second.Status().preview.totalSubmissions == 1,
			"Concurrent closure is idempotent and owners do not share classifier state");
}
}

int main()
{
	using namespace flicker::tests;
	try
	{
		SamplePixelOrders();
		ClassificationAndDrain();
		ProjectionAndUiSignals();
		AnalysisWithoutArtifacts();
		StreamingAnalysis();
		BoundedArtifacts();
		IncidentLimits();
		IncidentRecovery();
		ArtifactFilesAndPruning();
		ConcurrentCloseAndIndependentLifetime();
		Require(!ValidPixels(reinterpret_cast<const uint8_t*>(1), UINT32_MAX, 2, 0),
				"Pixel width overflow is rejected");
		Require(!ValidPixels(reinterpret_cast<const uint8_t*>(1), 8, 4, 1), "Short source rows are rejected");
		std::cout << "Flicker diagnostics checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
