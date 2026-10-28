#include "app/mcp/McpPch.h"
#include "app/mcp/McpVideoSave.h"
#include "app/mcp/McpArtifactCalls.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpDebug.h"
#include "app/platform/FileStorage.h"
#include "app/services/Utf8Path.h"
#include "control/ProtocolTypes.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

static std::atomic<unsigned> s_videoSaves{0};
class VideoSavePermit
{
  public:
	VideoSavePermit()
	{
		auto count = s_videoSaves.load();
		for (;;)
		{
			if (count >= 2) throw std::runtime_error("Video save capacity is busy; retry explicitly");
			if (s_videoSaves.compare_exchange_weak(count, count + 1)) break;
		}
	}
	~VideoSavePermit() { --s_videoSaves; }
	VideoSavePermit(const VideoSavePermit&) = delete;
	VideoSavePermit& operator=(const VideoSavePermit&) = delete;
};

static Json Error(const std::string& text)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", Json({{"ok", false}, {"error", text}}).dump()}}})}, {"isError", true}};
}
Json VideoSaveDefinition()
{
	const Json id = {{"oneOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}, {"maxLength", 20}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	return {{"name", "debug_save_video"}, {"description", "Save an inclusive retained frame range as a silent stereo MP4 at an absolute local path. Existing files require overwrite=true."},
		{"inputSchema", {{"type", "object"}, {"properties", {{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"session", id}, {"generation", id}, {"first_frame", id}, {"last_frame", id}, {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 600}}}, {"overwrite", {{"type", "boolean"}}}}},
			{"required", Json::array({"connection", "first_frame", "last_frame", "path"})}, {"additionalProperties", false}}}};
}

std::optional<Json> SaveVideo(McpApplication& app, McpDebug& debug, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	std::filesystem::path temporary;
	try
	{
		if (!arguments.is_object()) throw std::invalid_argument("Arguments must be an object");
		for (auto it = arguments.begin(); it != arguments.end(); ++it)
			if (it.key() != "connection" && it.key() != "session" && it.key() != "generation" && it.key() != "first_frame" && it.key() != "last_frame" && it.key() != "path" && it.key() != "timeout" && it.key() != "overwrite")
				throw std::invalid_argument("Unknown argument: " + it.key());
		if (arguments.contains("session") != arguments.contains("generation"))
			throw std::invalid_argument("Supply both session and generation, or omit both");
		const auto connection = arguments.at("connection").get<std::string>();
		const auto path = arguments.at("path").get<std::string>();
		if (connection.empty() || connection.size() > 256 || path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos)
			throw std::invalid_argument("Invalid connection or path");
		const auto destination = Utf8Path(path);
		auto extension = PathText(destination.extension());
		for (auto& ch : extension) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
		if (!destination.is_absolute() || extension != ".mp4") throw std::invalid_argument("Use an absolute .mp4 destination");
		const bool overwrite = arguments.value("overwrite", false);
		const double timeout = arguments.value("timeout", 120.0);
		if (!std::isfinite(timeout) || timeout <= 0 || timeout > 600) throw std::invalid_argument("timeout must be greater than zero and at most 600 seconds");
		if (!overwrite && std::filesystem::exists(destination)) throw std::runtime_error("Destination already exists");
		if (!cancellation->TryAdmit()) return {};
		VideoSavePermit permit;
		ArtifactCalls calls(app, debug, connection, cancellation, timeout, "Video save");
		std::string ticket;
		try
		{
			Json request = {{"connection", connection}, {"first_frame", arguments.at("first_frame")}, {"last_frame", arguments.at("last_frame")}};
			if (arguments.contains("session"))
			{
				request["session"] = arguments.at("session");
				request["generation"] = arguments.at("generation");
			}
			const auto start = calls.Call("debug_start_video_export", request);
			ticket = start.at("export").get<std::string>();
			std::uint64_t ticketId = 0;
			const auto parsed = std::from_chars(ticket.data(), ticket.data() + ticket.size(), ticketId);
			if (parsed.ec != std::errc{} || parsed.ptr != ticket.data() + ticket.size() || !ticketId) throw std::runtime_error("Invalid video export identity");
			Json info;
			for (;;)
			{
				info = calls.Call("debug_export_info", {{"connection", connection}, {"export", ticket}});
				const auto phase = info.at("phase").get<std::string>();
				if (phase == "complete") break;
				if (phase != "pending") throw std::runtime_error(info.value("detail", "Video export failed"));
				std::this_thread::sleep_for(20ms);
			}
			const auto text = info.at("total_bytes").get<std::string>();
			std::uint64_t total = 0;
			const auto size = std::from_chars(text.data(), text.data() + text.size(), total);
			if (size.ec != std::errc{} || size.ptr != text.data() + text.size() || !total || total > 512ULL * 1024 * 1024 || info.value("media_type", "") != "video/mp4")
				throw std::runtime_error("Invalid MP4 export description");
			temporary = CreatePrivateTemporaryFile(destination);
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output) throw std::runtime_error("Cannot open temporary video file");
			for (std::uint64_t offset = 0; offset < total;)
			{
				calls.Check();
				const auto length = std::min<std::uint64_t>(control::MaxPayload - 4096, total - offset);
				const auto bytes = calls.Read(ticketId, offset, length);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!output) throw std::runtime_error("Cannot write temporary video file");
				offset += bytes.size();
			}
			output.close();
			if (!output) throw std::runtime_error("Cannot close temporary video file");
			calls.Call("debug_release_export", {{"connection", connection}, {"export", ticket}});
			ticket.clear();
			calls.Check();
			if (overwrite) CommitTemporaryFile(temporary, destination);
			else RenameFileWithoutReplacing(temporary, destination);
			temporary.clear();
			const Json result = {{"ok", true}, {"path", PathText(destination)}, {"bytes", std::to_string(total)},
				{"first_frame", arguments.at("first_frame")}, {"last_frame", arguments.at("last_frame")}};
			return Json{{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}, {"isError", false}};
		}
		catch (...)
		{
			if (!ticket.empty()) { try { calls.Call("debug_release_export", {{"connection", connection}, {"export", ticket}}); } catch (...) {} }
			throw;
		}
	}
	catch (const std::exception& error)
	{
		if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
		if (cancellation->Requested()) return {};
		return Error(error.what());
	}
}
}
