#include "app/mcp/McpPch.h"
#include "app/mcp/McpHistory.h"
#include "app/mcp/McpSimulation.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

static void Keys(const Json& value, std::initializer_list<std::string_view> allowed)
{
	if (!value.is_object())
		throw std::invalid_argument("Arguments must be an object");
	for (auto it = value.begin(); it != value.end(); ++it)
		if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
			throw std::invalid_argument("Unknown argument: " + it.key());
}

static std::uint64_t Number(const Json& object, const char* key)
{
	const auto& value = object.at(key);
	if (value.is_number_integer() && value >= 0 && value <= ((1ULL << 53) - 1))
		return value.get<std::uint64_t>();
	if (!value.is_string())
		throw std::invalid_argument(std::string(key) + " requires a decimal string or safe integer");
	const auto text = value.get<std::string>();
	std::uint64_t result = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
	if (text.empty() || (text.size() > 1 && text.front() == '0') || parsed.ec != std::errc{} ||
		parsed.ptr != text.data() + text.size())
		throw std::invalid_argument(std::string(key) + " requires a canonical unsigned decimal integer");
	return result;
}

static wire::Message Nested(const wire::Field* field)
{
	if (!field)
		throw std::runtime_error("Runtime omitted historical metadata");
	auto decoded = wire::DecodeFields(field->value);
	if (decoded.error != wire::DecodeError::None)
		throw std::runtime_error("Runtime returned malformed historical metadata");
	wire::Message result;
	result.fields = std::move(decoded.fields);
	return result;
}

static std::uint64_t U64(const wire::Message& message, std::uint16_t tagValue)
{
	const auto* field = message.Find(tagValue);
	const auto value = field ? field->AsU64() : std::nullopt;
	if (!value)
		throw std::runtime_error("Runtime omitted a historical identifier");
	return *value;
}

static std::int64_t I64(const wire::Message& message, std::uint16_t tagValue)
{
	const auto* field = message.Find(tagValue);
	const auto value = field ? field->AsI64() : std::nullopt;
	if (!value)
		throw std::runtime_error("Runtime omitted historical timing");
	return *value;
}

static std::uint32_t U32(const wire::Message& message, std::uint16_t tagValue)
{
	const auto* field = message.Find(tagValue);
	const auto value = field ? field->AsU32() : std::nullopt;
	if (!value)
		throw std::runtime_error("Runtime omitted a historical value");
	return *value;
}

static std::string Text(const wire::Message& message, std::uint16_t tagValue)
{
	const auto* field = message.Find(tagValue);
	const auto value = field ? field->AsText() : std::nullopt;
	if (!value)
		throw std::runtime_error("Runtime omitted historical text");
	return std::string(*value);
}

static Json EncodedCommand(const wire::Field& field, const SimulationCommand& command)
{
	if (wire::EncodeSimulationCommand(command).value != field.value)
		throw std::runtime_error("Historical input command is not canonical");
	constexpr std::string_view digits = "0123456789abcdef";
	std::string hex;
	hex.reserve(field.value.size() * 2);
	for (const auto byte : field.value)
	{
		const auto value = std::to_integer<unsigned>(byte);
		hex.push_back(digits[value >> 4]);
		hex.push_back(digits[value & 15]);
	}
	return {{"encoding", "simxr-command-v1-hex"}, {"data", std::move(hex)}};
}

static Json AppliedInput(const wire::Field& field)
{
	const auto input = Nested(&field);
	static constexpr std::array known{std::uint16_t{1},	 std::uint16_t{2},	std::uint16_t{3},  std::uint16_t{4},
									  std::uint16_t{5},	 std::uint16_t{6},	std::uint16_t{7},  std::uint16_t{8},
									  std::uint16_t{9},	 std::uint16_t{10}, std::uint16_t{11}, tag::TrackerCollection,
									  tag::InputContext, tag::Seed,			tag::Scenario,	   tag::Ownership,
									  tag::ActiveActions};
	if (input.ValidateFields(known))
		throw std::runtime_error("Runtime returned an unsupported required historical input field");
	CommandConfiguration config;
	Json commands = Json::array(), context = Json::array();
	std::uint32_t seen = 0, trackers = 0, contextSeen = 0;
	bool collection = false;
	for (const auto& item : input.fields)
	{
		if (item.tag > 11 && item.tag != tag::TrackerCollection)
			continue;
		const auto command = wire::DecodeSimulationCommand(item);
		commands.push_back(EncodedCommand(item, command));
		std::uint32_t bit = 0;
		if (const auto* head = std::get_if<SetHeadPose>(&command); item.tag == 1 && head)
		{
			config.tracking.head = head->value;
			bit = 1;
		}
		else if (const auto* pose = std::get_if<SetControllerPose>(&command);
				 item.tag >= 2 && item.tag <= 3 && pose && pose->hand == static_cast<std::uint32_t>(item.tag - 2))
		{
			config.tracking.controllers[pose->hand] = pose->value;
			bit = 2u << pose->hand;
		}
		else if (const auto* controls = std::get_if<SetControllerControls>(&command);
				 item.tag >= 4 && item.tag <= 5 && controls &&
				 controls->hand == static_cast<std::uint32_t>(item.tag - 4))
		{
			config.input.controllers[controls->hand] = controls->value;
			bit = 8u << controls->hand;
		}
		else if (const auto* body = std::get_if<SetBodyControls>(&command); item.tag == 6 && body)
		{
			config.tracking.body = body->value;
			config.tracking.bodyObservation = body->observation;
			bit = 32;
		}
		else if (const auto* gaze = std::get_if<SetGaze>(&command); item.tag == 7 && gaze)
		{
			config.tracking.gaze = gaze->value;
			bit = 64;
		}
		else if (const auto* tracker = std::get_if<SetTracker>(&command);
				 item.tag >= 8 && item.tag <= 11 && tracker &&
				 tracker->index == static_cast<std::uint32_t>(item.tag - 8) && !collection)
		{
			config.tracking.trackers[tracker->index] = tracker->value;
			trackers |= 1u << tracker->index;
			bit = 128u << tracker->index;
		}
		else if (const auto* collectionValue = std::get_if<SetTrackers>(&command);
				 item.tag == tag::TrackerCollection && collectionValue && !trackers && !collection)
		{
			config.tracking.trackers = collectionValue->value;
			collection = true;
		}
		else
			throw std::runtime_error("Runtime returned unexpected historical input");
		if (seen & bit)
			throw std::runtime_error("Runtime repeated historical input");
		seen |= bit;
	}
	if ((collection && seen != 127) || (!collection && seen != 2047))
		throw std::runtime_error("Runtime returned incomplete historical input");
	if (const auto* inputContext = input.Find(tag::InputContext))
	{
		const auto fields = Nested(inputContext);
		for (const auto& item : fields.fields)
		{
			const auto command = wire::DecodeSimulationCommand(item);
			context.push_back(EncodedCommand(item, command));
			if (const auto* views = std::get_if<SetViewSettings>(&command); item.tag == 1 && views)
				config.views = views->value;
			else if (const auto* proportions = std::get_if<SetBodyProportions>(&command); item.tag == 2 && proportions)
				config.proportions = proportions->value;
			else if (const auto* profile = std::get_if<SetControllerProfile>(&command);
					 item.tag >= 3 && item.tag <= 4 && profile &&
					 profile->hand == static_cast<std::uint32_t>(item.tag - 3))
				config.input.preferredProfiles[profile->hand] = profile->path;
			else if (const auto* spaces = std::get_if<SetReferenceSpaces>(&command); item.tag == 5 && spaces)
				config.referenceSpaces = spaces->value;
			else if (const auto* eyes = std::get_if<SetEyeViews>(&command); item.tag == 6 && eyes)
				config.tracking.eyeViews = eyes->value;
			else
				throw std::runtime_error("Runtime returned unexpected historical input context");
			contextSeen |= 1u << (item.tag - 1);
		}
		if ((contextSeen & 31u) != 31u)
			throw std::runtime_error("Runtime returned incomplete historical input context");
	}
	const auto seed = U64(input, tag::Seed);
	auto applied = std::make_shared<AppliedCommandSnapshot>();
	applied->configuration = std::move(config);
	CommandSchedulerSnapshot snapshot;
	snapshot.applied = std::move(applied);
	auto state = InspectionJson(wire::SimulationState(snapshot).Encode()).at("configuration");
	state.erase("focused");
	if (!contextSeen)
	{
		state.erase("views");
		state.erase("proportions");
		state.erase("reference_spaces");
		state.erase("eye_views");
		for (auto& controller : state.at("controllers"))
			controller.erase("profile");
	}
	return {{"seed", std::to_string(seed)},
			{"scenario_mask", U32(input, tag::Scenario)},
			{"ownership_mask", U32(input, tag::Ownership)},
			{"active_actions", U32(input, tag::ActiveActions)},
			{"configuration", std::move(state)},
			{"commands", std::move(commands)},
			{"context", std::move(context)},
			{"context_available", contextSeen != 0}};
}

static Json Frame(const wire::Field& field, std::uint64_t generation, std::uint64_t after)
{
	const auto frame = Nested(&field);
	static constexpr std::array known{tag::Generation, tag::FrameId,	  tag::Prediction,		  tag::Stage,
									  tag::Submission, tag::CpuFrameTime, tag::CompositorGpuTime, tag::AppliedInput};
	if (frame.ValidateFields(known))
		throw std::runtime_error("Runtime returned an unsupported required historical frame field");
	const auto identity = U64(frame, tag::FrameId);
	if (!identity || identity <= after || U64(frame, tag::Generation) != generation)
		throw std::runtime_error("Runtime returned a stale or unordered historical frame");
	const auto prediction = Nested(frame.Find(tag::Prediction));
	static constexpr std::array predictionFields{tag::FrameId, tag::Tick, tag::Time, tag::Period};
	if (prediction.ValidateFields(predictionFields))
		throw std::runtime_error("Runtime returned an unsupported required prediction field");
	if (U64(prediction, tag::FrameId) != identity || I64(prediction, tag::Period) <= 0)
		throw std::runtime_error("Runtime returned inconsistent historical prediction");
	Json value = {{"frame_id", std::to_string(identity)},
				  {"generation", std::to_string(generation)},
				  {"stage", Text(frame, tag::Stage)},
				  {"prediction",
				   {{"frame_id", std::to_string(identity)},
					{"tick", std::to_string(U64(prediction, tag::Tick))},
					{"time_ns", std::to_string(I64(prediction, tag::Time))},
					{"period_ns", std::to_string(I64(prediction, tag::Period))}}}};
	if (const auto* submitted = frame.Find(tag::Submission))
	{
		const auto submission = Nested(submitted);
		static constexpr std::array submissionFields{tag::Time, tag::LayerCount, tag::OutputDisposition,
													 tag::DropReason};
		if (submission.ValidateFields(submissionFields))
			throw std::runtime_error("Runtime returned an unsupported required submission field");
		value["submission"] = {{"time_ns", std::to_string(I64(submission, tag::Time))},
							   {"layer_count", U32(submission, tag::LayerCount)},
							   {"output_disposition", Text(submission, tag::OutputDisposition)},
							   {"drop_reason", Text(submission, tag::DropReason)}};
	}
	for (const auto& [tagValue, name] :
		 {std::pair{tag::CpuFrameTime, "cpu_frame_time_ns"}, {tag::CompositorGpuTime, "compositor_gpu_time_ns"}})
		if (frame.Find(tagValue))
			value[name] = std::to_string(I64(frame, tagValue));
	if (const auto* input = frame.Find(tag::AppliedInput))
		value["applied_input"] = AppliedInput(*input);
	else
		value["applied_input"] = nullptr;
	return value;
}

bool HistoryHandles(const std::string& name)
{
	return name == "debug_frame_history" || name == "debug_projection_history";
}

Json HistoryDefinitions()
{
	const Json id = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
											{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json zeroId = {
		{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
							   {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const Json common = {{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
						 {"session", id},
						 {"generation", id}};
	const auto tool = [](const char* name, const char* description, Json properties) {
		return Json{{"name", name},
					{"description", description},
					{"inputSchema",
					 {{"type", "object"},
					  {"properties", std::move(properties)},
					  {"required", Json::array({"connection"})},
					  {"additionalProperties", false}}}};
	};
	auto frames = common;
	frames["after"] = zeroId;
	frames["count"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}};
	auto projections = common;
	projections["count"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}};
	return Json::array(
		{tool("debug_frame_history",
			  "Read bounded submitted CPU frame history and applied inputs without admission or image retention. "
			  "Continue after the last frame until a page is empty.",
			  frames),
		 tool("debug_projection_history",
			  "Read bounded submitted projection geometry and frame identities without image readback.", projections)});
}

HistoryRequest PrepareHistory(const std::string& name, const Json& arguments, std::uint64_t session,
							  std::uint64_t generation)
{
	const bool frames = name == "debug_frame_history";
	if (!frames && name != "debug_projection_history")
		throw std::invalid_argument("Unknown history tool");
	if (frames)
		Keys(arguments, {"connection", "session", "generation", "after", "count"});
	else
		Keys(arguments, {"connection", "session", "generation", "count"});
	HistoryRequest request{
		frames ? wire::OpCode::ReadFrameHistory : wire::OpCode::ReadProjectionHistory,
		{wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true)}};
	request.count = frames ? 128 : 64;
	if (frames && arguments.contains("after"))
	{
		request.after = Number(arguments, "after");
		request.fields.push_back(wire::Field::U64(tag::Offset, request.after));
	}
	if (arguments.contains("count"))
	{
		const auto count = Number(arguments, "count");
		if (!arguments.at("count").is_number_integer() || !count || count > (frames ? 128u : 64u))
			throw std::invalid_argument(frames ? "count must be an integer from 1 to 128"
											   : "count must be an integer from 1 to 64");
		request.count = static_cast<std::uint32_t>(count);
		request.fields.push_back(wire::Field::U64(tag::Length, count));
	}
	return request;
}

Json ProjectionHistoryDocument(const wire::Message& message, std::uint32_t count, std::uint64_t generation)
{
	const auto* document = message.Find(tag::Document);
	if (!document)
		throw std::runtime_error("Runtime omitted projection history");
	auto value = InspectionJson(document->value);
	if (!value.is_object() || value.value("generation", "") != std::to_string(generation) ||
		!value.contains("entries") || !value["entries"].is_array() || value["entries"].size() > count)
		throw std::runtime_error("Runtime returned invalid projection history");
	return value;
}

Json HistoryReply(const wire::Message& message, const HistoryRequest& request, std::uint64_t generation)
{
	const auto* error = message.Find(tag::Error);
	const auto code = error ? error->AsU32() : std::nullopt;
	if (!code)
		throw std::runtime_error("Runtime omitted its history error code");
	if (*code)
	{
		Json failure = {{"ok", false}, {"error", "Runtime rejected the request"}, {"code", *code}};
		if (const auto* detail = message.Find(tag::Detail))
			if (const auto text = detail->AsText())
				failure["detail"] = std::string(*text);
		return Result(failure, true);
	}
	if (request.opcode == wire::OpCode::ReadProjectionHistory)
		return Result({{"ok", true}, {"document", ProjectionHistoryDocument(message, request.count, generation)}});
	const auto frames = Nested(message.Find(tag::Frames));
	if (frames.fields.size() > request.count)
		throw std::runtime_error("Runtime returned too many historical frames");
	Json entries = Json::array();
	std::uint64_t cursor = request.after;
	std::size_t bytes = 0;
	bool truncated = false;
	constexpr std::size_t budget = 4 * 1024 * 1024;
	for (const auto& field : frames.fields)
	{
		if (field.tag != entries.size() + 1)
			throw std::runtime_error("Runtime returned noncontiguous historical frames");
		auto value = Frame(field, generation, cursor);
		const auto size = value.dump().size();
		if (size > budget - bytes)
		{
			if (entries.empty())
				throw std::runtime_error("One historical frame exceeds the response budget");
			truncated = true;
			break;
		}
		bytes += size;
		cursor = U64(Nested(&field), tag::FrameId);
		entries.push_back(std::move(value));
	}
	return Result({{"ok", true},
				   {"generation", std::to_string(generation)},
				   {"origin_time_ns", std::to_string(I64(message, tag::Time))},
				   {"capacity", std::to_string(U64(message, tag::Capacity))},
				   {"oldest_frame", std::to_string(U64(message, tag::Offset))},
				   {"next_after", std::to_string(cursor)},
				   {"truncated", truncated},
				   {"frames", std::move(entries)}});
}
} // namespace simxr::app::mcp
