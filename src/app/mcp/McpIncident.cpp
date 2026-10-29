#include "app/mcp/McpPch.h"
#include "app/mcp/McpIncident.h"
#include "diagnostics/IncidentAnalysis.h"
#include "app/mcp/McpIncidentSave.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpImage.h"
#include "app/services/ImageExport.h"
#include "app/mcp/McpTransientDebug.h"
#include "app/mcp/McpSimulation.h"
#include "app/services/RuntimePanelProtocol.h"
#include "control/DebugProtocol.h"
#include "control/ImageInspection.h"
#include "control/IncidentSample.h"
#include "control/ProtocolTypes.h"
#include "control/ControlSchema.h"
#include "graphics/ColorPixels.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;
static constexpr unsigned EyeHeight = control::IncidentSample::Height, Header = 14;
static constexpr unsigned CellWidth = control::IncidentSample::Width, CellHeight = EyeHeight + Header;
static std::atomic<unsigned> s_incidents{0};

class IncidentPermit
{
  public:
	IncidentPermit()
	{
		auto count = s_incidents.load();
		for (;;)
		{
			if (count >= 2)
				throw std::runtime_error("Incident capture capacity is busy; retry explicitly");
			if (s_incidents.compare_exchange_weak(count, count + 1))
				break;
		}
	}
	~IncidentPermit() { --s_incidents; }
	IncidentPermit(const IncidentPermit&) = delete;
	IncidentPermit& operator=(const IncidentPermit&) = delete;
};
bool IncidentHandles(const std::string& name)
{
	return name == "capture_flicker_window" || name == "capture_ui_flicker_window";
}
Json IncidentDefinitions()
{
	const auto definition = [](const char* name, const char* description)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", {{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
				{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}},
				{"max_frames", {{"type", "integer"}, {"minimum", 2}, {"maximum", 12}}},
				{"analyze", {{"type", "boolean"}, {"description", "Analyze every retained frame for temporal pixel anomalies; this explicitly reads reduced images."}}},
				{"include_image", {{"type", "boolean"}, {"description", "Return the JPEG contact sheet; defaults to true."}}},
				{"directory", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096},
					{"description", "Optional absolute path to a new incident directory. Saves all retained frames, a contact sheet and manifest."}}}}},
			{"required", Json::array({"application_id"})}, {"additionalProperties", false}}}};
	};
	return Json::array({
		definition("capture_flicker_window", "Capture bounded pre/post composed frames as a labeled stereo contact sheet, optionally analyzing temporal pixel anomalies."),
		definition("capture_ui_flicker_window", "Capture bounded pre/post quad-only frames cropped to each eye's historical UI rectangle, optionally analyzing temporal pixel anomalies.")});
}
static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}
static void Label(std::vector<std::byte>& sheet, unsigned sheetWidth, unsigned x, unsigned y, std::string_view frame, bool clear)
{
	if (frame.empty() || frame.size() > 20 || x + frame.size() * 4 + (clear ? 5 : 0) > sheetWidth ||
		(std::size_t{y} + 5) * sheetWidth * 4 > sheet.size())
		throw std::runtime_error("Incident frame label exceeds its cell");
	static constexpr std::array<std::array<unsigned, 5>, 10> digits{{
		{{7, 5, 5, 5, 7}}, {{2, 6, 2, 2, 7}}, {{7, 1, 7, 4, 7}}, {{7, 1, 7, 1, 7}}, {{5, 5, 7, 1, 1}},
		{{7, 4, 7, 1, 7}}, {{7, 4, 7, 5, 7}}, {{7, 1, 1, 1, 1}}, {{7, 5, 7, 5, 7}}, {{7, 5, 7, 1, 7}}}};
	for (const auto digit : frame)
	{
		if (digit < '0' || digit > '9')
			throw std::runtime_error("Invalid incident frame label");
		const auto& glyph = digits[digit - '0'];
		for (unsigned row = 0; row < 5; ++row)
			for (unsigned column = 0; column < 3; ++column)
				if (glyph[row] & (1u << (2 - column)))
				{
					const auto offset = (std::size_t{y + row} * sheetWidth + x + column) * 4;
					sheet[offset] = sheet[offset + 1] = sheet[offset + 2] = std::byte{0xff};
				}
		x += 4;
	}
	if (clear)
	{
		// Amber square marks a clear frame; the metadata contains its exact state.
		for (unsigned row = 0; row < 5; ++row)
			for (unsigned column = 0; column < 5; ++column)
			{
				const auto offset = (std::size_t{y + row} * sheetWidth + x + column) * 4;
				sheet[offset] = std::byte{0xff};
				sheet[offset + 1] = std::byte{0xb0};
			}
	}
}
struct Rectangle
{
	std::uint32_t x, y, width, height;
};
static Rectangle EyeRectangle(const Json& entry, unsigned eye, unsigned width, unsigned height, bool ui)
{
	if (!ui)
		return {0, 0, width, height};
	const auto& rectangles = entry.at("quad_rectangles");
	if (!rectangles.is_array() || rectangles.size() != 2 || !rectangles[eye].is_object())
		throw std::runtime_error("Incident has invalid historical quad rectangles");
	const auto& rectangle = rectangles[eye];
	const auto x = rectangle.at("x").get<std::int64_t>();
	const auto y = rectangle.at("y").get<std::int64_t>();
	const auto w = rectangle.at("width").get<std::int64_t>();
	const auto h = rectangle.at("height").get<std::int64_t>();
	if (!w && !h)
		return {0, 0, 0, 0};
	if (x < 0 || y < 0 || w <= 0 || h <= 0 || x > width || y > height || w > width - x || h > height - y)
		throw std::runtime_error("Historical UI rectangle is outside its exact frame");
	return {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}
std::optional<IncidentCapture> CaptureIncident(McpApplication& application, const IncidentRequest& request,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	const auto& directory = request.directory;
	const auto timeout = request.timeout;
	const auto maximum = request.maximum;
	const auto ui = request.ui, analyze = request.analyze, includeImage = request.includeImage;
	const auto allowMissing = request.allowMissing;
	if (request.application.empty() || request.application.size() > 256 ||
		!std::isfinite(timeout) || timeout <= 0 || timeout > 30 || maximum < 2 || maximum > 12)
		throw std::invalid_argument("Invalid incident capture options");
	if (!cancellation->TryAdmit())
		return {};
	IncidentPermit permit;
	TransientDebug calls(application, cancellation, timeout, "Incident capture");
	const auto opened = calls.Connect(request.application);
	const auto connection = opened.id;
	const auto target = ui ? "quad_layers" : "composed_eyes";
	const auto started = calls.Request(connection, wire::OpCode::BeginImageWindow,
		{wire::EncodeCaptureTarget({ui ? CaptureKind::QuadLayers : CaptureKind::ComposedEyes, 0, 0})});
	const auto window = panel::U64(started, tag::ImageWindowId);
	if (!window)
		throw std::runtime_error("Runtime omitted the incident window identity");
	wire::Message windowInfo;
	Json document;
	for (;;)
	{
		windowInfo = calls.Request(connection, wire::OpCode::ReadImageWindow, {wire::Field::U64(tag::ImageWindowId, window, true)});
		const auto* value = windowInfo.Find(tag::Document);
		if (!value)
			throw std::runtime_error("Runtime omitted the incident window document");
		document = InspectionJson(value->value, true);
		const auto phase = document.at("phase").get<std::string>();
		if (phase == "complete")
		{
			bool pending = false;
			for (const auto& entry : document.at("entries"))
				pending |= entry.at("phase") == "pending";
			if (!pending)
				break;
		}
		else if (phase != "collecting")
			throw std::runtime_error("Incident window was interrupted: " + phase);
		calls.Wait();
	}
	const auto& entries = document.at("entries");
	if (!entries.is_array() || entries.size() < 2 || entries.size() > 24 ||
		!document.at("generation").is_number_unsigned() || document.at("generation") != opened.generation ||
		document.at("target") != target || document.at("pre_count") == 0 || document.at("post_count") != 12)
		throw std::runtime_error("Incident window has incomplete or mismatched frame history");
	for (const auto& entry : entries)
		if (!allowMissing && entry.at("phase") != "ready" && entry.at("phase") != "clear")
			throw std::runtime_error("Incident contains unavailable frame " + entry.at("frame_id").dump());
	const auto count = std::min<std::size_t>(maximum, entries.size());
	const auto sheetWidth = CellWidth * 2;
	const auto sheetHeight = CellHeight * static_cast<unsigned>((count + 1) / 2);
	const bool renderSheet = includeImage || !directory.empty();
	std::vector<std::byte> sheet(renderSheet ? std::size_t{sheetWidth} * sheetHeight * 4 : 0, std::byte{0});
	for (std::size_t index = 3; index < sheet.size(); index += 4)
		sheet[index] = std::byte{0xff};
	Json selectedFrames = Json::array();
	std::vector<std::vector<std::byte>> archivedFrames;
	std::optional<flicker::IncidentAnalysis> analysis;
	Json events = Json::array(), skippedFrames = Json::array();
	if (analyze)
		analysis.emplace(ui);
	if (!directory.empty())
		archivedFrames.reserve(entries.size());
	const auto iterations = directory.empty() && !analysis ? count : entries.size();
	std::size_t selectedIndex = 0;
	std::vector<std::byte> cell;
	for (std::size_t index = 0; index < iterations; ++index)
	{
		calls.Check();
		const auto sourceIndex = directory.empty() && !analysis ? index * (entries.size() - 1) / (count - 1) : index;
		const bool selectedForSheet = selectedIndex < count &&
			sourceIndex == selectedIndex * (entries.size() - 1) / (count - 1);
		const auto& entry = entries.at(sourceIndex);
		const auto& identity = entry.at("frame_id");
		if (!identity.is_number_unsigned() || identity == 0)
			throw std::runtime_error("Runtime returned an invalid incident frame identity");
		const auto frameId = identity.get<std::uint64_t>();
		const auto frame = std::to_string(frameId);
		const bool clear = entry.at("phase") == "clear";
		const bool missing = entry.at("phase") != "ready" && !clear;
		cell.assign(std::size_t{CellWidth} * CellHeight * 4, std::byte{0});
		for (std::size_t alpha = 3; alpha < cell.size(); alpha += 4)
			cell[alpha] = std::byte{0xff};
		if (renderSheet)
			Label(cell, CellWidth, 4, 4, frame, clear);
		std::array<std::array<std::int32_t, 4>, 2> analysisRectangles{};
		if ((renderSheet || analysis) && !clear && !missing)
		{
			const auto retained = calls.Request(connection, wire::OpCode::RetainWindowImage,
				{wire::Field::U64(tag::ImageWindowId, window, true), wire::Field::U64(tag::FrameId, frameId, true)});
			const auto image = panel::U64(retained, tag::ImageReaderId);
			if (!image)
				throw std::runtime_error("Runtime omitted the incident image identity");
			const auto info = calls.Request(connection, wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, image, true)});
			if (panel::Frame(info) != frameId || panel::U64(panel::Nested(info.Find(tag::Frame)), tag::Generation) != opened.generation ||
				panel::U32(panel::Nested(info.Find(tag::Target)), tag::CaptureKind, UINT32_MAX) !=
					static_cast<unsigned>(ui ? control::TargetKind::QuadLayers : control::TargetKind::ComposedEyes))
				throw std::runtime_error("Incident image does not match its window frame or target");
			const auto* descriptor = info.Find(tag::ImageDescriptor);
			if (!descriptor)
				throw std::runtime_error("Runtime omitted the incident image description");
			const auto description = wire::DecodeImageDescription(*descriptor);
			const auto width = description.width, height = description.height;
			if (!width || !height || description.arrayLayers < 2)
				throw std::runtime_error("Incident image is not stereo");
			std::array<XrRect2Di, 2> rectangles{};
			for (unsigned eye = 0; eye < 2; ++eye)
			{
				const auto rectangle = EyeRectangle(entry, eye, width, height, ui);
				if (rectangle.x > INT32_MAX || rectangle.y > INT32_MAX || rectangle.width > INT32_MAX || rectangle.height > INT32_MAX)
					throw std::runtime_error("Incident rectangle exceeds native coordinates");
				rectangles[eye] = {{static_cast<int>(rectangle.x), static_cast<int>(rectangle.y)},
					{static_cast<int>(rectangle.width), static_cast<int>(rectangle.height)}};
			}
			if (!rectangles[0].extent.width && !rectangles[1].extent.width)
				throw std::runtime_error("Ready UI frame has no visible quad rectangle");
			auto sample = control::ReadIncidentSample(
				[&](auto opcode, auto fields) { return calls.Request(connection, opcode, std::move(fields)); },
				[&] { calls.Wait(); }, image, description, rectangles);
			analysisRectangles = sample.rectangles;
			std::copy(sample.pixels.begin(), sample.pixels.end(), cell.begin() + std::size_t(Header) * CellWidth * 4);
			calls.Request(connection, wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, image, true)});
		}
		if (analysis)
		{
			if (missing)
			{
				analysis->Skip(frameId);
				skippedFrames.push_back({{"frame_id", frame}, {"reason", "image_unavailable"}});
			}
			else
			{
				auto observation = analysis->Observe(frameId,
					std::span<const std::byte>(cell).subspan(std::size_t(Header) * CellWidth * 4),
					CellWidth, EyeHeight, CellWidth * 4, flicker::PixelOrder::Rgba, analysisRectangles);
				if (observation.skipped)
					skippedFrames.push_back({{"frame_id", frame}, {"reason", std::move(observation.reason)}});
				else if (!observation.reason.empty())
					events.push_back({{"frame_id", frame}, {"reason", std::move(observation.reason)}});
			}
		}
		if (selectedForSheet)
		{
			const auto cellX = static_cast<unsigned>(selectedIndex % 2) * CellWidth;
			const auto cellY = static_cast<unsigned>(selectedIndex / 2) * CellHeight;
			for (unsigned row = 0; renderSheet && row < CellHeight; ++row)
				std::copy_n(cell.begin() + std::size_t(row) * CellWidth * 4, CellWidth * 4,
					sheet.begin() + (std::size_t(cellY + row) * sheetWidth + cellX) * 4);
			selectedFrames.push_back({{"frame_id", frame}, {"phase", entry.at("phase")}, {"cell", selectedIndex}});
			++selectedIndex;
		}
		if (!directory.empty())
			archivedFrames.push_back(std::move(cell));
	}
	calls.Request(connection, wire::OpCode::ReleaseImageWindow, {wire::Field::U64(tag::ImageWindowId, window, true)});
	Json metadata = {{"ok", true}, {"application_id", request.application}, {"session", std::to_string(opened.session)},
		{"generation", std::to_string(opened.generation)}, {"runtime_process_id", std::to_string(opened.process)},
		{"runtime_epoch", std::to_string(opened.epoch)}, {"transport", "retained_image_window"}, {"target", target},
		{"window", InspectionJson(windowInfo.Find(tag::Document)->value)}, {"selected_frames", std::move(selectedFrames)}, {"columns", 2},
		{"cell_width", CellWidth}, {"cell_height", CellHeight}, {"clear_marker", "amber square"}};
	if (analysis)
	{
		const auto& counts = analysis->Counts();
		const bool detected = !events.empty();
		metadata["analysis"] = {{"mode", "on_demand_window"}, {"frames_analyzed", counts.observations},
			{"frames_skipped", counts.skipped}, {"temporal_pairs", counts.temporalPairs}, {"frame_gaps", counts.gaps},
			{"skipped_frames", std::move(skippedFrames)}, {"events", std::move(events)}, {"detected", detected},
			{"verdict", detected ? "POSSIBLE_FLICKER" : counts.temporalPairs ? "NO_ANOMALY_OBSERVED" : "INCONCLUSIVE"},
			{"limitations", ui ? "Pixel heuristics on cropped RGB8 thumbnails; frames missing either eye are skipped. No composition or continuous-monitor status is measured."
				: "Pixel heuristics on reduced RGB8 thumbnails; no continuous-monitor or layer/paint status is measured."}};
	}
	std::optional<EncodedImage> encoded;
	if (includeImage || !directory.empty())
	{
		ImageEncodingPermit encoder;
		calls.Check();
		encoded = EncodeImage(sheet, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), sheetWidth, sheetHeight, false, 78);
		calls.Check();
		if (!directory.empty())
			SaveIncident(directory, metadata, encoded->bytes, archivedFrames, CellWidth, CellHeight, [&] { calls.Check(); });
	}
	return IncidentCapture{std::move(metadata), includeImage ? std::move(encoded) : std::nullopt};
}
std::optional<Json> CaptureIncident(McpApplication& application, const std::string& name, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		for (auto it = arguments.begin(); it != arguments.end(); ++it)
			if (it.key() != "application_id" && it.key() != "timeout" && it.key() != "max_frames" &&
				it.key() != "directory" && it.key() != "analyze" && it.key() != "include_image")
				throw std::invalid_argument("Unknown argument: " + it.key());
		const auto& selected = arguments.at("application_id");
		if (!selected.is_string() || selected.get_ref<const std::string&>().empty() || selected.get_ref<const std::string&>().size() > 256)
			throw std::invalid_argument("application_id must identify a selected application");
		const auto directory = IncidentDirectory(arguments);
		if (arguments.contains("analyze") && !arguments.at("analyze").is_boolean())
			throw std::invalid_argument("analyze must be boolean");
		const bool analyze = arguments.value("analyze", false);
		if (arguments.contains("include_image") && !arguments.at("include_image").is_boolean())
			throw std::invalid_argument("include_image must be boolean");
		const bool includeImage = arguments.value("include_image", true);
		const auto timeout = arguments.value("timeout", 5.0);
		if (!std::isfinite(timeout) || timeout <= 0 || timeout > 30)
			throw std::invalid_argument("timeout must be greater than zero and at most 30 seconds");
		unsigned maximum = 12;
		if (arguments.contains("max_frames"))
		{
			const auto& value = arguments.at("max_frames");
			if (!value.is_number_integer() || value < 2 || value > 12)
				throw std::invalid_argument("max_frames must be an integer from 2 to 12");
			maximum = value.get<unsigned>();
		}
		const auto captured = CaptureIncident(application,
			{.application = selected.get<std::string>(), .directory = directory, .timeout = timeout,
			 .maximum = maximum, .ui = name == "capture_ui_flicker_window",
			 .analyze = analyze, .includeImage = includeImage}, cancellation);
		if (!captured)
			return {};
		Json content = Json::array({{{"type", "text"}, {"text", captured->metadata.dump()}}});
		if (captured->image)
			content.push_back({{"type", "image"}, {"mimeType", captured->image->mediaType},
				{"data", EncodeBase64(captured->image->bytes)}});
		return Json{{"content", std::move(content)}, {"isError", false}};
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Result({{"ok", false}, {"error", error.what()}}, true);
	}
}
}
