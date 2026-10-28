#include "Pch.h"
#include "app/mcp/McpLogs.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpIncident.h"
#include "app/mcp/McpLogFile.h"
#include "app/mcp/McpTransientDebug.h"
#include "app/services/Utf8Path.h"

#include <cmath>
#include <deque>

namespace simxr::app::mcp
{
using Json = nlohmann::json;

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

Json LogDefinition()
{
	return {{"name", "read_logs"}, {"description", "Read a bounded tail of the selected runtime process's legacy text log. "
		"A launch-derived directory is inferred when possible; log contents are not independently verified against the runtime epoch."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"directory", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
			{"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
			{"filter", {{"type", "string"}, {"maxLength", 256}, {"description", "Optional ASCII case-insensitive literal substring, not a regular expression."}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}}},
			{"required", Json::array({"application_id"})}, {"additionalProperties", false}}}};
}

Json ClearLogDefinition()
{
	return {{"name", "clear_logs"}, {"description", "Truncate only the selected runtime process's legacy log. "
		"Concurrent writes can add new lines immediately. The PID-named file cannot be authenticated to a runtime epoch; "
		"a cancelled or failed request may have already cleared it. Link checks are snapshots; concurrent links or renames can change the affected file."},
		{"inputSchema", {{"type", "object"}, {"properties", {
			{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
			{"directory", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
			{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}}},
			{"required", Json::array({"application_id"})}, {"additionalProperties", false}}}};
}

static std::string LowerAscii(std::string_view text)
{
	std::string value(text);
	for (auto& c : value)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c + ('a' - 'A'));
	return value;
}

std::optional<Json> LogCall(McpApplication& application, const Json& arguments, bool clear,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		for (auto item = arguments.begin(); item != arguments.end(); ++item)
			if (item.key() != "application_id" && item.key() != "directory" && item.key() != "timeout" &&
				(clear || (item.key() != "lines" && item.key() != "filter")))
				throw std::invalid_argument("Unknown argument: " + item.key());
		const auto& selected = arguments.at("application_id");
		if (!selected.is_string() || selected.get_ref<const std::string&>().empty() || selected.get_ref<const std::string&>().size() > 256)
			throw std::invalid_argument("application_id must identify a selected application");
		const auto count = arguments.value("lines", Json(100));
		if (!count.is_number_integer() || count < 1 || count > 1000)
			throw std::invalid_argument("lines must be an integer from 1 to 1000");
		std::string filter;
		if (arguments.contains("filter"))
		{
			if (!arguments.at("filter").is_string() || arguments.at("filter").get_ref<const std::string&>().size() > 256)
				throw std::invalid_argument("filter must be a string of at most 256 bytes");
			filter = LowerAscii(arguments.at("filter").get_ref<const std::string&>());
		}
		std::filesystem::path directory;
		if (arguments.contains("directory"))
		{
			if (!arguments.at("directory").is_string())
				throw std::invalid_argument("directory must be an absolute path");
			const auto& value = arguments.at("directory").get_ref<const std::string&>();
			if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos)
				throw std::invalid_argument("Invalid log directory");
			directory = Utf8Path(value);
			if (!directory.is_absolute())
				throw std::invalid_argument("directory must be an absolute path");
		}
		const double timeout = arguments.value("timeout", 5.0);
		if (!std::isfinite(timeout) || timeout <= 0 || timeout > 30)
			throw std::invalid_argument("timeout must be greater than zero and at most 30 seconds");
		if (!cancellation->TryAdmit())
			return {};
		TransientDebug calls(application, cancellation, timeout, clear ? "Log clear" : "Log read");
		const auto before = calls.Application("simulator_application_status", {{"application_id", selected}});
		if (!before.value("connected", false))
			throw std::runtime_error("The selected runtime is not connected");
		const auto process = before.at("runtime_process_id").get<std::string>();
		(void)IncidentIdentifier(process, "runtime process identifier");
		if (directory.empty())
		{
			if (!before.contains("inferred_runtime_data_directory") || !before.at("inferred_runtime_data_directory").is_string())
				throw std::runtime_error("Runtime data directory is unknown; supply directory explicitly");
			directory = Utf8Path(before.at("inferred_runtime_data_directory").get<std::string>());
			if (!directory.is_absolute())
				throw std::runtime_error("Inferred runtime data directory is not absolute");
		}
		const auto path = directory / ("openxr_simulator." + process + ".log");
		calls.Check();
		if (clear)
		{
			const auto current = calls.Application("simulator_application_status", {{"application_id", selected}});
			if (!current.value("connected", false) || current.at("runtime_process_id") != before.at("runtime_process_id") ||
				current.at("runtime_epoch") != before.at("runtime_epoch"))
				throw std::runtime_error("The selected runtime changed before clearing logs; retry explicitly");
			calls.Check();
			const auto cleared = ClearLogFile(path);
			try
			{
				calls.Check();
				const auto after = calls.Application("simulator_application_status", {{"application_id", selected}});
				if (!after.value("connected", false) || after.at("runtime_process_id") != before.at("runtime_process_id") ||
					after.at("runtime_epoch") != before.at("runtime_epoch"))
					throw std::runtime_error("The selected runtime changed");
			}
			catch (const std::exception& error)
			{
				throw std::runtime_error(std::string("Log clear may have completed; ") + error.what());
			}
			return Result({{"ok", true}, {"application_id", selected}, {"runtime_process_id", process},
				{"runtime_epoch", before.at("runtime_epoch")}, {"path", PathText(path)},
				{"directory_source", arguments.contains("directory") ? "explicit" : "inferred_from_launch"},
				{"identity_verified", false}, {"cleared", cleared},
				{"note", "The log is identified by process ID, not runtime epoch. Concurrent writes can add lines immediately; concurrent links or renames can change the affected file."}});
		}
		const auto snapshot = ReadLogFile(path);
		calls.Check();
		const auto after = calls.Application("simulator_application_status", {{"application_id", selected}});
		if (!after.value("connected", false) || after.at("runtime_process_id") != before.at("runtime_process_id") ||
			after.at("runtime_epoch") != before.at("runtime_epoch"))
			throw std::runtime_error("The selected runtime changed while reading logs; retry explicitly");
		Json result = {{"ok", true}, {"application_id", selected}, {"runtime_process_id", process},
			{"runtime_epoch", before.at("runtime_epoch")}, {"path", PathText(path)},
			{"directory_source", arguments.contains("directory") ? "explicit" : "inferred_from_launch"},
			{"identity_verified", false}, {"identity_note", "The log is named by process ID and may contain older data from a reused process ID; its runtime epoch is not recorded."},
			{"available", snapshot.has_value()}, {"lines", Json::array()}};
		if (snapshot)
		{
			std::string bytes;
			if (!snapshot->bytes.empty())
				bytes.assign(reinterpret_cast<const char*>(snapshot->bytes.data()), snapshot->bytes.size());
			const auto text = Json::parse(Json(bytes).dump(-1, ' ', false, Json::error_handler_t::replace)).get<std::string>();
			std::deque<std::string> lines;
			const auto maximum = count.get<std::size_t>();
			bool omittedLines = false;
			std::size_t start = 0;
			if (snapshot->truncated)
			{
				const auto first = text.find('\n');
				start = first == std::string::npos ? text.size() : first + 1;
			}
			while (start < text.size())
			{
				const auto end = text.find('\n', start);
				auto line = text.substr(start, end == std::string::npos ? end : end - start);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (lines.size() == maximum)
				{
					lines.pop_front();
					omittedLines = true;
				}
				lines.push_back(std::move(line));
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			for (const auto& line : lines)
				if (filter.empty() || LowerAscii(line).find(filter) != std::string::npos)
					result["lines"].push_back(line);
			result["file_size_bytes"] = std::to_string(snapshot->size);
			result["tail_truncated"] = snapshot->truncated;
			result["lines_truncated"] = omittedLines;
			result["changed_during_read"] = snapshot->changed;
		}
		return Result(result);
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Result({{"ok", false}, {"error", error.what()}}, true);
	}
}
}
