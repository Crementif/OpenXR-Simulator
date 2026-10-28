#include "Pch.h"
#include "app/mcp/McpImageSave.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpArtifactCalls.h"
#include "app/mcp/McpImageEncode.h"
#include "app/platform/FileStorage.h"
#include "app/services/Utf8Path.h"
#include "control/ProtocolTypes.h"
#include "graphics/ColorPixels.h"

#include <charconv>
#include <cmath>
#include <fstream>

namespace simxr::app::mcp
{
using Json = nlohmann::json;

static Json Error(const std::string& message)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", Json({{"ok", false}, {"error", message}}).dump()}}})},
		{"isError", true}};
}
Json ImageSaveDefinition()
{
	const Json id = {{"oneOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	return {{"name", "debug_save_image"},
		{"description", "Encode a completed single-eye image export and atomically save it to an absolute .png, .jpg or .jpeg path. Existing files require overwrite=true; the export remains owned by the connection."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"connection", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"session", id}, {"generation", id}, {"export", id},
			{"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
			{"quality", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 120}}},
			{"overwrite", {{"type", "boolean"}}}}},
			{"required", Json::array({"connection", "export", "path"})}, {"additionalProperties", false}}}};
}
static std::uint64_t Identifier(const Json& value, const char* label)
{
	if (value.is_number_integer() && value > 0 && value <= ((1ULL << 53) - 1))
		return value.get<std::uint64_t>();
	if (!value.is_string())
		throw std::invalid_argument(std::string(label) + " must be a decimal string or safe integer");
	const auto& text = value.get_ref<const std::string&>();
	if (text.empty() || text.size() > 20 || text.front() == '0')
		throw std::invalid_argument(std::string(label) + " must be a canonical nonzero integer");
	std::uint64_t number = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !number)
		throw std::invalid_argument(std::string(label) + " must be a canonical nonzero integer");
	return number;
}
std::optional<Json> SaveImage(McpApplication& app, McpDebug& debug, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	std::filesystem::path temporary;
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		for (auto item = arguments.begin(); item != arguments.end(); ++item)
			if (item.key() != "connection" && item.key() != "session" && item.key() != "generation" &&
				item.key() != "export" && item.key() != "path" && item.key() != "quality" &&
				item.key() != "timeout" && item.key() != "overwrite")
				throw std::invalid_argument("Unknown argument: " + item.key());
		if (arguments.contains("session") != arguments.contains("generation"))
			throw std::invalid_argument("Supply both session and generation, or omit both");
		const auto connection = arguments.at("connection").get<std::string>();
		const auto path = arguments.at("path").get<std::string>();
		if (connection.empty() || connection.size() > 256 || connection.find('\0') != std::string::npos ||
			path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos)
			throw std::invalid_argument("Invalid connection or path");
		const auto ticket = Identifier(arguments.at("export"), "export");
		const auto destination = Utf8Path(path);
		auto extension = PathText(destination.extension());
		for (auto& ch : extension)
			if (ch >= 'A' && ch <= 'Z')
				ch = static_cast<char>(ch + ('a' - 'A'));
		const bool png = extension == ".png";
		if (!destination.is_absolute() || (!png && extension != ".jpg" && extension != ".jpeg"))
			throw std::invalid_argument("Use an absolute .png, .jpg or .jpeg destination");
		if (png && arguments.contains("quality"))
			throw std::invalid_argument("quality applies only to JPEG");
		int quality = 85;
		if (arguments.contains("quality"))
		{
			if (!arguments["quality"].is_number_integer() || arguments["quality"] < 1 || arguments["quality"] > 100)
				throw std::invalid_argument("quality must be an integer from 1 to 100");
			quality = arguments["quality"].get<int>();
		}
		const auto overwrite = arguments.value("overwrite", false);
		const double timeout = arguments.value("timeout", 30.0);
		if (!std::isfinite(timeout) || timeout <= 0 || timeout > 120)
			throw std::invalid_argument("timeout must be greater than zero and at most 120 seconds");
		if (!cancellation->TryAdmit())
			return {};
		ImageEncodingPermit permit;
		ArtifactCalls calls(app, debug, connection, cancellation, timeout, "Image save");
		Json request = {{"connection", connection}, {"export", std::to_string(ticket)}};
		if (arguments.contains("session"))
		{
			request["session"] = arguments.at("session");
			request["generation"] = arguments.at("generation");
		}
		auto info = calls.Call("debug_export_info", std::move(request));
		if (info.value("phase", "") != "complete" || !info.contains("image_description"))
			throw std::runtime_error("Export must be a completed raw image; poll debug_export_info first");
		const auto& image = info.at("image_description");
		const auto width = image.at("width").get<std::uint32_t>();
		const auto height = image.at("height").get<std::uint32_t>();
		const auto format = image.at("format").get<std::uint32_t>();
		const auto pixelBytes = ColorPixelBytes(format);
		const auto total = Identifier(info.at("total_bytes"), "total_bytes");
		if (!pixelBytes || !width || !height || width > 4096 || height > 4096 ||
			image.at("array_layers") != 1 || std::uint64_t{width} * height > MaxImageInput / pixelBytes ||
			total != std::uint64_t{width} * height * pixelBytes)
			throw std::runtime_error("Choose a supported single-layer image within the 32 MiB encoding limit");
		std::vector<std::byte> pixels;
		pixels.reserve(static_cast<std::size_t>(total));
		while (pixels.size() < total)
		{
			calls.Check();
			const auto offset = pixels.size();
			const auto length = std::min<std::uint64_t>(control::MaxPayload - 4096, total - offset);
			const auto bytes = calls.Read(ticket, offset, length);
			pixels.insert(pixels.end(), bytes.begin(), bytes.end());
		}
		calls.Check();
		const auto encoded = EncodeImage(pixels, format, width, height, png, quality);
		calls.Check();
		temporary = CreatePrivateTemporaryFile(destination);
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("Cannot open temporary image file");
		output.write(reinterpret_cast<const char*>(encoded.bytes.data()), static_cast<std::streamsize>(encoded.bytes.size()));
		if (!output)
			throw std::runtime_error("Cannot write temporary image file");
		output.close();
		if (!output)
			throw std::runtime_error("Cannot close temporary image file");
		calls.Check();
		if (overwrite)
			CommitTemporaryFile(temporary, destination);
		else
			RenameFileWithoutReplacing(temporary, destination);
		temporary.clear();
		info["path"] = PathText(destination);
		info["media_type"] = encoded.mediaType;
		info["lossy"] = encoded.lossy;
		info["encoded_bytes"] = std::to_string(encoded.bytes.size());
		return Json{{"content", Json::array({{{"type", "text"}, {"text", info.dump()}}})}, {"isError", false}};
	}
	catch (const std::exception& error)
	{
		if (!temporary.empty())
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
		}
		if (cancellation->Requested())
			return {};
		return Error(error.what());
	}
}
}
