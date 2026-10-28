#include "app/mcp/McpPch.h"
#include "app/mcp/McpScreenshot.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpImage.h"
#include "app/mcp/McpImageEncode.h"
#include "app/mcp/StereoConsistency.h"
#include "app/mcp/McpStereoAnalysis.h"
#include "app/mcp/McpTransientDebug.h"
#include "control/ProtocolTypes.h"
#include "graphics/ColorPixels.h"
#include "math/Math.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

static std::atomic<unsigned> s_screenshots{0};
static std::atomic<unsigned> s_consistency{0};
enum class CaptureMode { Screenshot, Validation, Consistency };
class ScreenshotPermit
{
  public:
	ScreenshotPermit()
	{
		auto count = s_screenshots.load();
		for (;;)
		{
			if (count >= 2)
				throw std::runtime_error("Screenshot capacity is busy; retry explicitly");
			if (s_screenshots.compare_exchange_weak(count, count + 1))
				break;
		}
	}
	~ScreenshotPermit() { --s_screenshots; }
};
Json ScreenshotDefinition()
{
	return {{"name", "capture_screenshot"},
		{"description", "Capture the selected application's current display, or the next projection/quad composition, as standard MCP images. Both eyes return two images from the same frame."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"eye", {{"type", "string"}, {"enum", Json::array({"both", "left", "right"})}}},
			{"layer", {{"type", "string"}, {"enum", Json::array({"all", "projection", "quad"})}}},
			{"format", {{"type", "string"}, {"enum", Json::array({"jpeg", "png"})}}},
			{"max_width", {{"type", "integer"}, {"minimum", 64}, {"maximum", 4096}}},
			{"quality", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}}},
			{"required", Json::array({"application_id"})}, {"additionalProperties", false}}}};
}
Json StereoValidationDefinition()
{
	return {{"name", "validate_stereo"},
		{"description", "Measure central horizontal disparity in the next same-frame projection pair. This scene-dependent heuristic cannot prove IPD or stereo consistency; flat or ambiguous images are inconclusive."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}}},
			{"required", Json::array({"application_id"})}, {"additionalProperties", false}}}};
}
Json StereoConsistencyDefinition()
{
	return {{"name", "analyze_stereo_consistency"},
		{"description", "Capture same-frame projection eyes and analyze mismatched shadows, highlights and depth. Returns a bounded report and optional labeled image; sample multiple frames for a moving scene."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 30}}},
			{"interval", {{"type", "number"}, {"minimum", 0}, {"maximum", 30}}},
			{"threshold", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 4}}},
			{"include_image", {{"type", "boolean"}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}}},
			{"required", Json::array({"application_id"})}, {"additionalProperties", false}}}};
}

static std::optional<StereoFrusta> CaptureFrusta(const Json& description)
{
	try
	{
		const auto& views = description.at("source_views");
		if (!StereoProjectionsMatch(views))
			return {};
		const auto pose = [](const Json& value)
		{
			const auto& position = value.at("position");
			const auto& orientation = value.at("orientation");
			return XrPosef{{orientation.at(0).get<float>(), orientation.at(1).get<float>(),
				orientation.at(2).get<float>(), orientation.at(3).get<float>()},
				{position.at(0).get<float>(), position.at(1).get<float>(), position.at(2).get<float>()}};
		};
		const auto relative = RelativePose(pose(views[1]), pose(views[0]));
		if (std::abs(relative.orientation.x) > .001f || std::abs(relative.orientation.y) > .001f ||
			std::abs(relative.orientation.z) > .001f || std::abs(relative.position.y) > .001f ||
			std::abs(relative.position.z) > .001f || relative.position.x < 0)
			return {};
		const auto& left = views[0].at("fov");
		const auto& right = views[1].at("fov");
		const auto ll = std::tan(left.at(0).get<float>()), lr = std::tan(left.at(1).get<float>());
		const auto rl = std::tan(right.at(0).get<float>()), rr = std::tan(right.at(1).get<float>());
		if (!std::isfinite(ll) || !std::isfinite(lr) || !std::isfinite(rl) || !std::isfinite(rr) ||
			lr - ll <= 1e-6f || rr - rl <= 1e-6f)
			return {};
		const auto infinity = -ll / (lr - ll) - (-rl / (rr - rl));
		if (!std::isfinite(infinity) || std::abs(infinity) > 4)
			return {};
		return StereoFrusta{infinity, 1 / (lr - ll), relative.position.x};
	}
	catch (const Json::exception&)
	{
		return {};
	}
}
static std::string Option(const Json& arguments, const char* name, std::string fallback,
						  std::initializer_list<std::string_view> valid)
{
	const auto found = arguments.find(name);
	if (found == arguments.end())
		return fallback;
	if (!found->is_string())
		throw std::invalid_argument(std::string(name) + " must be a string");
	auto value = found->get<std::string>();
	if (std::find(valid.begin(), valid.end(), value) == valid.end())
		throw std::invalid_argument(std::string("Invalid ") + name);
	return value;
}
static unsigned Integer(const Json& arguments, const char* name, unsigned fallback, unsigned minimum, unsigned maximum)
{
	const auto found = arguments.find(name);
	if (found == arguments.end())
		return fallback;
	if (!found->is_number_integer() || *found < minimum || *found > maximum)
		throw std::invalid_argument(std::string("Invalid ") + name);
	return found->get<unsigned>();
}
static Json Error(const std::string& text)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", Json({{"ok", false}, {"error", text}}).dump()}}})},
		{"isError", true}};
}
static std::optional<Json> Capture(McpApplication& application, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation, CaptureMode mode)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		for (auto it = arguments.begin(); it != arguments.end(); ++it)
			if (it.key() != "application_id" && it.key() != "eye" && it.key() != "layer" && it.key() != "format" &&
				it.key() != "max_width" && it.key() != "quality" && it.key() != "timeout" &&
				!(mode == CaptureMode::Consistency && (it.key() == "threshold" || it.key() == "include_image")))
				throw std::invalid_argument("Unknown argument: " + it.key());
		const auto selected = arguments.find("application_id");
		if (selected == arguments.end() || !selected->is_string() || selected->get_ref<const std::string&>().empty() ||
			selected->get_ref<const std::string&>().size() > 256)
			throw std::invalid_argument("application_id must identify a selected application");
		const auto& target = *selected;
		const auto eye = Option(arguments, "eye", "both", {"both", "left", "right"});
		const auto layer = Option(arguments, "layer", "all", {"all", "projection", "quad"});
		const auto format = Option(arguments, "format", "jpeg", {"jpeg", "png"});
		const auto maxWidth = Integer(arguments, "max_width", 1024, 64, 4096);
		const auto quality = Integer(arguments, "quality", 85, 1, 100);
		float threshold = .16f;
		bool includeImage = true;
		if (mode == CaptureMode::Consistency)
		{
			if (arguments.contains("threshold"))
			{
				if (!arguments.at("threshold").is_number())
					throw std::invalid_argument("threshold must be a number");
				threshold = arguments.at("threshold").get<float>();
				if (!std::isfinite(threshold) || threshold <= 0 || threshold > 4)
					throw std::invalid_argument("threshold must be greater than zero and at most 4");
			}
			if (arguments.contains("include_image"))
			{
				if (!arguments.at("include_image").is_boolean())
					throw std::invalid_argument("include_image must be boolean");
				includeImage = arguments.at("include_image").get<bool>();
			}
		}
		double timeout = 5;
		if (const auto found = arguments.find("timeout"); found != arguments.end())
		{
			if (!found->is_number())
				throw std::invalid_argument("timeout must be a number");
			timeout = found->get<double>();
			if (!std::isfinite(timeout) || timeout <= 0 || timeout > 30)
				throw std::invalid_argument("timeout must be greater than zero and at most 30 seconds");
		}
		if (!cancellation->TryAdmit())
			return {};
		ScreenshotPermit permit;
		TransientDebug calls(application, cancellation, timeout, "Screenshot");
		const auto opened = calls.Data("debug_connect", {{"application_id", target}});
		const auto connection = opened.at("connection").get<std::string>();
		const Json common = {{"connection", connection}};
		Json retained;
		Json composition = nullptr;
		if (layer == "all")
		{
			for (;;)
			{
				try
				{
					retained = calls.Data("debug_retain_preview", common);
					break;
				}
				catch (const TransientError& error)
				{
					if (error.code != static_cast<unsigned>(control::ErrorCode::Busy) &&
						error.code != static_cast<unsigned>(control::ErrorCode::NotFound))
						throw;
					calls.Wait();
				}
			}
			if (!retained.value("image_available", false))
				throw TransientError("The current display is clear; no screenshot is available");
		}
		else
		{
			const auto lease = calls.Data("debug_acquire", common).at("lease").get<std::string>();
			auto renewal = std::chrono::steady_clock::now() + 1s;
			const auto maintainLease = [&]
			{
				if (std::chrono::steady_clock::now() >= renewal)
				{
					calls.Data("debug_renew", {{"connection", connection}, {"lease", lease}});
					renewal = std::chrono::steady_clock::now() + 1s;
				}
			};
			const auto submitted = calls.Data("debug_start_image_recording", {{"connection", connection}, {"lease", lease},
				{"command_id", "1"}, {"target", layer == "projection" ? "projection_layers" : "quad_layers"},
				{"capacity", 1}, {"frames", "1"}});
			const auto operation = submitted.at("operation_id").get<std::string>();
			Json operationState;
			for (;;)
			{
				maintainLease();
				operationState = calls.Data("debug_operation", {{"connection", connection}, {"operation", operation}});
				const auto phase = operationState.at("phase").get<std::string>();
				if (phase != "queued" && phase != "pending")
				{
					if (phase != "complete" || operationState.value("progress", "") != "recording_started")
						throw TransientError("Filtered capture could not start: " + operationState.value("progress", phase));
					break;
				}
				calls.Wait();
			}
			const auto& captures = operationState.at("captures");
			if (!captures.is_array() || captures.size() != 1 || !captures[0].is_string())
				throw TransientError("Filtered capture did not reserve exactly one frame");
			const auto capture = captures[0].get<std::string>();
			for (;;)
			{
				maintainLease();
				const auto status = calls.Data("debug_capture", {{"connection", connection}, {"capture", capture}});
				const auto phase = status.at("phase").get<std::string>();
				if (phase == "ready")
					break;
				if (phase != "reserved" && phase != "issued" && phase != "begun" && phase != "gpu_pending")
					throw TransientError("Filtered capture is unavailable: " + phase);
				calls.Wait();
			}
			const auto inspection = calls.Data("debug_inspect", {{"connection", connection}, {"capture", capture}});
			const auto revision = inspection.at("document").at("view_settings").get<std::string>();
			const auto views = calls.Data("debug_revision", {{"connection", connection}, {"revision", revision}});
			if (views.at("kind") != "view_settings_v1")
				throw TransientError("Captured view settings have an unexpected format");
			const auto& settings = views.at("document");
			composition = {{"frame", inspection.at("frame")}, {"ipd", settings.at("ipd")}, {"fovs", settings.at("outer_fov")}};
			retained = calls.Data("debug_retain_image", {{"connection", connection}, {"capture", capture}});
			calls.Data("debug_relinquish", {{"connection", connection}, {"lease", lease}});
		}
		const auto image = retained.at("image").get<std::string>();
		const auto info = calls.Data("debug_image_info", {{"connection", connection}, {"image", image}});
		const auto frame = info.at("frame");
		if ((!composition.is_null() && composition.at("frame") != frame) ||
			(retained.contains("frame") && retained.at("frame") != frame))
			throw TransientError("Screenshot image and composition belong to different frames");
		const auto& description = info.at("image_description");
		const auto width = description.at("width").get<std::uint32_t>();
		const auto height = description.at("height").get<std::uint32_t>();
		const auto layers = description.at("array_layers").get<std::uint32_t>();
		const auto sourceFormat = description.at("format").get<std::uint32_t>();
		const auto pixelSize = ColorPixelBytes(sourceFormat);
		if (!width || !height || layers < 2 || !pixelSize)
			throw TransientError("Screenshot requires a supported stereo image");
		const auto scale = std::min({1.0, double(maxWidth) / width,
			(mode == CaptureMode::Consistency ? 480.0 : mode == CaptureMode::Validation ? 640.0 : 4096.0) / height,
			std::sqrt(double(32ULL * 1024 * 1024) / (double(width) * height * pixelSize))});
		const auto outputWidth = std::max(1u, static_cast<unsigned>(std::floor(width * scale)));
		const auto outputHeight = std::max(1u, static_cast<unsigned>(std::floor(height * scale)));
		const bool resized = outputWidth != width || outputHeight != height;
		Json images = Json::array(), eyes = Json::array();
		std::array<std::vector<std::uint8_t>, 2> decoded;
		std::size_t encodedBytes = 0;
		for (unsigned index = 0; index < 2; ++index)
		{
			if ((eye == "left" && index != 0) || (eye == "right" && index != 1))
				continue;
			Json request = {{"connection", connection}, {"image", image}, {"layer", index}};
			if (resized)
				request["region"] = {{"x", 0}, {"y", 0}, {"width", width}, {"height", height},
					{"output_width", outputWidth}, {"output_height", outputHeight}};
			const auto exported = calls.Data("debug_start_export", request);
			const auto ticket = exported.at("export").get<std::string>();
			Json exportStatus;
			for (;;)
			{
				exportStatus = calls.Data("debug_export_info", {{"connection", connection}, {"export", ticket}});
				const auto phase = exportStatus.at("phase").get<std::string>();
				if (phase == "complete")
					break;
				if (phase != "pending")
					throw TransientError("Screenshot image export failed: " + phase);
				calls.Wait();
			}
			if (mode != CaptureMode::Screenshot)
			{
				std::uint64_t ticketValue = 0;
				const auto parsed = std::from_chars(ticket.data(), ticket.data() + ticket.size(), ticketValue);
				if (parsed.ec != std::errc{} || parsed.ptr != ticket.data() + ticket.size() || !ticketValue)
					throw TransientError("Invalid screenshot export identifier");
				const auto& output = exportStatus.at("image_description");
				const auto outputFormat = output.at("format").get<std::uint32_t>();
				const auto outputPixelSize = ColorPixelBytes(outputFormat);
				if (output.at("width") != outputWidth || output.at("height") != outputHeight ||
					output.at("array_layers") != 1 || !outputPixelSize)
					throw TransientError("Stereo export has unexpected dimensions or format");
				const auto total = std::uint64_t{outputWidth} * outputHeight * outputPixelSize;
				if (exportStatus.at("total_bytes") != std::to_string(total))
					throw TransientError("Stereo export has an unexpected byte count");
				std::vector<std::byte> pixels;
				pixels.reserve(static_cast<std::size_t>(total));
				while (pixels.size() < total)
				{
					const auto length = std::min<std::uint64_t>(control::MaxPayload - 4096, total - pixels.size());
					const auto bytes = calls.Read(connection, ticketValue, pixels.size(), length);
					pixels.insert(pixels.end(), bytes.begin(), bytes.end());
				}
				auto& sample = decoded[index];
				const auto pixelCount = std::size_t{outputWidth} * outputHeight;
				sample.resize(pixelCount * (mode == CaptureMode::Consistency ? 3 : 1));
				for (std::size_t position = 0; position < pixelCount; ++position)
				{
					if (position % outputWidth == 0)
						calls.Check();
					const auto color = DecodeColorPixel(outputFormat, std::span(pixels).subspan(position * outputPixelSize, outputPixelSize));
					const auto channel = [](float value) { return std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f); };
					if (mode == CaptureMode::Validation)
						sample[position] = static_cast<std::uint8_t>(std::lround(255 * (0.299f * channel(color[0]) +
							0.587f * channel(color[1]) + 0.114f * channel(color[2]))));
					else
						for (std::size_t component = 0; component < 3; ++component)
							sample[position * 3 + component] = static_cast<std::uint8_t>(std::lround(255 * channel(color[component])));
				}
				calls.Data("debug_release_export", {{"connection", connection}, {"export", ticket}});
				continue;
			}
			const auto rendered = calls.Call("debug_render_export", {{"connection", connection}, {"export", ticket},
				{"format", format}, {"quality", quality}});
			const auto& content = rendered.at("content");
			if (!content.is_array() || content.size() != 2 || content[1].value("type", "") != "image")
				throw TransientError("Image encoder returned an invalid screenshot");
			const auto encoded = content[1].at("data").get_ref<const std::string&>().size();
			if (encoded > 12 * 1024 * 1024 - encodedBytes)
				throw TransientError("Screenshot exceeds the combined response size limit");
			encodedBytes += encoded;
			const auto imageInfo = Json::parse(content[0].at("text").get<std::string>());
			eyes.push_back({{"eye", index == 0 ? "left" : "right"}, {"width", outputWidth}, {"height", outputHeight},
				{"lossy", imageInfo.at("lossy")}});
			images.push_back(content[1]);
			calls.Data("debug_release_export", {{"connection", connection}, {"export", ticket}});
		}
		calls.Data("debug_release_image", {{"connection", connection}, {"image", image}});
		if (mode == CaptureMode::Validation)
		{
			auto result = AnalyzeStereo(decoded[0], decoded[1], outputWidth, outputHeight, width, [&] { calls.Check(); });
			result["ok"] = true;
			result["application_id"] = target;
			result["session"] = opened.at("session");
			result["frame"] = frame;
			result["composition"] = std::move(composition);
			return Json{{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}, {"isError", false}};
		}
		if (mode == CaptureMode::Consistency)
		{
			const auto frusta = CaptureFrusta(description);
			StereoConsistencyReport report;
			if (frusta)
				report = AnalyzeStereoConsistency(decoded[0], decoded[1], outputWidth, outputHeight, height,
					threshold, frusta, includeImage, [&] { calls.Check(); });
			else
				report.document = {{"verdict", "INCONCLUSIVE_UNSUPPORTED_GEOMETRY"}, {"score", 0},
					{"regions", Json::array()}, {"infinity_disparity_pct", nullptr},
					{"notes", Json::array({"Captured eye views do not support the stereo correspondence model."})}};
			auto& document = report.document;
			document["ok"] = true;
			document["application_id"] = target;
			document["session"] = opened.at("session");
			document["runtime_process_id"] = opened.at("runtime_process_id");
			document["runtime_epoch"] = opened.at("runtime_epoch");
			document["frame"] = frame;
			document["image_views"] = description.at("source_views");
			document["frusta_source"] = frusta ? "captured_image_views" : "unavailable";
			document["analysis_width"] = outputWidth;
			document["analysis_height"] = outputHeight;
			document["resized"] = resized;
			document["analysis_source_format"] = sourceFormat;
			document["analysis_pixels"] = "RGB8 clamped; nearest-neighbour GPU reduction when resized";
			Json content = Json::array({{{"type", "text"}, {"text", document.dump()}}});
			if (!report.sheet.empty())
			{
				ImageEncodingPermit encoder;
				calls.Check();
				const auto bytes = std::as_bytes(std::span(report.sheet));
				const auto encoded = EncodeImage(bytes, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm),
					report.sheetWidth, report.sheetHeight, false, 80);
				calls.Check();
				content.push_back({{"type", "image"}, {"mimeType", encoded.mediaType}, {"data", EncodeBase64(encoded.bytes)}});
			}
			return Json{{"content", std::move(content)}, {"isError", false}};
		}
		Json metadata = {{"ok", true}, {"application_id", target}, {"session", opened.at("session")},
			{"frame", frame}, {"layer", layer}, {"eye", eye}, {"format", format}, {"resized", resized},
			{"source_width", width}, {"source_height", height}, {"eyes", std::move(eyes)}};
		if (!composition.is_null())
			metadata["composition"] = std::move(composition);
		if (retained.contains("display"))
			metadata["display"] = retained.at("display");
		if (retained.contains("document"))
			metadata["tracking"] = retained.at("document");
		Json content = Json::array({{{"type", "text"}, {"text", metadata.dump()}}});
		for (auto& value : images)
			content.push_back(std::move(value));
		return Json{{"content", std::move(content)}, {"isError", false}};
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Error(error.what());
	}
}

std::optional<Json> CaptureScreenshot(McpApplication& application, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	return Capture(application, arguments, cancellation, CaptureMode::Screenshot);
}

std::optional<Json> ValidateStereo(McpApplication& application, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	if (!arguments.is_object())
		return Error("Arguments must be an object");
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (it.key() != "application_id" && it.key() != "timeout")
			return Error("Unknown argument: " + it.key());
	Json request = arguments;
	request["eye"] = "both";
	request["layer"] = "projection";
	request["max_width"] = 640;
	return Capture(application, request, cancellation, CaptureMode::Validation);
}

std::optional<Json> CaptureStereoConsistency(McpApplication& application, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		for (auto it = arguments.begin(); it != arguments.end(); ++it)
			if (it.key() != "application_id" && it.key() != "frames" && it.key() != "interval" &&
				it.key() != "threshold" && it.key() != "include_image" && it.key() != "timeout")
				throw std::invalid_argument("Unknown argument: " + it.key());
		const auto selected = arguments.find("application_id");
		if (selected == arguments.end() || !selected->is_string() || selected->get_ref<const std::string&>().empty() ||
			selected->get_ref<const std::string&>().size() > 256)
			throw std::invalid_argument("application_id must identify a selected application");
		if (arguments.contains("threshold"))
		{
			if (!arguments.at("threshold").is_number())
				throw std::invalid_argument("threshold must be a number");
			const auto value = arguments.at("threshold").get<double>();
			if (!std::isfinite(value) || value <= 0 || value > 4)
				throw std::invalid_argument("threshold must be greater than zero and at most 4");
		}
		if (arguments.contains("include_image") && !arguments.at("include_image").is_boolean())
			throw std::invalid_argument("include_image must be boolean");
		const auto frames = Integer(arguments, "frames", 1, 1, 30);
		double interval = 1, timeout = 5;
		for (const auto& [key, target] : {std::pair{"interval", &interval}, {"timeout", &timeout}})
		{
			if (!arguments.contains(key))
				continue;
			if (!arguments.at(key).is_number())
				throw std::invalid_argument(std::string(key) + " must be a number");
			*target = arguments.at(key).get<double>();
			if (!std::isfinite(*target) || *target < 0 || *target > 30 || (std::string_view(key) == "timeout" && !*target))
				throw std::invalid_argument(std::string(key) + " is out of range");
		}
		if (frames * (interval + timeout) > 300)
			throw std::invalid_argument("The requested sampling window exceeds five minutes");
		if (!cancellation->TryAdmit())
			return {};
		unsigned expected = 0;
		if (!s_consistency.compare_exchange_strong(expected, 1))
			throw std::runtime_error("Stereo consistency analysis is busy; retry explicitly");
		struct Permit
		{
			~Permit() { s_consistency.store(0); }
		} permit;
		Json request = arguments;
		request.erase("frames");
		request.erase("interval");
		request["eye"] = "both";
		request["layer"] = "projection";
		request["max_width"] = 480;
		Json samples = Json::array();
		std::optional<Json> worst, image;
		double worstScore = -1;
		std::string session, generation, process, epoch;
		bool incomplete = false;
		for (unsigned index = 0; index < frames; ++index)
		{
			const auto started = std::chrono::steady_clock::now();
			const auto result = Capture(application, request, cancellation, CaptureMode::Consistency);
			if (!result || cancellation->Requested())
				return {};
			const auto& content = result->at("content");
			auto document = Json::parse(content.at(0).at("text").get<std::string>());
			if (result->value("isError", true))
			{
				incomplete = true;
				samples.push_back({{"index", index}, {"ok", false}, {"error", document.value("error", "Capture failed")}});
			}
			else
			{
				const auto currentSession = document.at("session").get<std::string>();
				const auto currentGeneration = document.at("frame").at("generation").get<std::string>();
				const auto currentProcess = document.at("runtime_process_id").get<std::string>();
				const auto currentEpoch = document.at("runtime_epoch").get<std::string>();
				if (!session.empty() && (session != currentSession || generation != currentGeneration ||
					process != currentProcess || epoch != currentEpoch))
					throw std::runtime_error("The selected XR session changed during stereo sampling");
				session = currentSession;
				generation = currentGeneration;
				process = currentProcess;
				epoch = currentEpoch;
				document["index"] = index;
				samples.push_back(document);
				const auto score = document.value("score", 0.0);
				if (!worst || score > worstScore)
				{
					worst = std::move(document);
					worstScore = score;
					image = content.size() > 1 ? std::optional<Json>(content[1]) : std::nullopt;
				}
			}
			if (index + 1 < frames)
			{
				const auto next = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<double>(interval));
				while (std::chrono::steady_clock::now() < next)
				{
					if (cancellation->Requested())
						return {};
					std::this_thread::sleep_for(std::min(10ms,
						std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now())));
				}
			}
		}
		if (!worst)
			return Error("No stereo frame could be analyzed: " + samples.dump());
		Json document = {{"ok", true}, {"incomplete", incomplete}, {"samples", std::move(samples)}, {"worst", *worst}};
		Json content = Json::array({{{"type", "text"}, {"text", document.dump()}}});
		if (image)
			content.push_back(std::move(*image));
		return Json{{"content", std::move(content)}, {"isError", false}};
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Error(error.what());
	}
}
} // namespace simxr::app::mcp
