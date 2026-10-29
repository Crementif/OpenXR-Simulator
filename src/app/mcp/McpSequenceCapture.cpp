#include "app/mcp/McpPch.h"
#include "control/ExportReader.h"
#include "app/mcp/McpSequenceCapture.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpArtifactCalls.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpDebug.h"
#include "app/services/ImageExport.h"
#include "app/mcp/McpImage.h"
#include "app/services/RuntimePanelProtocol.h"
#include "control/ImageInspection.h"
#include "app/mcp/McpSequence.h"
#include "app/services/Utf8Path.h"
#include "control/ProtocolTypes.h"
#include "graphics/ColorPixels.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;
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
	return {
		{"name", "capture_pose_sequence"},
		{"description",
		 "Run a pose sequence and automatically save its captured stereo frames and manifest in a new directory. Waits "
		 "for completion; bounded to 32 captures and 128 MiB. Cancel or timeout closes the debugger connection."},
		{"inputSchema",
		 {{"type", "object"},
		  {"properties",
		   {{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"session", id},
			{"generation", id},
			{"program_id", id},
			{"program",
			 {{"type", "string"}, {"description", "Optional JSONL program; omit for an already uploaded program."}}},
			{"loop", {{"type", "boolean"}}},
			{"directory", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
			{"max_captures", {{"type", "integer"}, {"minimum", 1}, {"maximum", 32}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 600}}}}},
		  {"required", Json::array({"connection", "program_id", "directory"})},
		  {"additionalProperties", false}}}};
}

static Json Result(const Json& data, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", data.dump()}}})}, {"isError", error}};
}

static void WriteManifest(const std::filesystem::path& directory, Json& manifest)
{
	DecimalSequenceIdentities(manifest);
	const auto content = manifest.dump(2);
	SaveExportFile(directory / "manifest.json", std::as_bytes(std::span(content.data(), content.size())), true);
}

struct StereoEncoded
{
	std::vector<std::byte> bytes;
	std::uint32_t width = 0, height = 0;
	bool lossy = false;
};

static StereoEncoded StereoImage(ArtifactCalls& calls, std::uint64_t reader, std::uint64_t frame)
{
	ImageEncodingPermit permit;
	std::vector<std::byte> sheet;
	std::uint32_t width = 0, height = 0;
	bool lossy = false;
	for (unsigned eye = 0; eye < 2; ++eye)
	{
		const auto started =
			calls.Request(wire::OpCode::StartExport,
						  {wire::Field::U64(tag::ImageReaderId, reader, true), wire::Field::U32(tag::Layer, eye)});
		const auto ticket = panel::U64(started, tag::ExportId);
		if (!ticket)
			throw std::runtime_error("Runtime omitted the sequence export identity");
		try
		{
			wire::Message info;
			for (;;)
			{
				info = calls.Request(wire::OpCode::ExportInfo, {wire::Field::U64(tag::ExportId, ticket, true)});
				const auto phase = panel::Text(info, tag::Phase);
				if (phase == "complete")
					break;
				if (phase != "pending")
					throw std::runtime_error("Sequence image export failed: " + phase);
				calls.Check();
				std::this_thread::sleep_for(10ms);
			}
			if (panel::Frame(info) != frame || panel::U32(info, tag::Layer, UINT32_MAX) != eye)
				throw std::runtime_error("Sequence image export changed frame or eye");
			const auto* descriptor = info.Find(tag::ImageDescriptor);
			if (!descriptor)
				throw std::runtime_error("Runtime omitted the sequence image description");
			const auto description = wire::DecodeImageDescription(*descriptor);
			const auto w = description.width, h = description.height, format = description.format;
			lossy |= format != static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm) &&
					 format != static_cast<std::uint32_t>(ColorFormat::Rgba8Srgb) &&
					 format != static_cast<std::uint32_t>(ColorFormat::Bgra8Unorm) &&
					 format != static_cast<std::uint32_t>(ColorFormat::Bgra8Srgb);
			const auto pixelBytes = ColorPixelBytes(format);
			const auto total = panel::U64(info, tag::TotalBytes);
			if (!pixelBytes || !w || !h || w > 2048 || h > 4096 || description.arrayLayers != 1 ||
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
			control::ExportReader::Read(
				[&](auto opcode, auto fields) { return calls.Request(opcode, std::move(fields)); }, ticket, total,
				[&](std::span<const std::byte> bytes) { raw.insert(raw.end(), bytes.begin(), bytes.end()); });
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
			calls.Request(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, ticket, true)});
		}
		catch (...)
		{
			try
			{
				calls.Request(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, ticket, true)});
			}
			catch (...)
			{}
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
	if (connection.empty() || connection.size() > 256 || connection.find('\0') != std::string::npos || text.empty() ||
		text.size() > 4096 || text.find('\0') != std::string::npos)
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
									const std::shared_ptr<McpCancellation>& cancellation,
									const std::function<void(const Json&)>& progress)
{
	std::filesystem::path output;
	Json manifest = {{"ok", false},
					 {"phase", "preparing"},
					 {"captures", Json::array()},
					 {"incomplete", false},
					 {"history_gap", false}};
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
		if (progress)
			progress(manifest);
		ArtifactCalls calls(app, debug, connection, cancellation, timeout, "Sequence capture");
		std::uint64_t sequence = 0;
		std::map<std::uint64_t, std::uint64_t> pending;
		std::set<std::uint64_t> seen;
		try
		{
			auto request = PrepareSequence("run_pose_sequence", start,
										   arguments.contains("session") ? ImageId(arguments, "session") : 0,
										   arguments.contains("generation") ? ImageId(arguments, "generation") : 0);
			if (!arguments.contains("session"))
				std::erase_if(request.fields, [](const auto& field) {
					return field.tag == tag::Session || field.tag == tag::Generation;
				});
			const auto started = calls.Sequence(
				request.opcode, std::move(request.fields),
				arguments.contains("program")
					? std::optional<std::string_view>{arguments.at("program").get_ref<const std::string&>()}
					: std::nullopt);
			sequence = SequenceInteger(started, "sequence_id");
			if (!started.at("document").at("active").get<bool>())
				throw std::runtime_error("Sequence is already complete; use a new program ID");
			manifest["sequence_id"] = sequence;
			manifest["phase"] = "running";
			WriteManifest(output, manifest);
			if (progress)
				progress(manifest);
			std::uint64_t cursor = 0, bytes = 0;
			for (;;)
			{
				calls.Check();
				const auto result =
					calls.Sequence(wire::OpCode::ReadSequence,
								   {wire::Field::U64(tag::SequenceId, sequence, true),
									wire::Field::U64(tag::Offset, cursor), wire::Field::U64(tag::Length, 64)});
				const auto& page = result.at("document");
				if (page.at("status").is_null())
					throw std::runtime_error("Sequence was replaced while saving captures");
				const auto& status = page.at("status");
				if (page.at("history_gap").get<bool>())
				{
					manifest["incomplete"] = true;
					manifest["history_gap"] = true;
				}
				cursor = SequenceInteger(page, "next_frame", true);
				manifest["status"] = status;
				for (const auto& receipt : page.at("captures"))
				{
					const auto frame = SequenceInteger(receipt, "frame");
					const auto capture = SequenceInteger(receipt, "capture_id", true);
					if (!capture)
					{
						manifest["incomplete"] = true;
						throw std::runtime_error("A sequence capture could not be admitted: " +
												 receipt.value("admission", "unknown"));
					}
					if (!seen.contains(capture))
						pending.emplace(capture, frame);
				}
				for (const auto& receipt : page.at("held_captures"))
				{
					const auto capture = SequenceInteger(receipt, "capture_id");
					const auto frame = SequenceInteger(receipt, "frame");
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
							calls.Sequence(wire::OpCode::StopSequence,
										   {wire::Field::U64(tag::SequenceId, sequence, true)});
						manifest["phase"] = "capture_limit";
						manifest["incomplete"] = true;
						break;
					}
					const auto capture = item->first;
					const auto current =
						calls.Request(wire::OpCode::Capture, {wire::Field::U64(tag::CaptureId, capture, true)});
					const auto phase = panel::Text(current, tag::Phase);
					if (phase == "reserved" || phase == "issued" || phase == "begun")
					{
						++item;
						continue;
					}
					if ((phase != "ready" && phase != "gpu_pending") || panel::Frame(current) != item->second)
						throw std::runtime_error("Sequence capture is unavailable or changed frame: " + phase);
					wire::Message retained;
					try
					{
						retained =
							calls.Request(wire::OpCode::RetainImage, {wire::Field::U64(tag::CaptureId, capture, true)});
					}
					catch (const ArtifactError& error)
					{
						if (error.code != static_cast<std::uint32_t>(control::ErrorCode::Busy))
							throw;
						++item;
						continue;
					}
					const auto reader = panel::U64(retained, tag::ImageReaderId);
					if (!reader)
						throw std::runtime_error("Runtime omitted the sequence image reader identity");
					try
					{
						const auto info = calls.Request(wire::OpCode::ImageInfo,
														{wire::Field::U64(tag::ImageReaderId, reader, true)});
						const auto generation = panel::U64(panel::Nested(info.Find(tag::Frame)), tag::Generation);
						if (panel::Frame(info) != item->second || !generation ||
							generation != panel::U64(panel::Nested(current.Find(tag::Frame)), tag::Generation))
							throw std::runtime_error("Sequence reader changed frame identity");
						const auto encoded = StereoImage(calls, reader, item->second);
						if (bytes + encoded.bytes.size() > 128ULL * 1024 * 1024)
							throw std::runtime_error("Sequence output exceeds its 128 MiB disk budget");
						calls.Check();
						const auto path = output / ("frame-" + std::to_string(item->second) + ".png");
						SaveExportFile(path, encoded.bytes, false);
						bytes += encoded.bytes.size();
						manifest["captures"].push_back({{"frame", item->second},
														{"capture_id", capture},
														{"path", PathText(path)},
														{"bytes", std::to_string(encoded.bytes.size())},
														{"frame_key", ImageDocument(current).at("frame")},
														{"width", encoded.width},
														{"height", encoded.height},
														{"lossy", encoded.lossy}});
						WriteManifest(output, manifest);
						if (progress)
							progress(manifest);
						calls.Request(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, reader, true)});
						calls.Sequence(wire::OpCode::ReleaseSequenceCapture,
									   {wire::Field::U64(tag::CaptureId, capture, true)});
					}
					catch (...)
					{
						try
						{
							calls.Request(wire::OpCode::ReleaseImage,
										  {wire::Field::U64(tag::ImageReaderId, reader, true)});
						}
						catch (...)
						{}
						throw;
					}
					seen.insert(item->first);
					item = pending.erase(item);
					if (manifest["captures"].size() >= static_cast<std::size_t>(limit) &&
						status.at("active").get<bool>())
					{
						calls.Sequence(wire::OpCode::StopSequence, {wire::Field::U64(tag::SequenceId, sequence, true)});
						manifest["phase"] = "capture_limit";
						manifest["incomplete"] = true;
						break;
					}
				}
				if (manifest["phase"] == "capture_limit")
				{
					const auto final =
						calls.Sequence(wire::OpCode::ReadSequence, {wire::Field::U64(tag::SequenceId, sequence, true)});
					if (!final.at("document").at("status").is_null())
						manifest["status"] = final.at("document").at("status");
					manifest["unsaved_captures"] = final.at("document").at("held_captures");
					for (const auto& held : manifest["unsaved_captures"])
						calls.Sequence(wire::OpCode::ReleaseSequenceCapture,
									   {wire::Field::U64(tag::CaptureId, SequenceInteger(held, "capture_id"), true)});
					break;
				}
				if (!status.at("active").get<bool>() && !page.at("more").get<bool>() && pending.empty() &&
					cursor >= SequenceInteger(status, "last_frame", true))
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
			if (progress)
				progress(manifest);
			return Result(manifest, !manifest["ok"].get<bool>());
		}
		catch (...)
		{
			if (sequence)
			{
				try
				{
					calls.Sequence(wire::OpCode::StopSequence, {wire::Field::U64(tag::SequenceId, sequence, true)});
				}
				catch (...)
				{}
				try
				{
					const auto final =
						calls.Sequence(wire::OpCode::ReadSequence, {wire::Field::U64(tag::SequenceId, sequence, true)});
					manifest["unsaved_captures"] = final.at("document").at("held_captures");
					for (const auto& held : manifest["unsaved_captures"])
						calls.Sequence(wire::OpCode::ReleaseSequenceCapture,
									   {wire::Field::U64(tag::CaptureId, SequenceInteger(held, "capture_id"), true)});
				}
				catch (...)
				{
					debug.Disconnect(connection);
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
			try
			{
				WriteManifest(output, manifest);
				if (progress)
					progress(manifest);
			}
			catch (const std::exception& failure)
			{
				manifest["manifest_error"] = failure.what();
			}
		}
		if (cancellation->Requested())
			return {};
		return Result(manifest, true);
	}
}
}
