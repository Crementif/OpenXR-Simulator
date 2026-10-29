#include "app/mcp/McpPch.h"
#include "app/mcp/McpImage.h"
#include "app/mcp/McpSimulation.h"
#include "control/ControlSchema.h"
#include "control/ImageInspection.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;

static void Keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
	for (auto it = object.begin(); it != object.end(); ++it)
		if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
			throw std::invalid_argument("Unknown argument: " + it.key());
}
std::uint64_t ImageId(const Json& object, const char* key, bool zero)
{
	const auto it = object.find(key);
	if (it == object.end())
		throw std::invalid_argument(std::string("Missing ") + key);
	std::uint64_t result = 0;
	if (it->is_string())
	{
		const auto text = it->get<std::string>();
		if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0'))
			throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
	}
	else if (it->is_number_integer() && *it >= 0 && *it <= ((1ULL << 53) - 1))
		result = it->get<std::uint64_t>();
	else
		throw std::invalid_argument(std::string(key) + " requires a decimal string or safe integer");
	if (!zero && !result)
		throw std::invalid_argument(std::string(key) + " must be nonzero");
	return result;
}
std::string EncodeBase64(std::span<const std::byte> bytes)
{
	static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve((bytes.size() + 2) / 3 * 4);
	for (std::size_t offset = 0; offset < bytes.size(); offset += 3)
	{
		const auto first = std::to_integer<unsigned>(bytes[offset]);
		const auto second = offset + 1 < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 1]) : 0;
		const auto third = offset + 2 < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 2]) : 0;
		result.push_back(alphabet[first >> 2]);
		result.push_back(alphabet[((first & 3) << 4) | (second >> 4)]);
		result.push_back(offset + 1 < bytes.size() ? alphabet[((second & 15) << 2) | (third >> 6)] : '=');
		result.push_back(offset + 2 < bytes.size() ? alphabet[third & 63] : '=');
	}
	return result;
}
static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}
bool ImageHandles(const std::string& name)
{
	static const std::set<std::string> names{"debug_retain_image", "debug_image_info", "debug_release_image",
		"debug_start_export", "debug_start_video_export", "debug_export_info", "debug_read_export",
		"debug_release_export", "debug_render_export", "debug_image_history", "debug_retain_history",
		"debug_begin_image_window", "debug_image_window", "debug_retain_window_image", "debug_release_image_window",
		"debug_retain_preview"};
	return names.contains(name);
}
Json ImageDefinitions()
{
	const Json id = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json zeroId = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
		{{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const Json common = {{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
		{"session", id}, {"generation", id}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	Json result = Json::array();
	auto capture = common;
	capture["capture"] = id;
	result.push_back(tool("debug_retain_image", "Retain the exact image of an existing capture; no pixel readback.", capture,
		Json::array({"connection", "capture"})));
	auto preview = common;
	preview["frame"] = id;
	result.push_back(tool("debug_retain_preview", "Retain the selected completed display image, or an exact retained preview frame, without pausing or reading pixels. A clear is reported explicitly.", preview,
		Json::array({"connection"})));
	auto history = common;
	history["target"] = {{"type", "string"}, {"enum", Json::array({"composed_eyes", "quad_layers"})}};
	auto historyPage = history;
	historyPage["count"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}};
	result.push_back(tool("debug_image_history", "Read recent exact GPU image history and unavailable or evicted frames without readback.", historyPage,
		Json::array({"connection"})));
	auto historicalImage = history;
	historicalImage["frame"] = id;
	result.push_back(tool("debug_retain_history", "Retain an existing historical image by exact frame identity without readback.", historicalImage,
		Json::array({"connection", "frame"})));
	result.push_back(tool("debug_begin_image_window", "Retain available image prehistory and collect twelve subsequent frames without changing application pacing.", history,
		Json::array({"connection"})));
	auto window = common;
	window["window"] = id;
	result.push_back(tool("debug_image_window", "Read a connection-owned image window and its collection state.", window,
		Json::array({"connection", "window"})));
	result.push_back(tool("debug_release_image_window", "Release an image window and stop its unfinished collection.", window,
		Json::array({"connection", "window"})));
	auto windowImage = window;
	windowImage["frame"] = id;
	result.push_back(tool("debug_retain_window_image", "Retain one exact image from a window independently of the window lifetime.", windowImage,
		Json::array({"connection", "window", "frame"})));
	auto image = common;
	image["image"] = id;
	result.push_back(tool("debug_image_info", "Inspect a retained image without reading pixels.", image, Json::array({"connection", "image"})));
	result.push_back(tool("debug_release_image", "Release a retained image reader.", image, Json::array({"connection", "image"})));
	auto start = image;
	start["layer"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}};
	const Json coordinate = {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}};
	const Json dimension = {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}};
	start["region"] = {{"type", "object"}, {"properties", {{"x", coordinate}, {"y", coordinate},
		{"width", dimension}, {"height", dimension}, {"output_width", dimension}, {"output_height", dimension}}},
		{"required", Json::array({"x", "y", "width", "height", "output_width", "output_height"})},
		{"additionalProperties", false}};
	result.push_back(tool("debug_start_export", "Begin an asynchronous raw pixel export. Optional region selects a crop and GPU nearest-neighbor resize.", start,
		Json::array({"connection", "image"})));
	auto video = common;
	video["first_frame"] = id;
	video["last_frame"] = id;
	result.push_back(tool("debug_start_video_export", "Start an asynchronous silent stereo MP4 export of an inclusive retained frame range.", video,
		Json::array({"connection", "first_frame", "last_frame"})));
	auto ticket = common;
	ticket["export"] = id;
	result.push_back(tool("debug_export_info", "Poll a retained export ticket for completion and format.", ticket, Json::array({"connection", "export"})));
	result.push_back(tool("debug_release_export", "Release an export ticket.", ticket, Json::array({"connection", "export"})));
	auto render = ticket;
	render["format"] = {{"type", "string"}, {"enum", Json::array({"jpeg", "png"})}};
	render["quality"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}};
	result.push_back(tool("debug_render_export", "Encode one completed single-layer export as standard MCP image content; JPEG is compact, PNG preserves 8-bit color.", render,
		Json::array({"connection", "export"})));
	auto read = ticket;
	read["offset"] = zeroId;
	read["length"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", wire::MaxPayload - 4096}};
	result.push_back(tool("debug_read_export", "Read a bounded raw export chunk as base64 after completion.", read,
		Json::array({"connection", "export"})));
	return result;
}
ImageRenderRequest PrepareImageRender(const Json& args)
{
	Keys(args, {"connection", "session", "generation", "export", "format", "quality"});
	ImageRenderRequest result;
	result.ticket = ImageId(args, "export");
	if (args.contains("session") != args.contains("generation"))
		throw std::invalid_argument("Supply both session and generation, or omit both");
	if (args.contains("session"))
	{
		result.session = ImageId(args, "session");
		result.generation = ImageId(args, "generation");
	}
	if (const auto format = args.find("format"); format != args.end())
	{
		if (!format->is_string() || (*format != "jpeg" && *format != "png"))
			throw std::invalid_argument("format must be jpeg or png");
		result.png = *format == "png";
	}
	if (args.contains("quality"))
	{
		const auto quality = ImageId(args, "quality");
		if (!args["quality"].is_number_integer() || quality > 100)
			throw std::invalid_argument("quality must be an integer from 1 to 100");
		result.quality = static_cast<int>(quality);
	}
	return result;
}
ImageRequest PrepareImage(const std::string& name, const Json& args, std::uint64_t session, std::uint64_t generation)
{
	ImageRequest request{wire::OpCode::RetainImage, {}};
	if (name == "debug_retain_image")
	{
		Keys(args, {"connection", "session", "generation", "capture"});
		request.fields = {wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true),
			wire::Field::U64(tag::CaptureId, ImageId(args, "capture"), true)};
	}
	else if (name == "debug_retain_preview")
	{
		Keys(args, {"connection", "session", "generation", "frame"});
		request.opcode = wire::OpCode::RetainPreview;
		request.fields = {wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true)};
		if (args.contains("frame"))
			request.fields.push_back(wire::Field::U64(tag::FrameId, ImageId(args, "frame"), true));
	}
	else if (name == "debug_image_history" || name == "debug_retain_history" || name == "debug_begin_image_window")
	{
		if (name == "debug_image_history")
			Keys(args, {"connection", "session", "generation", "target", "count"});
		else if (name == "debug_retain_history")
			Keys(args, {"connection", "session", "generation", "target", "frame"});
		else
			Keys(args, {"connection", "session", "generation", "target"});
		request.opcode = name == "debug_image_history" ? wire::OpCode::ReadImageHistory
			: name == "debug_retain_history" ? wire::OpCode::RetainHistoricalImage : wire::OpCode::BeginImageWindow;
		request.fields = {wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true)};
		if (args.contains("target"))
		{
			const auto& target = args.at("target");
			if (!target.is_string() || (target != "composed_eyes" && target != "quad_layers"))
				throw std::invalid_argument("target must be composed_eyes or quad_layers");
			const auto kind = target == "quad_layers" ? wire::TargetKind::QuadLayers : wire::TargetKind::ComposedEyes;
			const std::array values{wire::Field::U32(tag::CaptureKind, static_cast<std::uint32_t>(kind), true),
				wire::Field::U32(tag::Layer, 0), wire::Field::U32(tag::View, 0)};
			request.fields.push_back(wire::Field::Bytes(tag::Target, wire::EncodeFields(values)));
		}
		if (name == "debug_image_history" && args.contains("count"))
		{
			const auto count = ImageId(args, "count");
			if (!args["count"].is_number_integer() || count > 10)
				throw std::invalid_argument("count must be an integer from 1 to 10");
			request.fields.push_back(wire::Field::U64(tag::Length, count));
		}
		if (name == "debug_retain_history")
			request.fields.push_back(wire::Field::U64(tag::FrameId, ImageId(args, "frame"), true));
	}
	else if (name == "debug_image_window" || name == "debug_release_image_window" || name == "debug_retain_window_image")
	{
		if (name == "debug_retain_window_image")
			Keys(args, {"connection", "session", "generation", "window", "frame"});
		else
			Keys(args, {"connection", "session", "generation", "window"});
		request.opcode = name == "debug_image_window" ? wire::OpCode::ReadImageWindow
			: name == "debug_release_image_window" ? wire::OpCode::ReleaseImageWindow : wire::OpCode::RetainWindowImage;
		request.fields.push_back(wire::Field::U64(tag::ImageWindowId, ImageId(args, "window"), true));
		if (name == "debug_retain_window_image")
			request.fields.push_back(wire::Field::U64(tag::FrameId, ImageId(args, "frame"), true));
	}
	else if (name == "debug_start_video_export")
	{
		Keys(args, {"connection", "session", "generation", "first_frame", "last_frame"});
		const auto first = ImageId(args, "first_frame"), last = ImageId(args, "last_frame");
		if (last < first || last - first >= 1000000)
			throw std::invalid_argument("Select one to one million ordered frames");
		request.opcode = wire::OpCode::StartVideoExport;
		request.fields = {wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true),
			wire::Field::U64(tag::FirstFrame, first, true), wire::Field::U64(tag::LastFrame, last, true)};
	}
	else if (name == "debug_image_info" || name == "debug_release_image" || name == "debug_start_export")
	{
		Keys(args, name == "debug_start_export" ? std::initializer_list<std::string_view>{"connection", "session", "generation", "image", "layer", "region"}
			: std::initializer_list<std::string_view>{"connection", "session", "generation", "image"});
		request.opcode = name == "debug_image_info" ? wire::OpCode::ImageInfo :
			name == "debug_release_image" ? wire::OpCode::ReleaseImage : wire::OpCode::StartExport;
		request.fields.push_back(wire::Field::U64(tag::ImageReaderId, ImageId(args, "image"), true));
		if (args.contains("layer"))
		{
			const auto layer = ImageId(args, "layer", true);
			if (layer > UINT32_MAX || !args["layer"].is_number_integer())
				throw std::invalid_argument("layer must be a uint32 integer");
			request.fields.push_back(wire::Field::U32(tag::Layer, static_cast<std::uint32_t>(layer)));
		}
		if (args.contains("region"))
		{
			const auto& region = args.at("region");
			if (!region.is_object())
				throw std::invalid_argument("region must be an object");
			Keys(region, {"x", "y", "width", "height", "output_width", "output_height"});
			std::array<std::uint32_t, 6> values{};
			std::size_t index = 0;
			for (const auto* key : {"x", "y", "width", "height", "output_width", "output_height"})
			{
				const bool zero = index < 2;
				const auto value = ImageId(region, key, zero);
				if (value > UINT32_MAX || !region.at(key).is_number_integer())
					throw std::invalid_argument("Image region components must be uint32 integers");
				values[index++] = static_cast<std::uint32_t>(value);
			}
			request.fields.push_back(wire::EncodeImageRegion({values[0], values[1], values[2], values[3], values[4], values[5]}, true));
		}
	}
	else
	{
		Keys(args, name == "debug_read_export" ? std::initializer_list<std::string_view>{"connection", "session", "generation", "export", "offset", "length"}
			: std::initializer_list<std::string_view>{"connection", "session", "generation", "export"});
		request.opcode = name == "debug_export_info" ? wire::OpCode::ExportInfo :
			name == "debug_release_export" ? wire::OpCode::ReleaseExport : wire::OpCode::ReadExport;
		request.fields.push_back(wire::Field::U64(tag::ExportId, ImageId(args, "export"), true));
		if (name == "debug_read_export")
		{
			const auto offset = args.contains("offset") ? ImageId(args, "offset", true) : 0;
			const auto length = args.contains("length") ? ImageId(args, "length") : wire::MaxPayload - 4096;
			if (length > wire::MaxPayload - 4096 || (args.contains("length") && !args["length"].is_number_integer()))
				throw std::invalid_argument("length exceeds the bounded export range");
			request.fields.push_back(wire::Field::U64(tag::Offset, offset, true));
			request.fields.push_back(wire::Field::U64(tag::Length, length, true));
		}
	}
	return request;
}
Json ImageDocument(const wire::Message& message)
{
	const auto* field = message.Find(tag::Error);
	const auto code = field ? field->AsU32() : std::nullopt;
	if (!code)
		throw std::runtime_error("Runtime omitted its image error code");
	if (*code)
	{
		Json result = {{"ok", false}, {"error", "Runtime rejected the request"}, {"code", *code}};
		if (const auto* detail = message.Find(tag::Detail))
			if (const auto text = detail->AsText())
				result["detail"] = std::string(*text);
		return result;
	}
	Json result = {{"ok", true}};
	if (const auto* layer = message.Find(tag::Layer))
		if (const auto value = layer->AsU32())
			result["layer"] = *value;
	for (const auto& [tagValue, name] : {std::pair{tag::ImageReaderId, "image"}, {tag::ExportId, "export"},
		{tag::CaptureId, "capture"}, {tag::ImageWindowId, "window"}, {tag::TotalBytes, "total_bytes"}, {tag::Offset, "offset"}, {tag::Length, "length"},
		{tag::Capabilities, "capabilities"}})
		if (const auto* value = message.Find(tagValue))
			if (const auto number = value->AsU64())
				result[name] = std::to_string(*number);
	for (const auto& [tagValue, name] : {std::pair{tag::Phase, "phase"}, {tag::Detail, "detail"}, {tag::MediaType, "media_type"}})
		if (const auto* value = message.Find(tagValue))
			if (const auto text = value->AsText())
				result[name] = std::string(*text);
	for (const auto& [tagValue, name] : {std::pair{tag::Released, "released"}, {tag::End, "end"}, {tag::ImageAvailable, "image_available"}})
		if (const auto* value = message.Find(tagValue))
			if (const auto flag = value->AsBool())
				result[name] = *flag;
	if (const auto* document = message.Find(tag::Document))
		result["document"] = InspectionJson(document->value);
	if (const auto* age = message.Find(tag::SourceAge))
	{
		const auto value = age->AsI64();
		if (!value)
			throw std::runtime_error("Invalid preview source age");
		result["source_age_ns"] = std::to_string(*value);
	}
	if (const auto* display = message.Find(tag::Display))
	{
		const auto decoded = wire::DecodeFields(display->value);
		if (decoded.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid preview display metadata");
		wire::Message tick;
		tick.fields = decoded.fields;
		const auto* tickId = tick.Find(tag::Tick);
		const auto* time = tick.Find(tag::Time);
		const auto* kind = tick.Find(tag::Kind);
		if (!tickId || !tickId->AsU64() || !time || !time->AsI64() || !kind || !kind->AsText())
			throw std::runtime_error("Incomplete preview display metadata");
		Json value = {{"tick", std::to_string(*tickId->AsU64())}, {"time_ns", std::to_string(*time->AsI64())},
			{"kind", std::string(*kind->AsText())}};
		if (const auto* policy = tick.Find(tag::PresentationPolicy))
		{
			const auto text = policy->AsText();
			if (!text)
				throw std::runtime_error("Invalid preview presentation policy");
			value["presentation_policy"] = std::string(*text);
		}
		if (const auto* skipped = tick.Find(tag::SkippedTicks))
		{
			const auto count = skipped->AsU64();
			if (!count)
				throw std::runtime_error("Invalid skipped preview tick count");
			value["skipped_ticks"] = std::to_string(*count);
		}
		for (const auto& [tagValue, name] : {std::pair{tag::ReprojectionResult, "reprojection_result"},
				{tag::ReprojectionSubmissionTime, "reprojection_submission_time_ns"},
				{tag::ReprojectionCompletionLatency, "reprojection_completion_latency_ns"}})
			if (const auto* encoded = tick.Find(tagValue))
			{
				const auto number = encoded->AsI64();
				if (!number)
					throw std::runtime_error("Invalid preview reprojection metadata");
				value[name] = std::to_string(*number);
			}
		if (const auto* head = tick.Find(tag::DisplayHead))
			value["display_head"] = InspectionJson(head->value);
		if (const auto* source = tick.Find(tag::Source))
		{
			const auto decodedSource = wire::DecodeFields(source->value);
			if (decodedSource.error != wire::DecodeError::None)
				throw std::runtime_error("Invalid preview source metadata");
			wire::Message key;
			key.fields = decodedSource.fields;
			const auto* generation = key.Find(tag::Generation);
			const auto* frame = key.Find(tag::FrameId);
			if (!generation || !generation->AsU64() || !frame || !frame->AsU64())
				throw std::runtime_error("Incomplete preview source metadata");
			value["source"] = {{"generation", std::to_string(*generation->AsU64())},
				{"frame_id", std::to_string(*frame->AsU64())}};
		}
		if (const auto* imageTick = tick.Find(tag::ImageTick))
		{
			const auto id = imageTick->AsU64();
			if (!id)
				throw std::runtime_error("Invalid preview image tick");
			value["image_tick"] = std::to_string(*id);
		}
		if (const auto* reprojected = tick.Find(tag::Reprojected))
		{
			const auto flag = reprojected->AsBool();
			if (!flag)
				throw std::runtime_error("Invalid preview reprojection state");
			value["reprojected"] = *flag;
		}
		result["display"] = std::move(value);
	}
	if (const auto* descriptor = message.Find(tag::ImageDescriptor))
	{
		const auto image = wire::DecodeImageDescription(*descriptor);
		result["image_description"] = {{"format", image.format}, {"width", image.width}, {"height", image.height},
			{"depth", image.depth}, {"array_layers", image.arrayLayers}, {"mip_levels", image.mipLevels}, {"samples", image.samples}};
		Json views = Json::array();
		for (const auto& view : image.views)
			views.push_back({{"position", {view.pose.position.x, view.pose.position.y, view.pose.position.z}},
				{"orientation", {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w}},
				{"fov", {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown}}});
		result["image_description"]["source_views"] = std::move(views);
	}
	if (const auto* frame = message.Find(tag::Frame))
	{
		const auto decoded = wire::DecodeFields(frame->value);
		if (decoded.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid image frame metadata");
		wire::Message nested;
		nested.fields = decoded.fields;
		const auto* generation = nested.Find(tag::Generation);
		const auto* identity = nested.Find(tag::FrameId);
		if (!generation || !generation->AsU64() || !identity || !identity->AsU64())
			throw std::runtime_error("Incomplete image frame metadata");
		result["frame"] = {{"generation", std::to_string(*generation->AsU64())}, {"frame_id", std::to_string(*identity->AsU64())}};
	}
	if (const auto* target = message.Find(tag::Target))
	{
		const auto decoded = wire::DecodeFields(target->value);
		if (decoded.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid image target metadata");
		wire::Message nested;
		nested.fields = decoded.fields;
		const auto* kind = nested.Find(tag::CaptureKind);
		const auto* layer = nested.Find(tag::Layer);
		const auto* view = nested.Find(tag::View);
		if (!kind || !kind->AsU32() || !layer || !layer->AsU32() || !view || !view->AsU32())
			throw std::runtime_error("Incomplete image target metadata");
		result["target"] = {{"kind", *kind->AsU32()}, {"layer", *layer->AsU32()}, {"view", *view->AsU32()}};
	}
	if (const auto* region = message.Find(tag::ImageRegion))
	{
		const auto decoded = wire::DecodeFields(region->value);
		if (decoded.error != wire::DecodeError::None || decoded.fields.size() != 6)
			throw std::runtime_error("Invalid image region metadata");
		Json value = Json::object();
		std::size_t index = 0;
		for (const auto* name : {"x", "y", "width", "height", "output_width", "output_height"})
		{
			const auto& component = decoded.fields[index];
			const auto number = component.AsU32();
			if (component.tag != index + 1 || !number)
				throw std::runtime_error("Invalid image region component");
			value[name] = *number;
			++index;
		}
		result["region"] = std::move(value);
	}
	if (const auto* bytes = message.Find(tag::Bytes))
	{
		result["encoding"] = "base64";
		result["bytes"] = EncodeBase64(bytes->value);
	}
	return result;
}
Json ImageReply(const wire::Message& message)
{
	auto document = ImageDocument(message);
	return Result(document, !document.at("ok").get<bool>());
}
} // namespace simxr::app::mcp
