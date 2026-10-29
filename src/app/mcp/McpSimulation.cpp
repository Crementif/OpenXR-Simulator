#include "app/mcp/McpPch.h"
#include "app/mcp/McpSimulation.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;

static void Keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
	if (!object.is_object())
		throw std::invalid_argument("Arguments must be an object");
	for (auto it = object.begin(); it != object.end(); ++it)
		if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
			throw std::invalid_argument("Unknown argument: " + it.key());
}

static std::uint64_t Number(const Json& object, const char* key, bool allowZero = false)
{
	const auto it = object.find(key);
	if (it == object.end())
		throw std::invalid_argument(std::string("Missing ") + key);
	std::uint64_t value = 0;
	if (it->is_string())
	{
		const auto text = it->get<std::string>();
		if (text.empty() || (text.size() > 1 && text.front() == '0'))
			throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
	}
	else if (it->is_number_integer() && *it >= 0 && *it <= ((1ULL << 53) - 1))
		value = it->get<std::uint64_t>();
	else
		throw std::invalid_argument(std::string(key) + " requires a decimal string or safe integer");
	if (!allowZero && !value)
		throw std::invalid_argument(std::string(key) + " must be nonzero");
	return value;
}

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

bool SimulationHandles(const std::string& name)
{
	static const std::set<std::string> names{
		"simulation_schedule",		  "simulation_status",		   "simulation_cancel",
		"simulation_release_control", "simulation_diagnostics",	   "simulation_start_recording",
		"simulation_stop_recording",  "simulation_read_recording", "simulation_acknowledge_recording"};
	return names.contains(name);
}

Json SimulationDefinitions()
{
	const Json identifier = {
		{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
							   {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json zeroIdentifier = {
		{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
							   {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const Json text = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
	const Json common = {{"connection", text}, {"session", identifier}, {"generation", identifier}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required) {
		return Json{{"name", name},
					{"description", description},
					{"inputSchema",
					 {{"type", "object"},
					  {"properties", std::move(properties)},
					  {"required", std::move(required)},
					  {"additionalProperties", false}}}};
	};
	Json result = Json::array();
	auto schedule = common;
	schedule["transaction_id"] = identifier;
	schedule["frame"] = identifier;
	schedule["time_ns"] = {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,18})$"}};
	schedule["late_policy"] = {{"type", "string"}, {"enum", Json::array({"reject", "reschedule_next"})}};
	schedule["commands"] = {
		{"type", "array"},
		{"minItems", 1},
		{"maxItems", 64},
		{"items", {{"type", "object"}}},
		{"description",
		 "Typed commands include take_control, release_control, head_pose, controller_pose, controller_controls, body, "
		 "tracker, trackers, gaze, views, eye_views, reference_spaces, proportions, controller_profile and scenario. A "
		 "body command accepts active, confidence, preset (0 standing, 1 T-pose, 2 seated), root_pose, five controls "
		 "and bounded joint overrides; optional observation carries provider joints and nanosecond times. All "
		 "positions and lengths use metres, angles use radians, orientations are [x,y,z,w] quaternions, and buttons "
		 "require JSON booleans. Controller commands require hand (0 left, 1 right). A tracker requires index and "
		 "identity (uint64 decimal string or safe integer), role (0-17), connected, tracked, anchor, position, "
		 "orientation, joint_offset, flags and source_path. A trackers command replaces the inventory with a devices "
		 "array. Eye views contain left and right position, orientation, flags and fov, plus space_revision; "
		 "enabled=false clears them. Reference spaces contain local and stage poses and optional stage bounds. "
		 "Recorded simxr-command-v1-hex objects are also accepted. See the automation documentation for field "
		 "details."}};
	result.push_back(tool("simulation_schedule",
						  "Schedule one atomic input transaction at an exact frame or simulated time. Include "
						  "take_control before setters; admission is not application.",
						  schedule, Json::array({"connection", "transaction_id", "commands"})));
	auto status = common;
	status["transaction_id"] = identifier;
	result.push_back(tool("simulation_status", "Read applied simulator state or one transaction outcome.", status,
						  Json::array({"connection"})));
	result.push_back(tool("simulation_cancel", "Cancel an unapplied transaction owned by this connection.", status,
						  Json::array({"connection", "transaction_id"})));
	result.push_back(tool("simulation_release_control",
						  "Release this connection's inputs, sequences and capture tickets, cancel pending commands "
						  "and stop its input recording.",
						  common, Json::array({"connection"})));
	auto diagnostics = common;
	diagnostics["after"] = zeroIdentifier;
	diagnostics["frame"] = identifier;
	diagnostics["count"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 256}};
	result.push_back(tool("simulation_diagnostics", "Read bounded frame and simulation diagnostics.", diagnostics,
						  Json::array({"connection"})));
	auto start = common;
	start["drain"] = {{"type", "boolean"}};
	result.push_back(tool("simulation_start_recording",
						  "Start a session-wide recording owned by this connection. drain=true retains pages until "
						  "acknowledged; save recording_id from the returned document.",
						  start, Json::array({"connection"})));
	auto recording = common;
	recording["recording_id"] = identifier;
	result.push_back(
		tool("simulation_stop_recording",
			 "Stop this connection's recording. recording_id protects against stopping a replacement recording.",
			 recording, Json::array({"connection", "recording_id"})));
	auto acknowledge = recording;
	acknowledge["through"] = zeroIdentifier;
	result.push_back(tool("simulation_acknowledge_recording",
						  "Acknowledge persisted draining recording entries before the exclusive through offset, "
						  "releasing their native storage. Only the recording owner may acknowledge.",
						  acknowledge, Json::array({"connection", "recording_id", "through"})));
	recording["offset"] = zeroIdentifier;
	recording["count"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 16}};
	result.push_back(tool("simulation_read_recording", "Read a bounded page of recorded simulator transactions.",
						  recording, Json::array({"connection", "recording_id"})));
	return result;
}

SimulationRequest PrepareSimulation(const std::string& name, const Json& arguments, std::uint64_t session,
									std::uint64_t generation)
{
	SimulationRequest result{wire::OpCode::SimulationStatus,
							 {wire::Field::U64(tag::Session, session, true),
							  wire::Field::U64(tag::Generation, generation, true),
							  wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Mcp))}};
	if (name == "simulation_schedule")
	{
		Keys(arguments,
			 {"connection", "session", "generation", "transaction_id", "frame", "time_ns", "late_policy", "commands"});
		if (arguments.contains("frame") == arguments.contains("time_ns"))
			throw std::invalid_argument("Select exactly one frame or time_ns target");
		result.opcode = wire::OpCode::SubmitSimulation;
		result.fields.push_back(wire::Field::U64(tag::TransactionId, Number(arguments, "transaction_id"), true));
		if (arguments.contains("frame"))
			result.fields.push_back(wire::Field::U64(tag::ApplyFrame, Number(arguments, "frame"), true));
		else
		{
			const auto value = Number(arguments, "time_ns", true);
			if (!arguments["time_ns"].is_string() || value > INT64_MAX)
				throw std::invalid_argument("time_ns must be a nonnegative int64 decimal string");
			result.fields.push_back(wire::Field::I64(tag::ApplyTime, static_cast<std::int64_t>(value), true));
		}
		const auto policy = arguments.value("late_policy", std::string("reject"));
		if (policy != "reject" && policy != "reschedule_next")
			throw std::invalid_argument("Unknown late policy");
		result.fields.push_back(wire::Field::U32(
			tag::LatePolicy, static_cast<std::uint32_t>(policy == "reject" ? CommandLatePolicy::Reject
																		   : CommandLatePolicy::RescheduleNext)));
		const auto& commands = arguments.at("commands");
		if (!commands.is_array() || commands.empty() || commands.size() > 64)
			throw std::invalid_argument("commands must contain one to 64 entries");
		std::vector<wire::Field> encoded;
		encoded.reserve(commands.size());
		for (const auto& command : commands)
			encoded.push_back(wire::EncodeSimulationCommand(SimulationCommandFromJson(command),
															static_cast<std::uint16_t>(encoded.size() + 1)));
		result.fields.push_back(wire::Field::Bytes(tag::Commands, wire::EncodeFields(encoded), true));
	}
	else if (name == "simulation_status" || name == "simulation_cancel")
	{
		Keys(arguments, {"connection", "session", "generation", "transaction_id"});
		result.opcode = name == "simulation_status" ? wire::OpCode::SimulationStatus : wire::OpCode::CancelSimulation;
		if (arguments.contains("transaction_id") || name == "simulation_cancel")
			result.fields.push_back(wire::Field::U64(tag::TransactionId, Number(arguments, "transaction_id"), true));
	}
	else if (name == "simulation_diagnostics")
	{
		Keys(arguments, {"connection", "session", "generation", "after", "count", "frame"});
		result.opcode = wire::OpCode::ReadDiagnostics;
		if (arguments.contains("after"))
			result.fields.push_back(wire::Field::U64(tag::Offset, Number(arguments, "after", true)));
		if (arguments.contains("count"))
		{
			const auto count = Number(arguments, "count", true);
			if (count > 256)
				throw std::invalid_argument("count exceeds 256");
			result.fields.push_back(wire::Field::U64(tag::Length, count));
		}
		if (arguments.contains("frame"))
			result.fields.push_back(wire::Field::U64(tag::FrameId, Number(arguments, "frame")));
	}
	else if (name == "simulation_read_recording")
	{
		Keys(arguments, {"connection", "session", "generation", "recording_id", "offset", "count"});
		result.opcode = wire::OpCode::ReadRecording;
		result.fields.push_back(wire::Field::U64(tag::RecordingId, Number(arguments, "recording_id"), true));
		if (arguments.contains("offset"))
			result.fields.push_back(wire::Field::U64(tag::Offset, Number(arguments, "offset", true)));
		if (arguments.contains("count"))
		{
			const auto count = Number(arguments, "count", true);
			if (count > 16)
				throw std::invalid_argument("count exceeds 16");
			result.fields.push_back(wire::Field::U64(tag::Length, count));
		}
	}
	else if (name == "simulation_start_recording")
	{
		Keys(arguments, {"connection", "session", "generation", "drain"});
		result.opcode = wire::OpCode::StartRecording;
		if (arguments.contains("drain"))
		{
			if (!arguments.at("drain").is_boolean())
				throw std::invalid_argument("drain must be boolean");
			result.fields.push_back(wire::Field::Bool(tag::RecordingDrain, arguments.at("drain").get<bool>()));
		}
	}
	else if (name == "simulation_stop_recording" || name == "simulation_acknowledge_recording")
	{
		if (name == "simulation_stop_recording")
		{
			Keys(arguments, {"connection", "session", "generation", "recording_id"});
			result.opcode = wire::OpCode::StopRecording;
		}
		else
		{
			Keys(arguments, {"connection", "session", "generation", "recording_id", "through"});
			result.opcode = wire::OpCode::AcknowledgeRecording;
			result.fields.push_back(wire::Field::U64(tag::Offset, Number(arguments, "through", true), true));
		}
		result.fields.push_back(wire::Field::U64(tag::RecordingId, Number(arguments, "recording_id"), true));
	}
	else if (name == "simulation_release_control")
	{
		Keys(arguments, {"connection", "session", "generation"});
		result.opcode = wire::OpCode::ReleaseControl;
	}
	else
		throw std::invalid_argument("Unknown simulation tool");
	// Validate the complete wire payload before the caller admits a mutation.
	(void)wire::EncodeFields(result.fields);
	return result;
}

Json SimulationDocument(const wire::Message& message)
{
	const auto* error = message.Find(tag::Error);
	const auto code = error ? error->AsU32() : std::nullopt;
	if (!code)
		throw std::runtime_error("Runtime omitted its simulation error code");
	if (*code)
	{
		Json failure = {{"ok", false}, {"error", "Runtime rejected the request"}, {"code", *code}};
		if (const auto* detail = message.Find(tag::Detail))
			if (const auto text = detail->AsText())
				failure["detail"] = std::string(*text);
		return failure;
	}
	Json result = {{"ok", true}};
	if (const auto* phase = message.Find(tag::Phase))
		if (const auto text = phase->AsText())
			result["phase"] = std::string(*text);
	for (const auto& [field, name] :
		 {std::pair{tag::TransactionId, "transaction_id"}, {tag::Offset, "offset"}, {tag::Length, "length"}})
		if (const auto* value = message.Find(field))
			if (const auto number = value->AsU64())
				result[name] = std::to_string(*number);
	for (const auto& [field, name] : {std::pair{tag::Released, "released"}, {tag::End, "end"}})
		if (const auto* value = message.Find(field))
			if (const auto flag = value->AsBool())
				result[name] = *flag;
	if (const auto* document = message.Find(tag::Document))
		result["document"] = InspectionJson(document->value);
	return result;
}

Json SimulationReply(const wire::Message& message)
{
	const auto result = SimulationDocument(message);
	return Result(result, !result.at("ok").get<bool>());
}
} // namespace simxr::app::mcp
