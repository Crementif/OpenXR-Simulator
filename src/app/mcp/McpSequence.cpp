#include "Pch.h"
#include "app/mcp/McpSequence.h"
#include "app/mcp/McpSimulation.h"
#include "control/ControlSchema.h"
#include "control/SequenceControl.h"

#include <charconv>
#include <set>
#include <stdexcept>

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
	const auto& value = object.at(key);
	if (value.is_number_integer() && value >= (allowZero ? 0 : 1) && value <= ((1ULL << 53) - 1))
		return value.get<std::uint64_t>();
	if (!value.is_string())
		throw std::invalid_argument(std::string(key) + " requires a decimal string or safe integer");
	const auto& text = value.get_ref<const std::string&>();
	if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0'))
		throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
	std::uint64_t number = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || (!allowZero && !number))
		throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
	return number;
}
static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

bool SequenceHandles(const std::string& name)
{
	static const std::set<std::string> names{"upload_pose_sequence", "run_pose_sequence", "get_pose_sequence",
		"stop_pose_sequence", "release_pose_sequence_capture"};
	return names.contains(name);
}
Json SequenceDefinitions()
{
	const Json id = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json zero = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
		{{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const Json common = {{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
		{"session", id}, {"generation", id}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	Json result = Json::array();
	auto upload = common;
	upload["program_id"] = id;
	upload["offset"] = zero;
	upload["total_bytes"] = zero;
	upload["data"] = {{"type", "string"}, {"description", "UTF-8 JSONL chunk, at most 256 KiB; offsets and total are byte counts."}};
	result.push_back(tool("upload_pose_sequence", "Upload one ordered JSONL program chunk without executing it. Identical chunk retries are safe; total is limited to 16 MiB.", upload,
		Json::array({"connection", "program_id", "offset", "total_bytes", "data"})));
	auto run = common;
	run["program_id"] = id;
	run["loop"] = {{"type", "boolean"}};
	run["program"] = {{"type", "string"}, {"description", "Optional complete UTF-8 JSONL program, at most 512 KiB. Larger programs can be staged with upload_pose_sequence."}};
	result.push_back(tool("run_pose_sequence", "Upload an optional JSONL program and start it at an ordinary frame boundary. Repeating the same program and loop returns its current execution, not a restart.", run,
		Json::array({"connection", "program_id"})));
	auto status = common;
	status["sequence_id"] = id;
	status["after"] = zero;
	status["count"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}};
	result.push_back(tool("get_pose_sequence", "Read an owned execution and its paged exact-frame capture receipts; held captures survive history eviction.", status,
		Json::array({"connection", "sequence_id"})));
	auto stop = common;
	stop["sequence_id"] = id;
	result.push_back(tool("stop_pose_sequence", "Stop the identified current owned sequence; retained captures still require release.", stop,
		Json::array({"connection", "sequence_id"})));
	auto release = common;
	release["capture"] = id;
	result.push_back(tool("release_pose_sequence_capture", "Release a sequence-owned capture after retaining or exporting its image.", release,
		Json::array({"connection", "capture"})));
	return result;
}
SequenceRequest PrepareSequence(const std::string& name, const Json& args, std::uint64_t session, std::uint64_t generation)
{
	SequenceRequest result{wire::OpCode::ReadSequence,
		{wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true)}};
	if (name != "release_pose_sequence_capture")
		result.fields.push_back(wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Mcp)));
	if (name == "upload_pose_sequence")
	{
		Keys(args, {"connection", "session", "generation", "program_id", "offset", "total_bytes", "data"});
		const auto offset = Number(args, "offset", true), total = Number(args, "total_bytes", true);
		if (!args.at("data").is_string())
			throw std::invalid_argument("data must be a UTF-8 string");
		const auto& data = args.at("data").get_ref<const std::string&>();
		if (total > wire::SequenceControl::MaxBytes || offset > total || data.size() > wire::SequenceControl::ChunkBytes ||
			data.size() > total - offset || (data.empty() && total) || data.find('\0') != std::string::npos)
			throw std::invalid_argument("Invalid bounded sequence chunk, byte offset or total");
		result.opcode = wire::OpCode::UploadSequence;
		result.fields.push_back(wire::Field::U64(tag::ProgramId, Number(args, "program_id"), true));
		result.fields.push_back(wire::Field::U64(tag::Offset, offset, true));
		result.fields.push_back(wire::Field::U64(tag::TotalBytes, total, true));
		result.fields.push_back(wire::Field::Bytes(tag::Bytes,
			std::as_bytes(std::span(data.data(), data.size())), true));
	}
	else if (name == "run_pose_sequence")
	{
		Keys(args, {"connection", "session", "generation", "program_id", "loop", "program"});
		if (args.contains("loop") && !args["loop"].is_boolean())
			throw std::invalid_argument("loop must be boolean");
		if (args.contains("program") && (!args["program"].is_string() ||
			args["program"].get_ref<const std::string&>().size() > 512 * 1024 ||
			args["program"].get_ref<const std::string&>().find('\0') != std::string::npos))
			throw std::invalid_argument("program must be JSONL text of at most 512 KiB without NUL bytes");
		result.opcode = wire::OpCode::StartSequence;
		result.fields.push_back(wire::Field::U64(tag::ProgramId, Number(args, "program_id"), true));
		result.fields.push_back(wire::Field::Bool(tag::Loop, args.value("loop", false)));
	}
	else if (name == "get_pose_sequence" || name == "stop_pose_sequence")
	{
		if (name == "get_pose_sequence")
			Keys(args, {"connection", "session", "generation", "sequence_id", "after", "count"});
		else
			Keys(args, {"connection", "session", "generation", "sequence_id"});
		result.opcode = name == "get_pose_sequence" ? wire::OpCode::ReadSequence : wire::OpCode::StopSequence;
		result.fields.push_back(wire::Field::U64(tag::SequenceId, Number(args, "sequence_id"), true));
		if (name == "get_pose_sequence")
		{
			if (args.contains("after"))
				result.fields.push_back(wire::Field::U64(tag::Offset, Number(args, "after", true)));
			if (args.contains("count"))
			{
				const auto count = Number(args, "count");
				if (!args["count"].is_number_integer() || count > 64)
					throw std::invalid_argument("count must be an integer from 1 to 64");
				result.fields.push_back(wire::Field::U64(tag::Length, count));
			}
		}
	}
	else if (name == "release_pose_sequence_capture")
	{
		Keys(args, {"connection", "session", "generation", "capture"});
		result.opcode = wire::OpCode::ReleaseSequenceCapture;
		result.fields.push_back(wire::Field::U64(tag::CaptureId, Number(args, "capture"), true));
	}
	else
		throw std::invalid_argument("Unknown sequence operation");
	return result;
}
static void DecimalIdentities(Json& value)
{
	if (value.is_array())
	{
		for (auto& item : value)
			DecimalIdentities(item);
	}
	else if (value.is_object())
	{
		static const std::set<std::string_view> names{"frame", "origin", "last_frame", "next_frame", "oldest_frame",
			"current_sequence", "program_id", "sequence_id", "capture_id", "id"};
		for (auto it = value.begin(); it != value.end(); ++it)
		{
			if (names.contains(it.key()) && it->is_number_unsigned())
				*it = std::to_string(it->get<std::uint64_t>());
			else
				DecimalIdentities(*it);
		}
	}
}
Json SequenceReply(const wire::Message& response, const SequenceRequest& request)
{
	const auto* error = response.Find(tag::Error);
	if (!error || !error->AsU32())
		throw std::runtime_error("Runtime omitted sequence error code");
	if (const auto code = *error->AsU32())
	{
		Json failure = {{"ok", false}, {"error", "Runtime rejected the sequence request"}, {"code", code}};
		if (const auto* detail = response.Find(tag::Detail))
			if (const auto text = detail->AsText())
				failure["detail"] = std::string(*text);
		return Result(failure, true);
	}
	Json result = {{"ok", true}};
	if (const auto* id = response.Find(tag::SequenceId))
	{
		if (!id->AsU64() || !*id->AsU64())
			throw std::runtime_error("Runtime returned an invalid sequence identity");
		result["sequence_id"] = std::to_string(*id->AsU64());
	}
	if (const auto* released = response.Find(tag::Released))
	{
		if (!released->AsBool())
			throw std::runtime_error("Runtime returned an invalid sequence release result");
		result["released"] = *released->AsBool();
	}
	const auto* document = response.Find(tag::Document);
	if (request.opcode != wire::OpCode::ReleaseSequenceCapture && !document)
		throw std::runtime_error("Runtime omitted the sequence document");
	if (document)
	{
		result["document"] = InspectionJson(document->value);
		DecimalIdentities(result["document"]);
	}
	const auto field = [&](std::uint16_t wanted)
	{
		const auto found = std::find_if(request.fields.begin(), request.fields.end(),
			[wanted](const auto& item) { return item.tag == wanted; });
		if (found == request.fields.end() || !found->AsU64())
			throw std::runtime_error("Sequence request omitted an identity");
		return *found->AsU64();
	};
	if (request.opcode == wire::OpCode::UploadSequence)
	{
		const auto& data = result.at("document");
		const auto total = field(tag::TotalBytes), offset = field(tag::Offset);
		const auto bytes = std::find_if(request.fields.begin(), request.fields.end(),
			[](const auto& item) { return item.tag == tag::Bytes; });
		if (bytes == request.fields.end() || Number(data, "program_id") != field(tag::ProgramId) ||
			Number(data, "total", true) != total || Number(data, "received", true) < offset + bytes->value.size() ||
			Number(data, "received", true) > total || data.at("complete") != (Number(data, "received", true) == total))
			throw std::runtime_error("Runtime returned an inconsistent sequence upload acknowledgement");
	}
	else if (request.opcode == wire::OpCode::StartSequence)
	{
		if (!result.contains("sequence_id") || Number(result.at("document"), "id") != Number(result, "sequence_id"))
			throw std::runtime_error("Runtime returned an inconsistent sequence execution identity");
	}
	else if (request.opcode == wire::OpCode::ReadSequence &&
		Number(result.at("document"), "sequence_id") != field(tag::SequenceId))
		throw std::runtime_error("Runtime returned a different sequence history");
	else if (request.opcode == wire::OpCode::StopSequence &&
		Number(result.at("document"), "id") != field(tag::SequenceId))
		throw std::runtime_error("Runtime returned a different stopped sequence");
	else if (request.opcode == wire::OpCode::ReleaseSequenceCapture && !result.contains("released"))
		throw std::runtime_error("Runtime omitted sequence capture release status");
	return Result(result);
}
}
