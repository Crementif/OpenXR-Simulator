#include "app/mcp/McpPch.h"
#include "app/mcp/McpScreenshot.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpImage.h"
#include "app/services/ImageExport.h"
#include "app/mcp/StereoConsistency.h"
#include "app/mcp/McpStereoAnalysis.h"
#include "app/mcp/McpTransientDebug.h"
#include "app/mcp/McpSimulation.h"
#include "app/mcp/McpImageCache.h"
#include "app/services/RuntimePanelProtocol.h"
#include "control/DebugProtocol.h"
#include "control/ImageInspection.h"
#include "control/ExportReader.h"
#include "control/ProtocolTypes.h"
#include "graphics/ColorPixels.h"
#include "math/Math.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;

static std::uint64_t Identifier(const wire::Message& message, std::uint16_t tag)
{
	const auto value = panel::U64(message, tag);
	if (!value)
		throw TransientError("Runtime omitted a capture identifier");
	return value;
}
static FrameKey CaptureFrame(const wire::Message& message)
{
	const auto frame = panel::Nested(message.Find(tag::Frame));
	return {{Identifier(frame, tag::Generation)}, {Identifier(frame, tag::FrameId)}};
}
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
struct CaptureResult
{
	Json document;
	Json images = Json::array();
	std::array<std::uint64_t, 4> scope{};
};
static std::optional<Json> CaptureReply(std::optional<CaptureResult> result)
{
	if (!result)
		return {};
	Json content = Json::array({{{"type", "text"}, {"text", result->document.dump()}}});
	for (auto& image : result->images)
		content.push_back(std::move(image));
	return Json{{"content", std::move(content)}, {"isError", !result->document.value("ok", false)}};
}
static std::optional<CaptureResult> Capture(McpApplication& application, const Json& arguments,
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
		const auto opened = calls.Connect(target.get_ref<const std::string&>());
		const auto connection = opened.id;
		wire::Message retained;
		std::optional<FrameKey> compositionFrame;
		Json composition = nullptr;
		if (layer == "all")
		{
			for (;;)
			{
				try
				{
					retained = calls.Request(connection, wire::OpCode::RetainPreview, {});
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
			if (!panel::Boolean(retained, tag::ImageAvailable))
				throw TransientError("The current display is clear; no screenshot is available");
		}
		else
		{
			const auto lease = Identifier(calls.Request(connection, wire::OpCode::Acquire, {}), tag::Lease);
			auto renewal = std::chrono::steady_clock::now() + 1s;
			const auto maintainLease = [&]
			{
				if (std::chrono::steady_clock::now() >= renewal)
				{
					calls.Request(connection, wire::OpCode::Renew, {wire::Field::U64(tag::Lease, lease, true)});
					renewal = std::chrono::steady_clock::now() + 1s;
				}
			};
			const auto submitted = calls.Request(connection, wire::OpCode::Submit,
				{wire::Field::U64(tag::Lease, lease, true), wire::Field::U64(tag::CommandId, 1, true),
				 wire::Field::U32(tag::CommandKind, static_cast<std::uint32_t>(wire::CommandKind::StartImageRecording), true),
				 wire::EncodeCaptureTarget({layer == "projection" ? CaptureKind::ProjectionLayers : CaptureKind::QuadLayers, 0, 0}),
				 wire::Field::U32(tag::Capacity, 1, true), wire::Field::U64(tag::Length, 1, true)});
			const auto operation = Identifier(submitted, tag::OperationId);
			wire::Message operationState;
			for (;;)
			{
				maintainLease();
				operationState = calls.Request(connection, wire::OpCode::Operation, {wire::Field::U64(tag::OperationId, operation, true)});
				const auto phase = panel::Text(operationState, tag::Phase);
				if (phase != "queued" && phase != "pending")
				{
					if (phase != "complete" || panel::Text(operationState, tag::Progress) != "recording_started")
						throw TransientError("Filtered capture could not start: " + panel::Text(operationState, tag::Progress, phase.c_str()));
					break;
				}
				calls.Wait();
			}
			const auto captures = panel::Nested(operationState.Find(tag::Captures));
			if (captures.fields.size() != 1 || !captures.fields[0].AsU64() || !*captures.fields[0].AsU64())
				throw TransientError("Filtered capture did not reserve exactly one frame");
			const auto capture = *captures.fields[0].AsU64();
			for (;;)
			{
				maintainLease();
				const auto status = calls.Request(connection, wire::OpCode::Capture, {wire::Field::U64(tag::CaptureId, capture, true)});
				const auto phase = panel::Text(status, tag::Phase);
				if (phase == "ready")
					break;
				if (phase != "reserved" && phase != "issued" && phase != "begun" && phase != "gpu_pending")
					throw TransientError("Filtered capture is unavailable: " + phase);
				calls.Wait();
			}
			const auto inspection = calls.Request(connection, wire::OpCode::InspectFrame, {wire::Field::U64(tag::CaptureId, capture, true)});
			if (Identifier(inspection, tag::CaptureId) != capture || !panel::U64(inspection, tag::InspectionId) || !inspection.Find(tag::Document))
				throw TransientError("Runtime returned an invalid frame inspection");
			compositionFrame = CaptureFrame(inspection);
			const auto document = InspectionJson(inspection.Find(tag::Document)->value, true);
			const auto& revision = document.at("view_settings");
			if (!revision.is_number_unsigned() || revision == 0)
				throw TransientError("Runtime omitted the captured view settings revision");
			const auto revisionId = revision.get<std::uint64_t>();
			const auto views = calls.Request(connection, wire::OpCode::FeatureRevision, {wire::Field::U64(tag::RevisionId, revisionId, true)});
			if (Identifier(views, tag::RevisionId) != revisionId || panel::Text(views, tag::RevisionKind) != "view_settings_v1" || !views.Find(tag::Document))
				throw TransientError("Captured view settings have an unexpected identity or format");
			const auto settings = InspectionJson(views.Find(tag::Document)->value);
			composition = {{"ipd", settings.at("ipd")}, {"fovs", settings.at("outer_fov")}};
			retained = calls.Request(connection, wire::OpCode::RetainImage, {wire::Field::U64(tag::CaptureId, capture, true)});
			calls.Request(connection, wire::OpCode::Relinquish, {wire::Field::U64(tag::Lease, lease, true)});
		}
		const auto image = Identifier(retained, tag::ImageReaderId);
		const auto info = calls.Request(connection, wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, image, true)});
		const auto frameKey = CaptureFrame(info);
		if (frameKey.generation.value != opened.generation || (compositionFrame && *compositionFrame != frameKey) ||
			(retained.Find(tag::Frame) && CaptureFrame(retained) != frameKey))
			throw TransientError("Screenshot image and composition belong to different frames");
		const Json frame = {{"generation", std::to_string(frameKey.generation.value)}, {"frame_id", std::to_string(frameKey.frame.value)}};
		if (compositionFrame)
			composition["frame"] = frame;
		const auto* descriptor = info.Find(tag::ImageDescriptor);
		if (!descriptor)
			throw TransientError("Runtime omitted the screenshot image description");
		const auto source = wire::DecodeImageDescription(*descriptor);
		const auto width = source.width, height = source.height;
		const auto sourceFormat = source.format;
		const auto pixelSize = ColorPixelBytes(sourceFormat);
		if (!width || !height || source.arrayLayers < 2 || !pixelSize)
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
			std::vector<wire::Field> fields{wire::Field::U64(tag::ImageReaderId, image, true), wire::Field::U32(tag::Layer, index)};
			if (resized)
				fields.push_back(wire::EncodeImageRegion({0, 0, width, height, outputWidth, outputHeight}, true));
			control::ExportReader exported(
				[&](auto opcode, auto fields) { return calls.Request(connection, opcode, std::move(fields)); },
				std::move(fields), [&] { calls.Wait(); });
			const auto& exportStatus = exported.Info();
			if (mode != CaptureMode::Screenshot)
			{
				const auto* outputDescriptor = exportStatus.Find(tag::ImageDescriptor);
				if (!outputDescriptor)
					throw TransientError("Runtime omitted the stereo export description");
				const auto output = wire::DecodeImageDescription(*outputDescriptor);
				const auto outputFormat = output.format;
				const auto outputPixelSize = ColorPixelBytes(outputFormat);
				if (output.width != outputWidth || output.height != outputHeight || output.arrayLayers != 1 || !outputPixelSize)
					throw TransientError("Stereo export has unexpected dimensions or format");
				const auto total = std::uint64_t{outputWidth} * outputHeight * outputPixelSize;
				if (total > MaxImageInput || panel::U64(exportStatus, tag::TotalBytes) != total)
					throw TransientError("Stereo export has an unexpected byte count");
				std::vector<std::byte> pixels;
				pixels.reserve(static_cast<std::size_t>(total));
				exported.Read([&](std::span<const std::byte> bytes)
				{
					pixels.insert(pixels.end(), bytes.begin(), bytes.end());
				});
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
				exported.Release();
				continue;
			}
			const auto rendered = calls.Render(connection, {exported.Id(), format == "png", static_cast<int>(quality)});
			const auto encoded = rendered->data.size();
			if (encoded > 12 * 1024 * 1024 - encodedBytes)
				throw TransientError("Screenshot exceeds the combined response size limit");
			encodedBytes += encoded;
			eyes.push_back({{"eye", index == 0 ? "left" : "right"}, {"width", outputWidth}, {"height", outputHeight},
				{"lossy", rendered->lossy}});
			images.push_back({{"type", "image"}, {"mimeType", rendered->mediaType}, {"data", rendered->data}});
			exported.Release();
		}
		calls.Request(connection, wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, image, true)});
		if (mode == CaptureMode::Validation)
		{
			auto result = AnalyzeStereo(decoded[0], decoded[1], outputWidth, outputHeight, width, [&] { calls.Check(); });
			result["ok"] = true;
			result["application_id"] = target;
			result["session"] = std::to_string(opened.session);
			result["frame"] = frame;
			result["composition"] = std::move(composition);
			return CaptureResult{std::move(result)};
		}
		if (mode == CaptureMode::Consistency)
		{
			const auto description = ImageDocument(info).at("image_description");
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
			document["session"] = std::to_string(opened.session);
			document["runtime_process_id"] = std::to_string(opened.process);
			document["runtime_epoch"] = std::to_string(opened.epoch);
			document["frame"] = frame;
			document["image_views"] = description.at("source_views");
			document["frusta_source"] = frusta ? "captured_image_views" : "unavailable";
			document["analysis_width"] = outputWidth;
			document["analysis_height"] = outputHeight;
			document["resized"] = resized;
			document["analysis_source_format"] = sourceFormat;
			document["analysis_pixels"] = "RGB8 clamped; nearest-neighbour GPU reduction when resized";
			Json content = Json::array();
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
			return CaptureResult{std::move(document), std::move(content), {opened.session, opened.generation, opened.process, opened.epoch}};
		}
		Json metadata = {{"ok", true}, {"application_id", target}, {"session", std::to_string(opened.session)},
			{"frame", frame}, {"layer", layer}, {"eye", eye}, {"format", format}, {"resized", resized},
			{"source_width", width}, {"source_height", height}, {"eyes", std::move(eyes)}};
		if (!composition.is_null())
			metadata["composition"] = std::move(composition);
		const auto preview = ImageDocument(retained);
		if (preview.contains("display"))
			metadata["display"] = preview.at("display");
		if (preview.contains("document"))
			metadata["tracking"] = preview.at("document");
		return CaptureResult{std::move(metadata), std::move(images)};
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return CaptureResult{Json{{"ok", false}, {"error", error.what()}}};
	}
}

std::optional<Json> CaptureScreenshot(McpApplication& application, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	return CaptureReply(Capture(application, arguments, cancellation, CaptureMode::Screenshot));
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
	return CaptureReply(Capture(application, request, cancellation, CaptureMode::Validation));
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
		std::optional<std::array<std::uint64_t, 4>> scope;
		bool incomplete = false;
		for (unsigned index = 0; index < frames; ++index)
		{
			const auto started = std::chrono::steady_clock::now();
			auto result = Capture(application, request, cancellation, CaptureMode::Consistency);
			if (!result || cancellation->Requested())
				return {};
			auto document = std::move(result->document);
			if (!document.value("ok", false))
			{
				incomplete = true;
				samples.push_back({{"index", index}, {"ok", false}, {"error", document.value("error", "Capture failed")}});
			}
			else
			{
				if (scope && *scope != result->scope)
					throw std::runtime_error("The selected XR session changed during stereo sampling");
				scope = result->scope;
				document["index"] = index;
				samples.push_back(document);
				const auto score = document.value("score", 0.0);
				if (!worst || score > worstScore)
				{
					worst = std::move(document);
					worstScore = score;
					image = result->images.empty() ? std::nullopt : std::optional<Json>(std::move(result->images[0]));
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
