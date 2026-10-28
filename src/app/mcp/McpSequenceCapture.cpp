#include "app/mcp/McpPch.h"
#include "app/mcp/McpSequenceCapture.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpArtifactCalls.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpImageEncode.h"
#include "app/mcp/McpIncident.h"
#include "app/mcp/McpSequence.h"
#include "app/platform/FileStorage.h"
#include "app/services/Utf8Path.h"
#include "control/ProtocolTypes.h"
#include "graphics/ColorPixels.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
using namespace std::chrono_literals;
static std::mutex s_sequenceCaptureMutex;
static std::set<std::pair<const McpDebug*, std::string>> s_sequenceCaptures;

class SequenceCapturePermit
{
  public:
	SequenceCapturePermit(const McpDebug& debug, std::string connection) : m_key(&debug, std::move(connection))
	{
		std::lock_guard lock(s_sequenceCaptureMutex);
		if (s_sequenceCaptures.size() >= 2 || !s_sequenceCaptures.insert(m_key).second)
			throw std::runtime_error("Sequence capture capacity or connection is busy; retry explicitly");
	}
	~SequenceCapturePermit()
	{
		std::lock_guard lock(s_sequenceCaptureMutex);
		s_sequenceCaptures.erase(m_key);
	}
	SequenceCapturePermit(const SequenceCapturePermit&) = delete;
	SequenceCapturePermit& operator=(const SequenceCapturePermit&) = delete;
  private:
	std::pair<const McpDebug*, std::string> m_key;
};

Json SequenceCaptureDefinition()
{
	const Json id = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	return {{"name", "capture_pose_sequence"},
		{"description", "Run a pose sequence and automatically save its captured stereo frames and manifest in a new directory. Waits for completion; bounded to 32 captures and 128 MiB. Cancel or timeout closes the debugger connection."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"session", id}, {"generation", id}, {"program_id", id},
			{"program", {{"type", "string"}, {"description", "Optional JSONL program; omit for an already uploaded program."}}},
			{"loop", {{"type", "boolean"}}}, {"directory", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
			{"max_captures", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 600}}}}},
			{"required", Json::array({"connection", "program_id", "directory"})}, {"additionalProperties", false}}}};
}
static Json Result(const Json& data, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", data.dump()}}})}, {"isError", error}};
}
static void WriteFile(const std::filesystem::path& destination, std::span<const std::byte> bytes, bool overwrite)
{
	std::filesystem::path temporary;
	try
	{
		temporary = CreatePrivateTemporaryFile(destination);
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("Cannot open temporary sequence file");
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		output.close();
		if (!output)
			throw std::runtime_error("Cannot write or close temporary sequence file");
		if (overwrite)
			CommitTemporaryFile(temporary, destination);
		else
			RenameFileWithoutReplacing(temporary, destination);
		temporary.clear();
	}
	catch (...)
	{
		if (!temporary.empty())
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
		}
		throw;
	}
}
static void WriteManifest(const std::filesystem::path& directory, const Json& manifest)
{
	const auto content = manifest.dump(2);
	WriteFile(directory / "manifest.json", std::as_bytes(std::span(content.data(), content.size())), true);
}
static std::uint64_t Id(const Json& value, const char* field, bool zero = false)
{
	if (zero && value == "0")
		return 0;
	return IncidentIdentifier(value, field);
}
struct StereoEncoded
{
	std::vector<std::byte> bytes;
	std::uint32_t width = 0, height = 0;
	bool lossy = false;
};
static StereoEncoded StereoImage(ArtifactCalls& calls, const std::string& connection,
	const std::string& reader, const std::string& frame)
{
	ImageEncodingPermit permit;
	std::vector<std::byte> sheet;
	std::uint32_t width = 0, height = 0;
	bool lossy = false;
	for (unsigned eye = 0; eye < 2; ++eye)
	{
		const auto started = calls.Call("debug_start_export", {{"connection", connection}, {"image", reader}, {"layer", eye}});
		const auto ticket = started.at("export").get<std::string>();
		const auto ticketId = Id(ticket, "export");
		try
		{
			Json info;
			for (;;)
			{
				info = calls.Call("debug_export_info", {{"connection", connection}, {"export", ticket}});
				const auto phase = info.value("phase", "");
				if (phase == "complete")
					break;
				if (phase != "pending")
					throw std::runtime_error("Sequence image export failed: " + phase);
				calls.Check();
				std::this_thread::sleep_for(10ms);
			}
			if (info.at("frame").at("frame_id") != frame || info.at("layer") != eye)
				throw std::runtime_error("Sequence image export changed frame or eye");
			const auto& description = info.at("image_description");
			const auto w = description.at("width").get<std::uint32_t>();
			const auto h = description.at("height").get<std::uint32_t>();
			const auto format = description.at("format").get<std::uint32_t>();
			lossy |= format != static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm) &&
				format != static_cast<std::uint32_t>(ColorFormat::Rgba8Srgb) &&
				format != static_cast<std::uint32_t>(ColorFormat::Bgra8Unorm) &&
				format != static_cast<std::uint32_t>(ColorFormat::Bgra8Srgb);
			const auto pixelBytes = ColorPixelBytes(format);
			const auto total = Id(info.at("total_bytes"), "export size");
			if (!pixelBytes || !w || !h || w > 2048 || h > 4096 || description.at("array_layers") != 1 ||
				std::uint64_t{w} * h > MaxImageInput / (pixelBytes * 2) || total != std::uint64_t{w} * h * pixelBytes ||
				(eye && (w != width || h != height)))
				throw std::runtime_error("Sequence stereo image exceeds its supported shape or size");
			if (!eye)
			{
				width = w;
				height = h;
				if (std::uint64_t{width} * height > MaxImageInput / 8)
					throw std::runtime_error("Sequence stereo image exceeds the encoding budget");
				sheet.resize(std::size_t{width} * height * 8);
			}
			std::vector<std::byte> raw;
			raw.reserve(static_cast<std::size_t>(total));
			while (raw.size() < total)
			{
				const auto offset = raw.size();
				const auto length = std::min<std::uint64_t>(control::MaxPayload - 4096, total - offset);
				const auto part = calls.Read(ticketId, offset, length);
				raw.insert(raw.end(), part.begin(), part.end());
			}
			for (std::size_t i = 0; i < std::size_t{width} * height; ++i)
			{
				const auto color = DecodeColorPixel(format, std::span(raw).subspan(i * pixelBytes, pixelBytes));
				const auto destination = ((i / width) * width * 2 + eye * width + i % width) * 4;
				for (std::size_t channel = 0; channel < 4; ++channel)
				{
					const auto value = std::isnan(color[channel]) ? 0.0f : std::clamp(color[channel], 0.0f, 1.0f);
					sheet[destination + channel] = static_cast<std::byte>(std::lround(value * 255.0f));
				}
			}
			calls.Call("debug_release_export", {{"connection", connection}, {"export", ticket}});
		}
		catch (...)
		{
			try { calls.Call("debug_release_export", {{"connection", connection}, {"export", ticket}}); }
			catch (...) {}
			throw;
		}
	}
	calls.Check();
	auto encoded = EncodeImage(sheet, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), width * 2, height, true, 85);
	return {std::move(encoded.bytes), width * 2, height, lossy};
}
void ValidateSequenceCapture(const Json& arguments)
{
	if (!arguments.is_object())
		throw std::invalid_argument("Arguments must be an object");
	for (auto item = arguments.begin(); item != arguments.end(); ++item)
		if (item.key() != "connection" && item.key() != "session" && item.key() != "generation" &&
			item.key() != "program_id" && item.key() != "program" && item.key() != "loop" &&
			item.key() != "directory" && item.key() != "timeout" && item.key() != "max_captures")
			throw std::invalid_argument("Unknown argument: " + item.key());
	if (arguments.contains("session") != arguments.contains("generation"))
		throw std::invalid_argument("Supply both session and generation, or omit both");
	const auto connection = arguments.at("connection").get<std::string>();
	const auto text = arguments.at("directory").get<std::string>();
	if (connection.empty() || connection.size() > 256 || connection.find('\0') != std::string::npos ||
		text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos)
		throw std::invalid_argument("Invalid connection or directory");
	const auto root = Utf8Path(text);
	if (!root.is_absolute() || !std::filesystem::is_directory(root))
		throw std::invalid_argument("directory must be an existing absolute directory");
	const auto limit = arguments.value("max_captures", Json(32));
	if (!limit.is_number_integer() || limit < 1 || limit > 32)
		throw std::invalid_argument("max_captures must be an integer from 1 to 32");
	const double timeout = arguments.value("timeout", 120.0);
	if (!std::isfinite(timeout) || timeout <= 0 || timeout > 600)
		throw std::invalid_argument("timeout must be greater than zero and at most 600 seconds");
	Json start = {{"connection", connection}, {"program_id", arguments.at("program_id")}};
	for (const auto* key : {"session", "generation", "program", "loop"})
		if (arguments.contains(key))
			start[key] = arguments.at(key);
	(void)PrepareSequence("run_pose_sequence", start, 1, 1);
}

std::optional<Json> CaptureSequence(McpApplication& app, McpDebug& debug, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation, const std::function<void(const Json&)>& progress)
{
	std::filesystem::path output;
	Json manifest = {{"ok", false}, {"phase", "preparing"}, {"captures", Json::array()},
		{"incomplete", false}, {"history_gap", false}};
	try
	{
		ValidateSequenceCapture(arguments);
		const auto connection = arguments.at("connection").get<std::string>();
		const auto text = arguments.at("directory").get<std::string>();
		const auto root = Utf8Path(text);
		const auto limit = arguments.value("max_captures", 32);
		const double timeout = arguments.value("timeout", 120.0);
		Json start = {{"connection", connection}, {"program_id", arguments.at("program_id")}};
		for (const auto* key : {"session", "generation", "program", "loop"})
			if (arguments.contains(key))
				start[key] = arguments.at(key);
		if (!cancellation->TryAdmit())
			return {};
		SequenceCapturePermit permit(debug, connection);
		const auto candidate = root / ("sequence-" + NewMcpIdentifier());
		if (!std::filesystem::create_directory(candidate))
			throw std::runtime_error("Cannot create a unique sequence output directory");
		output = candidate;
		manifest["directory"] = PathText(output);
		manifest["program_id"] = arguments.at("program_id");
		WriteManifest(output, manifest);
		if (progress) progress(manifest);
		ArtifactCalls calls(app, debug, connection, cancellation, timeout, "Sequence capture");
		std::string sequence;
		std::map<std::uint64_t, std::string> pending;
		std::set<std::uint64_t> seen;
		try
		{
			const auto started = calls.Call("run_pose_sequence", start);
			sequence = started.at("sequence_id").get<std::string>();
			Id(sequence, "sequence");
			if (!started.at("document").at("active").get<bool>())
				throw std::runtime_error("Sequence is already complete; use a new program ID");
			manifest["sequence_id"] = sequence;
			manifest["phase"] = "running";
			WriteManifest(output, manifest);
			if (progress) progress(manifest);
			std::uint64_t cursor = 0, bytes = 0;
			for (;;)
			{
				calls.Check();
				const auto result = calls.Call("get_pose_sequence", {{"connection", connection}, {"sequence_id", sequence},
					{"after", std::to_string(cursor)}, {"count", 64}});
				const auto& page = result.at("document");
				if (page.at("status").is_null())
					throw std::runtime_error("Sequence was replaced while saving captures");
				const auto& status = page.at("status");
				if (page.at("history_gap").get<bool>())
				{
					manifest["incomplete"] = true;
					manifest["history_gap"] = true;
				}
				cursor = Id(page.at("next_frame"), "sequence cursor", true);
				manifest["status"] = status;
				for (const auto& receipt : page.at("captures"))
				{
					const auto frame = receipt.at("frame").get<std::string>();
					Id(frame, "capture frame");
					const auto capture = Id(receipt.at("capture_id"), "capture", true);
					if (!capture)
					{
						manifest["incomplete"] = true;
						throw std::runtime_error("A sequence capture could not be admitted: " + receipt.value("admission", "unknown"));
					}
					if (!seen.contains(capture))
						pending.emplace(capture, frame);
				}
				for (const auto& receipt : page.at("held_captures"))
				{
					const auto capture = Id(receipt.at("capture_id"), "capture");
					const auto frame = receipt.at("frame").get<std::string>();
					Id(frame, "capture frame");
					if (!seen.contains(capture))
						pending.emplace(capture, frame);
				}
				if (pending.size() > 64)
					throw std::runtime_error("Sequence capture ownership exceeded its bound");
				for (auto item = pending.begin(); item != pending.end();)
				{
					if (manifest["captures"].size() >= static_cast<std::size_t>(limit))
					{
						if (status.at("active").get<bool>())
							calls.Call("stop_pose_sequence", {{"connection", connection}, {"sequence_id", sequence}});
						manifest["phase"] = "capture_limit";
						manifest["incomplete"] = true;
						break;
					}
					const auto capture = std::to_string(item->first);
					const auto current = calls.Call("debug_capture", {{"connection", connection}, {"capture", capture}});
					const auto phase = current.value("phase", "");
					if (phase == "reserved" || phase == "issued" || phase == "begun")
					{
						++item;
						continue;
					}
					if ((phase != "ready" && phase != "gpu_pending") || current.at("frame").at("frame_id") != item->second)
						throw std::runtime_error("Sequence capture is unavailable or changed frame: " + phase);
					Json retained;
					try
					{
						retained = calls.Call("debug_retain_image", {{"connection", connection}, {"capture", capture}});
					}
					catch (const ArtifactError& error)
					{
						if (error.code != static_cast<std::uint32_t>(control::ErrorCode::Busy))
							throw;
						++item;
						continue;
					}
					const auto reader = retained.at("image").get<std::string>();
					Id(reader, "image reader");
					try
					{
						const auto info = calls.Call("debug_image_info", {{"connection", connection}, {"image", reader}});
						if (info.at("frame").at("frame_id") != item->second || info.at("frame") != current.at("frame"))
							throw std::runtime_error("Sequence reader changed frame identity");
						const auto encoded = StereoImage(calls, connection, reader, item->second);
						if (bytes + encoded.bytes.size() > 128ULL * 1024 * 1024)
							throw std::runtime_error("Sequence output exceeds its 128 MiB disk budget");
						calls.Check();
						const auto path = output / ("frame-" + item->second + ".png");
						WriteFile(path, encoded.bytes, false);
						bytes += encoded.bytes.size();
						manifest["captures"].push_back({{"frame", item->second}, {"capture_id", capture},
							{"path", PathText(path)}, {"bytes", std::to_string(encoded.bytes.size())}, {"frame_key", current.at("frame")},
							{"width", encoded.width}, {"height", encoded.height}, {"lossy", encoded.lossy}});
						WriteManifest(output, manifest);
						if (progress) progress(manifest);
						calls.Call("debug_release_image", {{"connection", connection}, {"image", reader}});
						calls.Call("release_pose_sequence_capture", {{"connection", connection}, {"capture", capture}});
					}
					catch (...)
					{
						try { calls.Call("debug_release_image", {{"connection", connection}, {"image", reader}}); }
						catch (...) {}
						throw;
					}
					seen.insert(item->first);
					item = pending.erase(item);
					if (manifest["captures"].size() >= static_cast<std::size_t>(limit) && status.at("active").get<bool>())
					{
						calls.Call("stop_pose_sequence", {{"connection", connection}, {"sequence_id", sequence}});
						manifest["phase"] = "capture_limit";
						manifest["incomplete"] = true;
						break;
					}
				}
				if (manifest["phase"] == "capture_limit")
				{
					const auto final = calls.Call("get_pose_sequence", {{"connection", connection}, {"sequence_id", sequence}});
					if (!final.at("document").at("status").is_null())
						manifest["status"] = final.at("document").at("status");
					manifest["unsaved_captures"] = final.at("document").at("held_captures");
					for (const auto& held : manifest["unsaved_captures"])
						calls.Call("release_pose_sequence_capture", {{"connection", connection}, {"capture", held.at("capture_id")}});
					break;
				}
				if (!status.at("active").get<bool>() && !page.at("more").get<bool>() && pending.empty() &&
					cursor >= Id(status.at("last_frame"), "last frame", true))
				{
					const auto statusResult = status.at("result").get<std::string>();
					manifest["phase"] = statusResult == "applied" ? "complete" : statusResult;
					manifest["incomplete"] = manifest["incomplete"].get<bool>() || statusResult != "applied";
					break;
				}
				std::this_thread::sleep_for(10ms);
			}
			manifest["ok"] = !manifest["incomplete"].get<bool>();
			WriteManifest(output, manifest);
			if (progress) progress(manifest);
			return Result(manifest, !manifest["ok"].get<bool>());
		}
		catch (...)
		{
			if (!sequence.empty())
			{
				try { calls.Call("stop_pose_sequence", {{"connection", connection}, {"sequence_id", sequence}}); }
				catch (...) {}
				try
				{
					const auto final = calls.Call("get_pose_sequence", {{"connection", connection}, {"sequence_id", sequence}});
					manifest["unsaved_captures"] = final.at("document").at("held_captures");
					for (const auto& held : manifest["unsaved_captures"])
						calls.Call("release_pose_sequence_capture", {{"connection", connection}, {"capture", held.at("capture_id")}});
				}
				catch (...)
				{
					// If ownership cannot be enumerated, closing the source is the only
					// reliable way to retire all captures, including newly admitted ones.
					try
					{
						(void)debug.Call(app, "debug_disconnect", {{"connection", connection}},
							std::make_shared<McpCancellation>());
					}
					catch (...) {}
				}
			}
			throw;
		}
	}
	catch (const std::exception& error)
	{
		manifest["ok"] = false;
		manifest["phase"] = cancellation->Requested() ? "cancelled" : "failed";
		manifest["incomplete"] = true;
		manifest["error"] = error.what();
		std::error_code directoryError;
		if (!output.empty() && std::filesystem::is_directory(output, directoryError))
		{
			try { WriteManifest(output, manifest); if (progress) progress(manifest); }
			catch (const std::exception& failure) { manifest["manifest_error"] = failure.what(); }
		}
		if (cancellation->Requested())
			return {};
		return Result(manifest, true);
	}
}
}
