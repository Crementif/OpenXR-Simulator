#include "app/mcp/McpPch.h"
#include "control/ExportReader.h"
#include "app/mcp/McpVideoSave.h"
#include "app/mcp/McpImage.h"
#include "app/services/RuntimePanelProtocol.h"
#include "app/mcp/McpArtifactCalls.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpDebug.h"
#include "app/services/ImageExport.h"
#include "app/services/Utf8Path.h"
#include "control/ProtocolTypes.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;
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
		std::uint64_t ticket = 0;
		try
		{
			const auto first = ImageId(arguments, "first_frame"), last = ImageId(arguments, "last_frame");
			if (last < first || last - first >= 1000000)
				throw std::invalid_argument("Select one to one million ordered frames");
			std::vector<wire::Field> request{wire::Field::U64(tag::FirstFrame, first, true),
				wire::Field::U64(tag::LastFrame, last, true)};
			if (arguments.contains("session"))
			{
				request.push_back(wire::Field::U64(tag::Session, ImageId(arguments, "session"), true));
				request.push_back(wire::Field::U64(tag::Generation, ImageId(arguments, "generation"), true));
			}
			ticket = panel::U64(calls.Request(wire::OpCode::StartVideoExport, std::move(request)), tag::ExportId);
			if (!ticket)
				throw std::runtime_error("Invalid video export identity");
			wire::Message info;
			for (;;)
			{
				info = calls.Request(wire::OpCode::ExportInfo, {wire::Field::U64(tag::ExportId, ticket, true)});
				const auto phase = panel::Text(info, tag::Phase);
				if (phase == "complete")
					break;
				if (phase != "pending")
					throw std::runtime_error(panel::Text(info, tag::Detail, "Video export failed"));
				std::this_thread::sleep_for(20ms);
			}
			const auto total = panel::U64(info, tag::TotalBytes);
			if (!total || total > 512ULL * 1024 * 1024 || panel::Text(info, tag::MediaType) != "video/mp4")
				throw std::runtime_error("Invalid MP4 export description");
			SaveExportFile(destination, [&](std::ostream& output)
			{
				control::ExportReader::Read([&](auto opcode, auto fields) { return calls.Request(opcode, std::move(fields)); },
					ticket, total, [&](std::span<const std::byte> bytes)
				{
					output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
					if (!output)
						throw std::runtime_error("Cannot write video export");
				});
			}, overwrite, [&]
			{
				calls.Request(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, ticket, true)});
				ticket = 0;
				calls.Check();
			});
			const Json result = {{"ok", true}, {"path", PathText(destination)}, {"bytes", std::to_string(total)},
				{"first_frame", arguments.at("first_frame")}, {"last_frame", arguments.at("last_frame")}};
			return Json{{"content", Json::array({{{"type", "text"}, {"text", result.dump()}}})}, {"isError", false}};
		}
		catch (...)
		{
			if (ticket) { try { calls.Request(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, ticket, true)}); } catch (...) {} }
			throw;
		}
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested()) return {};
		return Error(error.what());
	}
}
}
