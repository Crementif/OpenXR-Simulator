#include "Pch.h"
#include "app/mcp/McpImage.h"
#include "app/mcp/McpIncident.h"
#include "app/mcp/McpIncidentAnalysis.h"
#include "app/mcp/McpFlickerCoverage.h"
#include "app/mcp/McpIncidentSave.h"
#include "app/mcp/McpImageCache.h"
#include "app/mcp/McpImageEncode.h"
#include "app/mcp/McpStereoAnalysis.h"
#include "control/ControlSchema.h"
#include "graphics/ColorPixels.h"
#include "../TestSupport.h"

#include <cstring>
#include <exception>
#include <latch>
#include <limits>
#include <thread>

int main(int argc, char** argv)
{
	using namespace simxr;
	using namespace simxr::app::mcp;
	{
		using Json = nlohmann::json;
		FlickerCoverage coverage;
		coverage.AddWindow(Json::array({{{"frame_id", "1"}}, {{"frame_id", "2"}}}),
			{{"skipped_frames", Json::array({{{"frame_id", "1"}, {"reason", "missing_eye"}}})}, {"events", Json::array()}});
		coverage.AddWindow(Json::array({{{"frame_id", "3"}}, {{"frame_id", "4"}}}),
			{{"skipped_frames", Json::array()}, {"events", Json::array()}});
		auto status = coverage.Report();
		Require(status.at("frames_observed") == 3 && status.at("frames_skipped") == 1,
			"Earlier skipped-eye coverage remains visible after a healthy window");
		FlickerCoverage recovered;
		recovered.AddWindow(Json::array({{{"frame_id", "1"}}, {{"frame_id", "2"}}}),
			{{"skipped_frames", Json::array({{{"frame_id", "2"}}})}, {"events", Json::array()}});
		recovered.AddWindow(Json::array({{{"frame_id", "2"}}, {{"frame_id", "4"}}}),
			{{"skipped_frames", Json::array()}, {"events", Json::array()}});
		status = recovered.Report();
		Require(status.at("frames_observed") == 3 && status.at("frames_skipped") == 0 && status.at("unobserved_frames") == 1,
			"Overlapping windows recover skipped frames without double-counting and preserve gaps");
		FlickerCoverage overflow;
		for (unsigned start = 1; start <= 70; start += 10)
		{
			Json entries = Json::array(), events = Json::array();
			for (unsigned frame = start; frame < start + 10; ++frame)
			{
				const auto id = std::to_string(frame);
				entries.push_back({{"frame_id", id}});
				events.push_back({{"frame_id", id}, {"reason", "test"}});
			}
			overflow.AddWindow(entries, {{"skipped_frames", Json::array()}, {"events", events}});
		}
		status = overflow.Report();
		Require(status.at("events").size() == 64 && status.at("events_total") == 70 && status.at("events_dropped") == 6,
			"Monitor event overflow is bounded and explicitly reported");
	}
	Require(IncidentIdentifier("18446744073709551615", "frame") == UINT64_MAX,
		"Incident frame identifiers retain full uint64 precision");
	{
		const std::array<std::array<std::int32_t, 4>, 2> rectangles{{{{0, 0, 160, 90}}, {{160, 0, 160, 90}}}};
		std::vector<std::byte> cell(320 * 104 * 4, std::byte{128});
		IncidentAnalysis preview(false);
		for (std::uint64_t frame = 1; frame <= 5; ++frame)
			preview.Observe(frame, cell, rectangles);
		std::fill(cell.begin(), cell.end(), std::byte{0});
		preview.Observe(6, cell, rectangles);
		const auto report = preview.Report();
		Require(report.at("detected") && report.at("frames_analyzed") == 6 &&
			report.at("events")[0].at("frame_id") == "6" &&
			report.at("events")[0].at("reason") == "VISIBLE_TO_BLANK_FRAME",
			"On-demand preview analysis identifies a blank frame");
		IncidentAnalysis ui(true);
		std::fill(cell.begin(), cell.end(), std::byte{80});
		ui.Observe(1, cell, rectangles);
		std::fill(cell.begin(), cell.end(), std::byte{180});
		ui.Observe(2, cell, rectangles);
		std::fill(cell.begin(), cell.end(), std::byte{80});
		ui.Observe(3, cell, rectangles);
		ui.Observe(4, cell, {});
		const auto uiReport = ui.Report();
		Require(uiReport.at("detected") && uiReport.at("frames_skipped") == 1 &&
			uiReport.at("events")[0].at("reason") == "ALTERNATING_UI_REGION",
			"UI analysis identifies alternating crops and skips missing eyes");
		IncidentAnalysis gaps(true);
		gaps.Observe(1, cell, rectangles);
		gaps.Observe(2, cell, {});
		gaps.Observe(3, cell, rectangles);
		gaps.Observe(8, cell, rectangles);
		const auto incomplete = gaps.Report();
		Require(incomplete.at("verdict") == "INCONCLUSIVE" && incomplete.at("temporal_pairs") == 0 &&
			incomplete.at("frames_skipped") == 1 && incomplete.at("frame_gaps") == 1,
			"Missing eyes and frame gaps cannot produce a clean temporal verdict");
	}
	for (const auto& invalid : {std::string("01"), std::string(100000, '0') + "1", std::string("18446744073709551616")})
	{
		bool rejected = false;
		try { (void)IncidentIdentifier(invalid, "frame"); }
		catch (const std::runtime_error&) { rejected = true; }
		Require(rejected, "Incident rejected an oversized, noncanonical or overflowing frame identifier");
	}
	{
		const auto root = std::filesystem::temp_directory_path() /
			("simxr-incident-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Require(std::filesystem::create_directory(root), "Create isolated incident test directory");
		nlohmann::json metadata = {{"window", {{"entries", nlohmann::json::array({
			{{"frame_id", "1"}, {"phase", "ready"}}, {{"frame_id", "2"}, {"phase", "clear"}}})}}}};
		const std::vector<std::vector<std::byte>> frames(2, std::vector<std::byte>(320 * 104 * 4));
		const std::vector<std::byte> jpeg(4);
		unsigned checks = 0;
		bool interrupted = false;
		try
		{
			SaveIncident(root / "interrupted", metadata, jpeg, frames, 320, 104, [&]
			{
				if (++checks == 6)
					throw std::runtime_error("cancelled");
			});
		}
		catch (const std::runtime_error&) { interrupted = true; }
		Require(interrupted && !std::filesystem::exists(root / "interrupted"),
			"Interrupted incident save must remove its own partial output");
		std::filesystem::remove(root);
	}
	Require(CacheableImageSource(static_cast<unsigned>(control::TargetKind::LayerView), false, false),
		"A captured layer without a display tick can be reused");
	Require(!CacheableImageSource(static_cast<unsigned>(control::TargetKind::VirtualDisplay), false, false) &&
		CacheableImageSource(static_cast<unsigned>(control::TargetKind::VirtualDisplay), true, true),
		"A virtual display needs its own image tick for cross-export reuse");
	Require(!CacheableImageSource(static_cast<unsigned>(control::TargetKind::ComposedEyes), true, false),
		"An invalid image tick must not be cached");
	{
		constexpr std::uint32_t width = 128, height = 96;
		std::vector<std::uint8_t> left(width * height), right(width * height), flat(width * height, 20);
		std::uint32_t state = 0x12345678;
		for (auto& pixel : left)
		{
			state = state * 1664525 + 1013904223;
			pixel = static_cast<std::uint8_t>(state >> 24);
		}
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 4; x < width; ++x)
				right[y * width + x] = left[y * width + x - 4];
		const auto check = [] {};
		const auto shifted = AnalyzeStereo(left, right, width, height, width, check);
		Require(shifted.at("verdict") == "PASS" && shifted.at("horizontal_disparity_px") == 4,
			"Stereo analysis did not recover the known eye shift");
		Require(AnalyzeStereo(left, left, width, height, width, check).at("verdict") == "FAIL_NO_PARALLAX",
			"Stereo analysis did not identify identical textured eyes");
		Require(AnalyzeStereo(flat, flat, width, height, width, check).at("verdict") == "INCONCLUSIVE_NO_FEATURES",
			"Featureless images should not imply a stereo verdict");
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
				flat[y * width + x] = (x % 8 < 4) ? 0 : 255;
		Require(AnalyzeStereo(flat, flat, width, height, width, check).at("verdict") == "INCONCLUSIVE_AMBIGUOUS",
			"Repeated patterns should not imply a unique stereo match");
		bool interrupted = false;
		try
		{
			(void)AnalyzeStereo(left, right, width, height, width, [] { throw std::runtime_error("cancelled"); });
		}
		catch (const std::runtime_error&)
		{
			interrupted = true;
		}
		Require(interrupted, "Stereo analysis ignored cancellation");
	}
	{
		ImageCache cache(30, 2);
		const auto makeImage = [](std::string data)
		{
			auto image = std::make_shared<CachedImage>();
			image->data = std::move(data);
			image->mediaType = "png";
			return image;
		};
		cache.Insert("first", makeImage("one"));
		cache.Insert("second", makeImage("two"));
		const auto retained = cache.Find("first");
		Require(retained && retained->data == "one", "Image cache did not preserve its content");
		cache.Insert("third", makeImage("three"));
		Require(!cache.Find("second") && cache.Find("first") && cache.Find("third"), "Image cache did not evict least recently used content");
		cache.Insert("huge", makeImage(std::string(30, 'x')));
		Require(!cache.Find("huge") && cache.Find("first"), "Oversized content displaced the bounded cache");
		cache.Insert("fourth", makeImage("four"));
		cache.Insert("fifth", makeImage("five"));
		Require(retained->data == "one" && !cache.Find("first"), "An evicted image did not survive an active reader");
		ImageCache byteBound(15, 8);
		byteBound.Insert("a", makeImage("12345"));
		byteBound.Insert("b", makeImage("12345"));
		Require(!byteBound.Find("a") && byteBound.Find("b"), "Image cache exceeded its byte budget");
	}
	{
		ImageEncodingPermit first;
		ImageEncodingPermit second;
		bool rejected = false;
		try
		{
			ImageEncodingPermit third;
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected, "A third concurrent image encoding exceeded its budget");
	}
	const std::vector<std::byte> rgba(64 * 64 * 4, std::byte{0x7f});
	EncodedImage png, jpeg;
	ImageEncodingPermit firstPermit;
	ImageEncodingPermit secondPermit;
	std::exception_ptr firstError, secondError;
	std::latch start(1);
	std::jthread first([&]
	{
		try
		{
			start.wait();
			png = EncodeImage(rgba, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 64, 64, true, 85);
		}
		catch (...)
		{
			firstError = std::current_exception();
		}
	});
	std::jthread second;
	try
	{
		second = std::jthread([&]
		{
			try
			{
				start.wait();
				jpeg = EncodeImage(rgba, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 64, 64, false, 85);
			}
			catch (...)
			{
				secondError = std::current_exception();
			}
		});
	}
	catch (...)
	{
		start.count_down();
		throw;
	}
	start.count_down();
	first.join();
	second.join();
	if (firstError)
		std::rethrow_exception(firstError);
	if (secondError)
		std::rethrow_exception(secondError);
	Require(png.mediaType == "image/png" && !png.lossy && png.bytes.size() > 8 &&
			png.bytes[0] == std::byte{0x89} && png.bytes[1] == std::byte{0x50}, "Concurrent PNG encoding failed");
	Require(jpeg.mediaType == "image/jpeg" && jpeg.lossy && jpeg.bytes.size() > 4 &&
			jpeg.bytes[0] == std::byte{0xff} && jpeg.bytes[1] == std::byte{0xd8}, "Concurrent JPEG encoding failed");
	std::vector<std::byte> floating(8 * sizeof(float));
	const std::array values{-1.0f, 0.5f, 2.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1.0f};
	std::memcpy(floating.data(), values.data(), floating.size());
	const auto hdr = EncodeImage(floating, static_cast<std::uint32_t>(ColorFormat::Rgba32Float), 2, 1, true, 85);
	Require(hdr.lossy && !hdr.bytes.empty(), "HDR to PNG conversion did not report lost precision");
	const std::uint32_t packed = (512U << 10) | (1023U << 20) | (3U << 30);
	std::array<std::byte, 4> tenBit{};
	for (std::size_t index = 0; index < tenBit.size(); ++index)
		tenBit[index] = static_cast<std::byte>((packed >> (index * 8)) & 0xff);
	const auto rgb10 = EncodeImage(tenBit, static_cast<std::uint32_t>(ColorFormat::Rgb10A2Unorm), 1, 1, true, 85);
	Require(rgb10.lossy && !rgb10.bytes.empty(), "10-bit to PNG conversion did not report lost precision");
	if (argc == 2 && std::string_view(argv[1]) == "--fixtures")
	{
		std::printf("HDR:%s\nRGB10:%s\n", EncodeBase64(hdr.bytes).c_str(), EncodeBase64(rgb10.bytes).c_str());
	}
	bool rejected = false;
	try
	{
		(void)EncodeImage(rgba, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 4097, 1, true, 85);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Oversized image reached the encoder");
	return 0;
}
