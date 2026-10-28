#include "Pch.h"
#include "app/mcp/McpSequenceJobs.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpSequenceCapture.h"

namespace simxr::app
{
using Json = nlohmann::json;

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

Json McpSequenceJobs::Definitions()
{
	auto start = mcp::SequenceCaptureDefinition();
	start["name"] = "start_pose_sequence_capture";
	start["description"] = "Start saving a pose sequence in a session-owned background job. Poll get_pose_sequence_capture. "
		"Retry only with the same job_id and identical arguments; cancellation or disconnect can close its debugger connection.";
	start["inputSchema"]["properties"]["job_id"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
	start["inputSchema"]["required"].push_back("job_id");
	const Json id = {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
	const auto tool = [&](const char* name, const char* description)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", {{"job_id", id}}}, {"required", Json::array({"job_id"})}, {"additionalProperties", false}}}};
	};
	return Json::array({std::move(start),
		tool("get_pose_sequence_capture", "Read the current or completed background capture and its latest written manifest."),
		tool("cancel_pose_sequence_capture", "Cancel and join the background capture. Cancellation retires its debugger connection; inspect the manifest for partial output.")});
}

bool McpSequenceJobs::Handles(const std::string& name)
{
	return name == "start_pose_sequence_capture" || name == "get_pose_sequence_capture" || name == "cancel_pose_sequence_capture";
}

std::optional<Json> McpSequenceJobs::Call(McpApplication& app, McpDebug& debug, const std::string& name,
	const Json& arguments, const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object() || !arguments.contains("job_id") || !arguments.at("job_id").is_string())
			throw std::invalid_argument("job_id must be a nonempty string");
		const auto id = arguments.at("job_id").get<std::string>();
		if (id.empty() || id.size() > 128 || id.find('\0') != std::string::npos)
			throw std::invalid_argument("job_id must be a nonempty string of at most 128 bytes");
		if (name == "get_pose_sequence_capture")
		{
			if (arguments.size() != 1)
				throw std::invalid_argument("Supply job_id only");
			std::lock_guard lock(m_mutex);
			if (m_status.is_null() || m_status.at("job_id") != id)
				throw std::invalid_argument("Unknown job_id for this MCP session");
			return Result(m_status);
		}
		if (name == "cancel_pose_sequence_capture")
		{
			if (arguments.size() != 1)
				throw std::invalid_argument("Supply job_id only");
			if (!cancellation->TryAdmit())
				return {};
			std::lock_guard lifecycle(m_lifecycle);
			{
				std::lock_guard lock(m_mutex);
				if (m_status.is_null() || m_status.at("job_id") != id)
					throw std::invalid_argument("Unknown job_id for this MCP session");
				if (m_cancellation && (m_status.at("phase") == "starting" || m_status.at("phase") == "running"))
					m_cancellation->Cancel();
			}
			if (m_worker.joinable())
				m_worker.join();
			std::lock_guard lock(m_mutex);
			return Result(m_status);
		}
		if (name != "start_pose_sequence_capture")
			throw std::invalid_argument("Unknown sequence capture tool");
		Json options = arguments;
		options.erase("job_id");
		std::lock_guard lifecycle(m_lifecycle);
		{
			std::lock_guard lock(m_mutex);
			if (m_closed)
				throw std::runtime_error("MCP session is closed");
			if (!m_status.is_null() && m_status.at("job_id") == id)
			{
				if (m_options != options)
					throw std::invalid_argument("job_id was already used with different options");
				return Result(m_status);
			}
			if (m_retired.contains(id))
				throw std::invalid_argument("job_id was already retired; choose a new identifier");
			if (!m_status.is_null() && (m_status.at("phase") == "starting" || m_status.at("phase") == "running"))
				throw std::runtime_error("A sequence capture job is already running in this MCP session");
			if (!m_status.is_null() && m_retired.size() >= 64)
				throw std::runtime_error("Sequence job identifier history is full; reconnect the MCP session");
		}
		mcp::ValidateSequenceCapture(options);
		if (!cancellation->TryAdmit())
			return {};
		if (m_worker.joinable())
			m_worker.join();
		auto workCancellation = std::make_shared<McpCancellation>();
		{
			std::lock_guard lock(m_mutex);
			if (!m_status.is_null())
				m_retired.insert(m_status.at("job_id").get<std::string>());
			m_options = options;
			m_cancellation = workCancellation;
			m_status = {{"ok", true}, {"job_id", id}, {"phase", "starting"}, {"capture", nullptr}};
		}
		try
		{
			m_worker = std::thread(&McpSequenceJobs::Run, this, std::ref(app), std::ref(debug), options, workCancellation);
		}
		catch (...)
		{
			std::lock_guard lock(m_mutex);
			m_status["phase"] = "failed";
			m_status["ok"] = false;
			m_status["error"] = "Could not start sequence capture worker";
			throw;
		}
		std::lock_guard lock(m_mutex);
		return Result(m_status);
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Result({{"ok", false}, {"error", error.what()}}, true);
	}
}

void McpSequenceJobs::Run(McpApplication& app, McpDebug& debug, Json options,
	std::shared_ptr<McpCancellation> cancellation) noexcept
{
	try
	{
		const auto result = mcp::CaptureSequence(app, debug, options, cancellation, [&](const Json& manifest)
		{
			std::lock_guard lock(m_mutex);
			m_status["capture"] = manifest;
			const auto phase = manifest.at("phase").get<std::string>();
			m_status["phase"] = phase == "preparing" ? "starting" : "running";
		});
		std::lock_guard lock(m_mutex);
		if (!result)
		{
			m_status["phase"] = "cancelled";
			m_status["ok"] = false;
		}
		else
		{
			const auto data = Json::parse(result->at("content").at(0).at("text").get<std::string>());
			m_status["capture"] = data;
			m_status["phase"] = data.value("phase", "failed");
			m_status["ok"] = !result->value("isError", true);
		}
	}
	catch (const std::exception& error)
	{
		std::lock_guard lock(m_mutex);
		m_status["ok"] = false;
		m_status["phase"] = cancellation->Requested() ? "cancelled" : "failed";
		m_status["error"] = error.what();
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		m_status["ok"] = false;
		m_status["phase"] = "failed";
		m_status["error"] = "Unexpected sequence capture worker failure";
	}
}

void McpSequenceJobs::Close() noexcept
{
	std::lock_guard lifecycle(m_lifecycle);
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		if (m_cancellation && !m_status.is_null() &&
			(m_status.at("phase") == "starting" || m_status.at("phase") == "running"))
			m_cancellation->Cancel();
	}
	if (m_worker.joinable())
		m_worker.join();
}
McpSequenceJobs::~McpSequenceJobs()
{
	Close();
}
}
