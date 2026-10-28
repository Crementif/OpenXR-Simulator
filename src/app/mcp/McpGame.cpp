#include "app/mcp/McpPch.h"
#include "app/mcp/McpGame.h"
#include "app/mcp/McpSimulation.h"
#include "control/ControlSchema.h"
#include "simxr/game_bridge.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;

static std::uint64_t Number(const Json& object, const char* key, bool zero = false)
{
	const auto it = object.find(key);
	if (it == object.end())
		throw std::invalid_argument(std::string("Missing ") + key);
	std::uint64_t result = 0;
	if (it->is_string())
	{
		const auto text = it->get<std::string>();
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
		if (text.empty() || (text.size() > 1 && text.front() == '0') || parsed.ec != std::errc{} ||
			parsed.ptr != text.data() + text.size())
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
static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}
bool GameHandles(const std::string& name)
{
	static const std::set<std::string> names{"game_bridge_status", "game_bridge_observations", "game_bridge_command",
		"game_bridge_operation"};
	return names.contains(name);
}
Json GameDefinitions()
{
	const Json text = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
	const Json id = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json cursor = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
		{{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const Json common = {{"connection", text}, {"session", id}, {"generation", id}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	Json result = Json::array();
	result.push_back(tool("game_bridge_status", "Read the cooperating game's identity, supported commands and latest telemetry.", common, Json::array({"connection"})));
	auto observations = common;
	observations["offset"] = cursor;
	observations["count"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 16}};
	result.push_back(tool("game_bridge_observations", "Read bounded game telemetry and lifecycle events; check gap before assuming continuity.", observations, Json::array({"connection"})));
	auto command = common;
	command["game"] = id;
	command["command_id"] = id;
	command["kind"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}};
	command["payload"] = {{"type", "string"}, {"maxLength", SIMXR_GAME_PAYLOAD_CAPACITY}};
	result.push_back(tool("game_bridge_command", "Queue a supported game command at a safe update point. Poll its operation; queued does not mean applied.", command, Json::array({"connection", "game", "command_id", "kind"})));
	auto operation = common;
	operation["game"] = id;
	operation["operation"] = id;
	result.push_back(tool("game_bridge_operation", "Read this connection's game command acknowledgement.", operation, Json::array({"connection", "game", "operation"})));
	return result;
}
GameRequest PrepareGame(const std::string& name, const Json& arguments, std::uint64_t session, std::uint64_t generation)
{
	if (!arguments.is_object())
		throw std::invalid_argument("Arguments must be an object");
	if (arguments.contains("session") != arguments.contains("generation"))
		throw std::invalid_argument("Supply both session and generation, or omit both");
	std::set<std::string_view> allowed{"connection", "session", "generation"};
	GameRequest request{wire::OpCode::GameStatus, {}};
	if (arguments.contains("session"))
	{
		if (!session || !generation || Number(arguments, "session") != session ||
			Number(arguments, "generation") != generation)
			throw std::invalid_argument("Session or generation does not match this connection");
		request.fields.push_back(wire::Field::U64(tag::Session, session, true));
		request.fields.push_back(wire::Field::U64(tag::Generation, generation, true));
	}
	if (name == "game_bridge_observations")
	{
		allowed.insert("offset");
		allowed.insert("count");
		request.opcode = wire::OpCode::ReadGameObservations;
		if (arguments.contains("offset"))
			request.fields.push_back(wire::Field::U64(tag::Offset, Number(arguments, "offset", true)));
		if (arguments.contains("count"))
		{
			if (!arguments["count"].is_number_integer())
				throw std::invalid_argument("count must be an integer");
			const auto count = Number(arguments, "count", true);
			if (count > 16)
				throw std::invalid_argument("count exceeds 16");
			request.fields.push_back(wire::Field::U64(tag::Length, count));
		}
	}
	else if (name == "game_bridge_command" || name == "game_bridge_operation")
	{
		allowed.insert("game");
		request.fields.push_back(wire::Field::U64(tag::GameId, Number(arguments, "game"), true));
		if (name == "game_bridge_command")
		{
			allowed.insert("command_id");
			allowed.insert("kind");
			allowed.insert("payload");
			if (!arguments.contains("kind") || !arguments["kind"].is_number_integer())
				throw std::invalid_argument("kind must be an integer");
			const auto kind = Number(arguments, "kind");
			if (kind > 3)
				throw std::invalid_argument("kind must be 1, 2 or 3");
			const auto payload = arguments.value("payload", std::string{});
			if (payload.size() > SIMXR_GAME_PAYLOAD_CAPACITY || payload.find('\0') != std::string::npos)
				throw std::invalid_argument("Game payload exceeds 4096 UTF-8 bytes or contains NUL");
			request.opcode = wire::OpCode::SubmitGame;
			request.fields.push_back(wire::Field::U64(tag::CommandId, Number(arguments, "command_id"), true));
			request.fields.push_back(wire::Field::U32(tag::GameKind, static_cast<std::uint32_t>(kind), true));
			request.fields.push_back(wire::Field::Text(tag::Detail, payload, true));
		}
		else
		{
			allowed.insert("operation");
			request.opcode = wire::OpCode::GameOperation;
			request.fields.push_back(wire::Field::U64(tag::OperationId, Number(arguments, "operation"), true));
		}
	}
	else if (name != "game_bridge_status")
		throw std::invalid_argument("Unknown game bridge tool");
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
		if (!allowed.contains(it.key()))
			throw std::invalid_argument("Unknown argument: " + it.key());
	(void)wire::EncodeFields(request.fields);
	return request;
}
Json GameReply(const wire::Message& message, wire::OpCode opcode)
{
	const auto* error = message.Find(tag::Error);
	const auto code = error ? error->AsU32() : std::nullopt;
	if (!code)
		throw std::runtime_error("Runtime omitted its game bridge error code");
	if (*code)
	{
		Json failure = {{"ok", false}, {"error", "Runtime rejected the request"}, {"code", *code}};
		if (const auto* detail = message.Find(tag::Detail))
			if (const auto value = detail->AsText())
				failure["detail"] = std::string(*value);
		return Result(failure, true);
	}
	const auto* document = message.Find(tag::Document);
	if (!document)
		throw std::runtime_error("Runtime omitted game bridge details");
	if (opcode == wire::OpCode::GameStatus || opcode == wire::OpCode::SubmitGame || opcode == wire::OpCode::GameOperation)
	{
		const auto* game = message.Find(tag::GameId);
		if (!game || !game->AsU64() || (opcode != wire::OpCode::GameStatus && !*game->AsU64()))
			throw std::runtime_error("Runtime omitted game identity");
	}
	if (opcode == wire::OpCode::SubmitGame || opcode == wire::OpCode::GameOperation)
	{
		const auto* operation = message.Find(tag::OperationId);
		const auto* phase = message.Find(tag::Phase);
		static const std::set<std::string_view> phases{"queued", "delivered", "complete", "failed", "cancelled", "interrupted"};
		if (!operation || !operation->AsU64().value_or(0) || !phase || !phase->AsText() || !phases.contains(*phase->AsText()))
			throw std::runtime_error("Runtime omitted game operation details");
	}
	Json result = {{"ok", true}};
	for (const auto& [field, name] : {std::pair{tag::GameId, "game_id"}, {tag::OperationId, "operation_id"},
			{tag::Session, "session"}, {tag::Generation, "generation"}, {tag::ClockRevision, "clock_revision"}})
		if (const auto* value = message.Find(field))
			if (const auto number = value->AsU64())
				result[name] = std::to_string(*number);
	if (const auto* phase = message.Find(tag::Phase))
		if (const auto text = phase->AsText())
			result["phase"] = std::string(*text);
	if (const auto* domain = message.Find(tag::ClockDomain))
		if (const auto value = domain->AsU32())
			result["clock_domain"] = *value;
	for (const auto& [field, name] : {std::pair{tag::NativeTime, "native_time_ns"},
			{tag::RuntimeTimeBefore, "runtime_time_before_ns"}, {tag::RuntimeTimeAfter, "runtime_time_after_ns"},
			{tag::ClockResolution, "clock_resolution_ns"}})
		if (const auto* value = message.Find(field))
			if (const auto number = value->AsI64())
				result[name] = std::to_string(*number);
	result["document"] = InspectionJson(document->value);
	return Result(result);
}
} // namespace simxr::app::mcp
