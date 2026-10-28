#include "Pch.h"
#include "app/mcp/McpIncident.h"
#include "app/mcp/McpIncidentAnalysis.h"
#include "app/mcp/McpIncidentSave.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpImage.h"
#include "app/mcp/McpImageEncode.h"
#include "app/mcp/McpTransientDebug.h"
#include "control/ProtocolTypes.h"
#include "control/ControlSchema.h"
#include "graphics/ColorPixels.h"

#include <atomic>
#include <cmath>

namespace simxr::app::mcp
{
using Json = nlohmann::json;
static constexpr unsigned EyeWidth = 160, EyeHeight = 90, Header = 14;
static constexpr unsigned CellWidth = EyeWidth * 2, CellHeight = EyeHeight + Header;
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
static void RenderEye(TransientDebug& calls, const std::string& connection, const std::string& image,
	unsigned eye, Rectangle rectangle, std::vector<std::byte>& sheet, unsigned sheetWidth, unsigned cellX, unsigned cellY)
{
	if (!rectangle.width)
		return;
	const auto scale = std::min({1.0, double(EyeWidth) / rectangle.width, double(EyeHeight) / rectangle.height});
	const auto width = std::max(1u, static_cast<unsigned>(std::floor(rectangle.width * scale)));
	const auto height = std::max(1u, static_cast<unsigned>(std::floor(rectangle.height * scale)));
	const auto started = calls.Data("debug_start_export", {{"connection", connection}, {"image", image}, {"layer", eye},
		{"region", {{"x", rectangle.x}, {"y", rectangle.y}, {"width", rectangle.width}, {"height", rectangle.height},
			{"output_width", width}, {"output_height", height}}}});
	const auto ticket = started.at("export").get<std::string>();
	const auto ticketId = IncidentIdentifier(ticket, "export identifier");
	Json status;
	for (;;)
	{
		status = calls.Data("debug_export_info", {{"connection", connection}, {"export", ticket}});
		const auto phase = status.at("phase").get<std::string>();
		if (phase == "complete")
			break;
		if (phase != "pending")
			throw std::runtime_error("Incident frame export failed: " + phase);
		calls.Wait();
	}
	const auto& description = status.at("image_description");
	const auto format = description.at("format").get<std::uint32_t>();
	const auto pixelSize = ColorPixelBytes(format);
	const auto total = std::uint64_t{width} * height * pixelSize;
	if (!pixelSize || description.at("width") != width || description.at("height") != height ||
		description.at("array_layers") != 1 || status.at("total_bytes") != std::to_string(total))
		throw std::runtime_error("Incident export has an unexpected shape or format");
	std::vector<std::byte> pixels;
	pixels.reserve(static_cast<std::size_t>(total));
	while (pixels.size() < total)
	{
		const auto length = std::min<std::uint64_t>(control::MaxPayload - 4096, total - pixels.size());
		const auto bytes = calls.Read(connection, ticketId, pixels.size(), length);
		pixels.insert(pixels.end(), bytes.begin(), bytes.end());
	}
	const auto left = cellX + eye * EyeWidth + (EyeWidth - width) / 2;
	const auto top = cellY + Header + (EyeHeight - height) / 2;
	for (unsigned y = 0; y < height; ++y)
	{
		calls.Check();
		for (unsigned x = 0; x < width; ++x)
		{
			const auto color = DecodeColorPixel(format, std::span(pixels).subspan((std::size_t{y} * width + x) * pixelSize, pixelSize));
			const auto offset = (std::size_t{top + y} * sheetWidth + left + x) * 4;
			for (unsigned channel = 0; channel < 3; ++channel)
			{
				const auto value = std::isnan(color[channel]) ? 0.0f : std::clamp(color[channel], 0.0f, 1.0f);
				sheet[offset + channel] = std::byte(static_cast<unsigned char>(std::lround(value * 255.0f)));
			}
		}
	}
	calls.Data("debug_release_export", {{"connection", connection}, {"export", ticket}});
}
std::optional<Json> CaptureIncident(McpApplication& application, const std::string& name, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation, bool allowMissing)
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
		if (!cancellation->TryAdmit())
			return {};
		IncidentPermit permit;
		TransientDebug calls(application, cancellation, timeout, "Incident capture");
		const auto opened = calls.Data("debug_connect", {{"application_id", selected}});
		const auto connection = opened.at("connection").get<std::string>();
		const bool ui = name == "capture_ui_flicker_window";
		const auto target = ui ? "quad_layers" : "composed_eyes";
		const auto started = calls.Data("debug_begin_image_window", {{"connection", connection}, {"target", target}});
		const auto window = started.at("window").get<std::string>();
		Json document;
		for (;;)
		{
			document = calls.Data("debug_image_window", {{"connection", connection}, {"window", window}}).at("document");
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
		if (!entries.is_array() || entries.size() < 2 || entries.size() > 24 || document.at("generation") != opened.at("generation") ||
			document.at("target") != target || document.at("pre_count") == 0 || document.at("post_count") != 12)
			throw std::runtime_error("Incident window has incomplete or mismatched frame history");
		for (const auto& entry : entries)
			if (!allowMissing && entry.at("phase") != "ready" && entry.at("phase") != "clear")
				throw std::runtime_error("Incident contains unavailable frame " + entry.at("frame_id").get<std::string>());
		const auto count = std::min<std::size_t>(maximum, entries.size());
		const auto sheetWidth = CellWidth * 2;
		const auto sheetHeight = CellHeight * static_cast<unsigned>((count + 1) / 2);
		std::vector<std::byte> sheet(std::size_t{sheetWidth} * sheetHeight * 4, std::byte{0});
		for (std::size_t index = 3; index < sheet.size(); index += 4)
			sheet[index] = std::byte{0xff};
		Json selectedFrames = Json::array();
		std::vector<std::vector<std::byte>> archivedFrames;
		std::optional<IncidentAnalysis> analysis;
		if (analyze)
			analysis.emplace(ui);
		if (!directory.empty())
			archivedFrames.reserve(entries.size());
		const auto iterations = directory.empty() && !analysis ? count : entries.size();
		std::size_t selectedIndex = 0;
		for (std::size_t index = 0; index < iterations; ++index)
		{
			calls.Check();
			const auto sourceIndex = directory.empty() && !analysis ? index * (entries.size() - 1) / (count - 1) : index;
			const bool selectedForSheet = selectedIndex < count &&
				sourceIndex == selectedIndex * (entries.size() - 1) / (count - 1);
			const auto& entry = entries.at(sourceIndex);
			const auto frame = entry.at("frame_id").get<std::string>();
			(void)IncidentIdentifier(frame, "frame identifier");
			const bool clear = entry.at("phase") == "clear";
			const bool missing = entry.at("phase") != "ready" && !clear;
			std::vector<std::byte> cell(std::size_t{CellWidth} * CellHeight * 4, std::byte{0});
			for (std::size_t alpha = 3; alpha < cell.size(); alpha += 4)
				cell[alpha] = std::byte{0xff};
			Label(cell, CellWidth, 4, 4, frame, clear);
			std::array<std::array<std::int32_t, 4>, 2> analysisRectangles{};
			if (!clear && !missing)
			{
				const auto retained = calls.Data("debug_retain_window_image", {{"connection", connection}, {"window", window}, {"frame", frame}});
				const auto image = retained.at("image").get<std::string>();
				const auto info = calls.Data("debug_image_info", {{"connection", connection}, {"image", image}});
				if (info.at("frame").at("frame_id") != frame || info.at("frame").at("generation") != opened.at("generation") ||
					info.at("target").at("kind") != static_cast<unsigned>(ui ? control::TargetKind::QuadLayers : control::TargetKind::ComposedEyes))
					throw std::runtime_error("Incident image does not match its window frame or target");
				const auto& description = info.at("image_description");
				const auto width = description.at("width").get<std::uint32_t>();
				const auto height = description.at("height").get<std::uint32_t>();
				if (!width || !height || description.at("array_layers").get<std::uint32_t>() < 2)
					throw std::runtime_error("Incident image is not stereo");
				bool visible = false;
				for (unsigned eye = 0; eye < 2; ++eye)
				{
					const auto rectangle = EyeRectangle(entry, eye, width, height, ui);
					visible |= rectangle.width != 0;
					if (rectangle.width)
					{
						const auto scale = std::min({1.0, double(EyeWidth) / rectangle.width, double(EyeHeight) / rectangle.height});
						const auto w = std::max(1u, static_cast<unsigned>(std::floor(rectangle.width * scale)));
						const auto h = std::max(1u, static_cast<unsigned>(std::floor(rectangle.height * scale)));
						analysisRectangles[eye] = {static_cast<std::int32_t>(eye * EyeWidth + (EyeWidth - w) / 2),
							static_cast<std::int32_t>((EyeHeight - h) / 2), static_cast<std::int32_t>(w), static_cast<std::int32_t>(h)};
					}
					RenderEye(calls, connection, image, eye, rectangle, cell, CellWidth, 0, 0);
				}
				if (!visible)
					throw std::runtime_error("Ready UI frame has no visible quad rectangle");
				calls.Data("debug_release_image", {{"connection", connection}, {"image", image}});
			}
			if (analysis)
			{
				const auto frameId = IncidentIdentifier(frame, "frame identifier");
				if (missing)
					analysis->Skip(frameId);
				else
					analysis->Observe(frameId, cell, analysisRectangles);
			}
			if (selectedForSheet)
			{
				const auto cellX = static_cast<unsigned>(selectedIndex % 2) * CellWidth;
				const auto cellY = static_cast<unsigned>(selectedIndex / 2) * CellHeight;
				for (unsigned row = 0; row < CellHeight; ++row)
					std::copy_n(cell.begin() + std::size_t(row) * CellWidth * 4, CellWidth * 4,
						sheet.begin() + (std::size_t(cellY + row) * sheetWidth + cellX) * 4);
				selectedFrames.push_back({{"frame_id", frame}, {"phase", entry.at("phase")}, {"cell", selectedIndex}});
				++selectedIndex;
			}
			if (!directory.empty())
				archivedFrames.push_back(std::move(cell));
		}
		calls.Data("debug_release_image_window", {{"connection", connection}, {"window", window}});
		Json metadata = {{"ok", true}, {"application_id", selected}, {"session", opened.at("session")},
			{"generation", opened.at("generation")}, {"runtime_process_id", opened.at("runtime_process_id")},
			{"runtime_epoch", opened.at("runtime_epoch")}, {"transport", "retained_image_window"}, {"target", target},
			{"window", document}, {"selected_frames", std::move(selectedFrames)}, {"columns", 2},
			{"cell_width", CellWidth}, {"cell_height", CellHeight}, {"clear_marker", "amber square"}};
		if (analysis)
			metadata["analysis"] = analysis->Report();
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
		Json content = Json::array({{{"type", "text"}, {"text", metadata.dump()}}});
		if (includeImage)
			content.push_back({{"type", "image"}, {"mimeType", encoded->mediaType}, {"data", EncodeBase64(encoded->bytes)}});
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
