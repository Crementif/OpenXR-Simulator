#include "app/mcp/McpPch.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpControls.h"
#include "app/mcp/McpDiagnostics.h"
#include "app/mcp/McpGame.h"
#include "app/mcp/McpHistory.h"
#include "app/mcp/McpIncident.h"
#include "app/mcp/McpLogs.h"
#include "app/mcp/McpImage.h"
#include "app/mcp/McpImageCache.h"
#include "app/services/ImageExport.h"
#include "app/mcp/McpImageSave.h"
#include "app/mcp/McpScreenshot.h"
#include "app/mcp/McpSequence.h"
#include "app/mcp/McpSequenceCapture.h"
#include "app/mcp/McpSimulation.h"
#include "app/mcp/McpVideoSave.h"
#include "control/ControlPacket.h"
#include "control/ExportReader.h"
#include "control/ControlSchema.h"
#include "control/ImageInspection.h"
#include "control/LocalControlClient.h"
#include "control/SimulationControl.h"
#include "graphics/ColorPixels.h"

#include <tuple>

namespace simxr::app
{
using Json = nlohmann::json;
namespace wire = control;
namespace tag = control::tag;

static mcp::ImageCache s_imageCache(32ULL * 1024 * 1024, 16);

struct McpDebug::Entry
{
	std::mutex owner;
	std::timed_mutex exchange;
	std::shared_ptr<wire::LocalControlClient> client;
	std::atomic<bool> closed{false};
	std::string application, endpoint;
	std::uint64_t process = 0, session = 0, generation = 0, epoch = 0, capabilities = 0;
	std::optional<std::uint64_t> controlEditPending;
	std::deque<std::pair<std::uint64_t, Json>> controlEditRequests;
	std::uint64_t controlEditLast = 0;
	std::uint16_t minor = 0;
	bool gameOnly = false, launched = false;

	std::shared_ptr<wire::LocalControlClient> Client()
	{
		std::lock_guard lock(owner);
		if (closed.load() || !client)
			throw std::runtime_error("Control connection is closed");
		return client;
	}
	void Close() noexcept
	{
		if (closed.exchange(true))
			return;
		std::shared_ptr<wire::LocalControlClient> current;
		{
			std::lock_guard lock(owner);
			current = client;
		}
		if (current)
			current->Close();
	}
};

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}
static Json Failure(const std::string& message)
{
	return Result({{"ok", false}, {"error", message}}, true);
}
static std::string Text(const Json& object, const char* key)
{
	const auto found = object.find(key);
	if (found == object.end() || !found->is_string())
		throw std::invalid_argument(std::string(key) + " must be a string");
	const auto value = found->get<std::string>();
	if (value.empty() || value.size() > 256 || value.find('\0') != std::string::npos)
		throw std::invalid_argument(std::string("Invalid ") + key);
	return value;
}
static std::uint64_t Number(const Json& object, const char* key, bool allowZero = false)
{
	const auto found = object.find(key);
	if (found == object.end())
		throw std::invalid_argument(std::string("Missing ") + key);
	if (found->is_number_integer() && *found >= (allowZero ? 0 : 1) && *found <= ((1ULL << 53) - 1))
		return found->get<std::uint64_t>();
	const auto value = Text(object, key);
	std::uint64_t result = 0;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
	if ((value.size() > 1 && value.front() == '0') || parsed.ec != std::errc{} ||
		parsed.ptr != value.data() + value.size() || (!allowZero && !result))
		throw std::invalid_argument(std::string(key) + " must be a canonical decimal string" +
			(allowZero ? "" : " greater than zero"));
	return result;
}
static void Keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
	if (!object.is_object())
		throw std::invalid_argument("Arguments must be an object");
	for (auto item = object.begin(); item != object.end(); ++item)
		if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
			throw std::invalid_argument("Unknown argument: " + item.key());
}
static std::uint64_t U64(const wire::Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	const auto value = field ? field->AsU64() : std::nullopt;
	if (!value)
		throw std::runtime_error("Runtime omitted a required identifier");
	return *value;
}
static wire::Message Nested(const wire::Field* field)
{
	if (!field)
		throw std::runtime_error("Runtime omitted required debugger details");
	auto decoded = wire::DecodeFields(field->value);
	if (decoded.error != wire::DecodeError::None)
		throw std::runtime_error("Runtime returned malformed debugger details");
	wire::Message message;
	message.fields = std::move(decoded.fields);
	return message;
}
static void AddId(Json& result, const wire::Message& message, std::uint16_t name, const char* key)
{
	if (const auto* field = message.Find(name))
		if (const auto value = field->AsU64())
			result[key] = std::to_string(*value);
}
static void AddText(Json& result, const wire::Message& message, std::uint16_t name, const char* key)
{
	if (const auto* field = message.Find(name))
		if (const auto value = field->AsText())
			result[key] = std::string(*value);
}
static void AddBool(Json& result, const wire::Message& message, std::uint16_t name, const char* key)
{
	if (const auto* field = message.Find(name))
		if (const auto value = field->AsBool())
			result[key] = *value;
}
static void AddU32(Json& result, const wire::Message& message, std::uint16_t name, const char* key)
{
	if (const auto* field = message.Find(name))
		if (const auto value = field->AsU32())
			result[key] = *value;
}
static Json IdList(const wire::Field* field)
{
	const auto values = Nested(field);
	Json result = Json::array();
	for (const auto& entry : values.fields)
	{
		const auto id = entry.AsU64();
		if (!id)
			throw std::runtime_error("Runtime returned malformed identifier list");
		result.push_back(std::to_string(*id));
	}
	return result;
}
Json McpDebug::Document(const wire::Message& message, bool metadataOnly)
{
	const auto* error = message.Find(tag::Error);
	const auto code = error ? error->AsU32() : std::nullopt;
	if (!code)
		throw std::runtime_error("Runtime response omitted its error code");
	if (*code)
	{
		Json failure = {{"ok", false}, {"error", "Runtime rejected the request"}, {"code", *code}};
		AddText(failure, message, tag::Detail, "detail");
		return failure;
	}
	Json value = {{"ok", true}};
	if (metadataOnly)
		value["metadata_only"] = true;
	for (const auto& [field, name] : {std::pair{tag::Lease, "lease"}, {tag::OperationId, "operation_id"},
									 {tag::CommandId, "command_id"}, {tag::CaptureId, "capture_id"},
									 {tag::Capabilities, "capabilities"}})
		AddId(value, message, field, name);
	for (const auto& [field, name] : {std::pair{tag::Phase, "phase"}, {tag::Progress, "progress"},
									 {tag::RetentionResult, "retention_result"}})
		AddText(value, message, field, name);
	AddU32(value, message, tag::SnapshotBudgetMiB, "snapshot_budget_mib");
	AddId(value, message, tag::SnapshotImageBytes, "snapshot_image_bytes");
	if (const auto* field = message.Find(tag::ImageRecording))
	{
		const auto recording = Nested(field);
		Json data = Json::object();
		AddText(data, recording, tag::Phase, "phase");
		AddId(data, recording, tag::Capacity, "capacity");
		AddId(data, recording, tag::Length, "captured");
		AddId(data, recording, tag::TotalFrames, "requested");
		if (const auto* selected = recording.Find(tag::Target))
		{
			const auto target = Nested(selected);
			Json choice = Json::object();
			AddU32(choice, target, tag::CaptureKind, "kind");
			AddU32(choice, target, tag::Layer, "layer");
			AddU32(choice, target, tag::View, "view");
			data["target"] = std::move(choice);
		}
		if (const auto* captures = recording.Find(tag::Captures))
			data["captures"] = IdList(captures);
		if (const auto* missed = recording.Find(tag::Frame))
		{
			const auto frame = Nested(missed);
			Json key = Json::object();
			AddId(key, frame, tag::Generation, "generation");
			AddId(key, frame, tag::FrameId, "frame_id");
			data["missed_frame"] = std::move(key);
		}
		value["image_recording"] = std::move(data);
	}
	for (const auto& [field, name] : {std::pair{tag::Duplicate, "duplicate"}, {tag::Running, "running"},
									 {tag::Closed, "closed"}, {tag::Manual, "manual"},
									 {tag::ImageAvailable, "image_available"}, {tag::Pinned, "pinned"}})
		AddBool(value, message, field, name);
	if (const auto* field = message.Find(tag::Result))
		if (const auto number = field->AsI64())
			value["result"] = *number;
	if (const auto* field = message.Find(tag::State))
		if (const auto number = field->AsI64())
			value["state"] = *number;
	if (const auto* field = message.Find(tag::LeaseRemaining))
		if (const auto number = field->AsI64())
			value["lease_remaining_ns"] = std::to_string(*number);
	if (const auto* field = message.Find(tag::Frame))
	{
		const auto frame = Nested(field);
		Json data = Json::object();
		AddId(data, frame, tag::Generation, "generation");
		AddId(data, frame, tag::FrameId, "frame_id");
		value["frame"] = std::move(data);
	}
	if (const auto* field = message.Find(tag::Target))
	{
		const auto target = Nested(field);
		Json data = Json::object();
		AddU32(data, target, tag::CaptureKind, "kind");
		AddU32(data, target, tag::Layer, "layer");
		AddU32(data, target, tag::View, "view");
		value["target"] = std::move(data);
	}
	for (const auto& [field, name] : {std::pair{tag::Captures, "captures"}, {tag::Diagnostics, "diagnostics"}})
		if (const auto* list = message.Find(field))
			value[name] = IdList(list);
	if (const auto* field = message.Find(tag::Pipeline))
	{
		const auto pipeline = Nested(field);
		Json data = Json::object();
		AddBool(data, pipeline, tag::Running, "running");
		AddBool(data, pipeline, tag::WaitPending, "wait_pending");
		AddBool(data, pipeline, tag::Paused, "paused");
		AddU32(data, pipeline, tag::Permits, "permits");
		AddId(data, pipeline, tag::NextAdmission, "next_admission");
		value["pipeline"] = std::move(data);
	}
	if (const auto* field = message.Find(tag::Retention))
	{
		const auto retention = Nested(field);
		Json data = Json::object();
		AddId(data, retention, tag::Capacity, "capacity");
		AddId(data, retention, tag::HeldSlots, "held_slots");
		AddId(data, retention, tag::Retiring, "retiring");
		AddId(data, retention, tag::Pinned, "pinned");
		if (const auto* captures = retention.Find(tag::Captures))
			data["captures"] = IdList(captures);
		value["retention"] = std::move(data);
	}
	return value;
}

Json McpDebug::Definitions()
{
	const Json text = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
	const Json number = {{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}};
	const Json connection = {{"connection", text}, {"session", number}, {"generation", number}};
	auto lease = connection;
	lease["lease"] = number;
	auto command = lease;
	command["command_id"] = number;
	const auto tool = [](const char* name, const char* description, Json properties, Json required)
	{
		return Json{{"name", name}, {"description", description},
					{"inputSchema", {{"type", "object"}, {"properties", std::move(properties)},
									 {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	Json tools = Json::array();
	tools.push_back(tool("debug_connect", "Open a client-owned debugger connection to a running application. "
						"The connection is tied to its current process and XR session generation.",
						{{"application_id", text}}, Json::array({"application_id"})));
	tools.push_back(tool("game_bridge_connect", "Connect to a simulator-launched game bridge without requiring an XR session. "
						"The connection belongs to this MCP session and is tied to the selected process.",
						{{"application_id", text}}, Json::array({"application_id"})));
	tools.push_back(tool("debug_disconnect", "Close a debugger connection and release its runtime ownership.",
						connection, Json::array({"connection"})));
	tools.push_back(tool("debug_status", "Read debugger and frame-pacing status without advancing a frame.",
						connection, Json::array({"connection"})));
	tools.push_back(tool("debug_acquire", "Acquire the session's debugger lease. Another owner may reject this request.",
						connection, Json::array({"connection"})));
	tools.push_back(tool("debug_renew", "Renew this connection's debugger lease.", lease,
						Json::array({"connection", "lease"})));
	tools.push_back(tool("debug_relinquish", "Release this connection's debugger lease.", lease,
						Json::array({"connection", "lease"})));
	for (const auto& [name, description] : {
		std::pair{"debug_pause", "Request a pause at the next safe boundary."},
		{"debug_resume", "Resume normal frame pacing."},
		{"debug_stop_image_recording", "Stop reserving stream frames; retained captures remain available."},
		{"debug_display_step", "Advance one virtual display tick while paused."}})
		tools.push_back(tool(name, description, command, Json::array({"connection", "lease", "command_id"})));
	auto step = command;
	step["lookahead"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}};
	step["target"] = {{"type", "string"}, {"enum", Json::array({"composed_eyes", "projection_layers", "quad_layers", "layer_view", "layer_depth", "layer_depth_linear"})}};
	step["layer"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}};
	step["view"] = step["layer"];
	step["advance_display"] = {{"type", "boolean"}};
	tools.push_back(tool("debug_step", "Admit one frame and retain its selected capture.", step,
						Json::array({"connection", "lease", "command_id"})));
	tools.push_back(tool("debug_strict_admission", "Reserve an exact capture and admit one frame.", step,
						Json::array({"connection", "lease", "command_id"})));
	auto recording = command;
	recording["target"] = step["target"];
	recording["layer"] = step["layer"];
	recording["view"] = step["view"];
	recording["capacity"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}};
	recording["frames"] = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
		{{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	tools.push_back(tool("debug_start_image_recording", "Start a bounded real-time image stream without changing pacing; poll status for captures and overrun.", recording,
		Json::array({"connection", "lease", "command_id"})));
	auto budget = command;
	budget["budget_mib"] = {{"type", "integer"}, {"minimum", 128}, {"maximum", 8192}};
	tools.push_back(tool("debug_configure_snapshots", "Set the runtime process-wide snapshot image budget in MiB.", budget,
		Json::array({"connection", "lease", "command_id", "budget_mib"})));
	for (const auto& [name, field, description] : {
		std::tuple{"debug_cancel", "operation", "Cancel unused permits for an operation."},
		{"debug_release", "capture", "Release a retained capture."},
		{"debug_pin_snapshot", "capture", "Pin an exact capture against automatic eviction."},
		{"debug_unpin_snapshot", "capture", "Allow an exact capture to be evicted again."}})
	{
		auto properties = command;
		properties[field] = number;
		tools.push_back(tool(name, description, std::move(properties), Json::array({"connection", "lease", "command_id", field})));
	}
	for (const auto& [name, field, description] : {
		std::tuple{"debug_operation", "operation", "Inspect an admitted debugger operation."},
		{"debug_capture", "capture", "Inspect metadata for an exact retained capture."},
		{"debug_inspect", "capture", "Retain historical CPU state and its immutable feature revisions for an exact capture."},
		{"debug_revision", "revision", "Read a connection-owned immutable feature revision while its inspection is retained."},
		{"debug_release_inspection", "inspection", "Release a retained CPU inspection and unused feature revisions."}})
	{
		auto properties = connection;
		properties[field] = number;
		tools.push_back(tool(name, description, std::move(properties), Json::array({"connection", field})));
	}
	auto acknowledgement = lease;
	acknowledgement["through"] = number;
	tools.push_back(tool("debug_acknowledge", "Retire completed debugger command IDs through this value.",
						acknowledgement, Json::array({"connection", "lease", "through"})));
	for (auto& definition : mcp::SimulationDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : mcp::GameDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : mcp::HistoryDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : mcp::ImageDefinitions())
		tools.push_back(std::move(definition));
	tools.push_back(mcp::ScreenshotDefinition());
	tools.push_back(mcp::StereoValidationDefinition());
	tools.push_back(mcp::StereoConsistencyDefinition());
	tools.push_back(mcp::VideoSaveDefinition());
	tools.push_back(mcp::ImageSaveDefinition());
	for (auto& definition : mcp::DiagnosticsDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : mcp::IncidentDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : mcp::ControlsDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : mcp::SequenceDefinitions())
		tools.push_back(std::move(definition));
	tools.push_back(mcp::SequenceCaptureDefinition());
	tools.push_back(mcp::LogDefinition());
	tools.push_back(mcp::ClearLogDefinition());
	return tools;
}

bool McpDebug::Handles(const std::string& name)
{
	static const std::set<std::string> names{"debug_connect", "game_bridge_connect", "debug_disconnect", "debug_status", "debug_acquire",
		"debug_renew", "debug_relinquish", "debug_pause", "debug_resume", "debug_step", "debug_strict_admission",
		"debug_display_step", "debug_cancel", "debug_release", "debug_operation", "debug_capture", "debug_acknowledge",
		"debug_inspect", "debug_revision", "debug_release_inspection", "debug_start_image_recording",
		"debug_stop_image_recording", "debug_pin_snapshot", "debug_unpin_snapshot", "debug_configure_snapshots"};
	return name == "read_logs" || name == "clear_logs" || name == "capture_screenshot" || name == "validate_stereo" || name == "analyze_stereo_consistency" || name == "debug_save_video" || name == "debug_save_image" || name == "capture_pose_sequence" || mcp::DiagnosticsHandles(name) || mcp::IncidentHandles(name) || mcp::ControlsHandles(name) || mcp::SequenceHandles(name) || names.contains(name) || mcp::SimulationHandles(name) || mcp::ImageHandles(name) || mcp::GameHandles(name) ||
		mcp::HistoryHandles(name);
}

static Json DebugReply(const wire::Message& message, bool metadataOnly = false)
{
	const auto value = McpDebug::Document(message, metadataOnly);
	return Result(value, !value.at("ok").get<bool>());
}

static void ValidateTarget(const ApplicationSnapshot& status, const std::string& endpoint,
	std::uint64_t process, std::uint64_t session, std::uint64_t generation, std::uint64_t epoch)
{
	if (!status.connected || !status.running || status.endpoint != endpoint || status.runtimeProcess != process ||
		status.runtimeEpoch != epoch || status.session != session || status.generation != generation)
		throw std::runtime_error("Application process or XR session changed; open a new debugger connection");
}
static void ValidateGameTarget(const ApplicationSnapshot& status, const std::string& endpoint,
	std::uint64_t process, std::uint64_t epoch, bool launched)
{
	if (status.launch.restartPending)
		throw std::runtime_error("The selected game process changed; open a new bridge connection");
	if (launched)
	{
		if (status.launch.state != LaunchState::Running || status.launch.process != process || status.launchEndpoint != endpoint)
			throw std::runtime_error("The selected game process changed; open a new bridge connection");
	}
	else if (!status.connected || status.endpoint != endpoint || status.runtimeProcess != process || status.runtimeEpoch != epoch)
		throw std::runtime_error("The selected runtime changed; open a new bridge connection");
}

class DebugTransportFailure : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};
static wire::Message Exchange(const std::shared_ptr<wire::LocalControlClient>& client, wire::OpCode opcode,
							  std::vector<wire::Field> fields)
{
	wire::ControlPacket packet;
	packet.message.opcode = static_cast<std::uint16_t>(opcode);
	packet.message.fields = std::move(fields);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool retry = false;
	try
	{
		for (;;)
		{
			try
			{
				return (retry ? client->Retry() : client->Request(std::move(packet))).message;
			}
			catch (const wire::ControlExchangeTimeout&)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					throw DebugTransportFailure("Runtime request timed out after admission; its outcome may be unknown. The connection was closed.");
				retry = true;
			}
		}
	}
	catch (const DebugTransportFailure&)
	{
		throw;
	}
	catch (const std::exception& error)
	{
		throw DebugTransportFailure(std::string("Runtime connection failed; the request outcome may be unknown: ") + error.what());
	}
}

static std::vector<wire::Field> SessionFields(std::uint64_t session, std::uint64_t generation)
{
	return {wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true)};
}

McpDebug::~McpDebug()
{
	Close();
}
void McpDebug::Close() noexcept
{
	std::unordered_map<std::string, std::shared_ptr<Entry>> retired;
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		retired.swap(m_entries);
	}
	for (const auto& [identity, entry] : retired)
		entry->Close();
}

std::optional<McpConnection> McpDebug::Connect(McpApplication& application, const std::string& target,
	const std::shared_ptr<McpCancellation>& cancellation, bool gameOnly)
{
	auto response = application.Lookup(target, cancellation);
	if (!response)
		return {};
	const auto& status = *response;
	if (!gameOnly && (!status.connected || !status.running))
		throw std::runtime_error("Select a connected application with one running XR session");
	if (!cancellation->TryAdmit())
		return {};
	auto entry = std::make_shared<Entry>();
	entry->application = target;
	entry->gameOnly = gameOnly;
	entry->launched = gameOnly && status.launch.state == LaunchState::Running &&
		!status.launchEndpoint.empty();
	if (entry->launched)
	{
		entry->endpoint = status.launchEndpoint;
		entry->process = status.launch.process;
	}
	else
	{
		if (!status.connected)
			throw std::runtime_error("Select a running launched game or connected runtime");
		entry->endpoint = status.endpoint;
		entry->process = status.runtimeProcess;
		entry->epoch = status.runtimeEpoch;
	}
	if (!gameOnly)
	{
		entry->session = status.session;
		entry->generation = status.generation;
	}
	else
		ValidateGameTarget(status, entry->endpoint, entry->process, entry->epoch, entry->launched);
	if (!entry->process || (!entry->launched && !entry->epoch) || (!gameOnly && (!entry->session || !entry->generation)))
		throw std::runtime_error("Application has an invalid process or session identity");
	const auto identity = NewMcpIdentifier();
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			throw std::runtime_error("MCP session is closed");
		if (m_entries.size() >= 8 || !m_entries.emplace(identity, entry).second)
			throw std::runtime_error("Debugger connection capacity reached");
	}
	std::shared_ptr<wire::LocalControlClient> client;
	try
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		for (;;)
		{
			if (entry->closed.load())
				throw std::runtime_error("MCP session is closed");
			try
			{
				client = std::make_shared<wire::LocalControlClient>(entry->endpoint, std::chrono::milliseconds(50));
				break;
			}
			catch (const wire::ControlConnectTimeout&)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					throw;
			}
		}
		if (client->PeerProcess() != entry->process)
			throw std::runtime_error("Runtime endpoint belongs to a different process");
		{
			std::lock_guard lock(entry->owner);
			if (entry->closed.load())
				throw std::runtime_error("MCP session is closed");
			entry->client = client;
		}
		const auto hello = Exchange(client, wire::OpCode::Hello,
			{wire::Field::Text(tag::ClientName, "OpenXR Simulator MCP"),
			 wire::Field::U64(tag::RequiredCapabilities, gameOnly ? 0 : static_cast<std::uint64_t>(wire::Capability::DebugCommands))});
		const auto* error = hello.Find(tag::Error);
		if (!error || error->AsU32() != 0)
			throw std::runtime_error("Runtime did not negotiate debugger capabilities");
		if (entry->closed.load())
			throw std::runtime_error("MCP session is closed");
		entry->capabilities = U64(hello, tag::Capabilities);
		entry->minor = hello.minor;
		if (gameOnly)
		{
			auto latest = application.Lookup(target, std::make_shared<McpCancellation>());
			if (!latest)
				throw std::runtime_error("The selected game is unavailable");
			ValidateGameTarget(*latest, entry->endpoint, entry->process, entry->epoch, entry->launched);
			if (entry->closed.load() || cancellation->Requested())
				throw std::runtime_error("Game bridge connection was cancelled");
		}
		return McpConnection{identity, entry->process, entry->epoch, entry->session, entry->generation, entry->capabilities};
	}
	catch (...)
	{
		Disconnect(identity);
		throw;
	}
}

void McpDebug::Disconnect(const std::string& connection) noexcept
{
	std::shared_ptr<Entry> entry;
	{
		std::lock_guard lock(m_mutex);
		const auto found = m_entries.find(connection);
		if (found == m_entries.end())
			return;
		entry = std::move(found->second);
		m_entries.erase(found);
	}
	entry->Close();
}
std::shared_ptr<McpDebug::Entry> McpDebug::FindConnection(const std::string& connection)
{
	std::shared_ptr<Entry> entry;
	{
		std::lock_guard lock(m_mutex);
		const auto found = m_entries.find(connection);
		if (m_closed || found == m_entries.end())
			throw std::runtime_error("Control connection is closed");
		entry = found->second;
	}
	return entry;
}
std::unique_lock<std::timed_mutex> McpDebug::LockConnection(McpApplication& application, const std::string& connection,
	const std::shared_ptr<Entry>& entry, const std::shared_ptr<McpCancellation>& cancellation)
{
	if (cancellation->Requested())
		throw std::runtime_error("Control request was cancelled");
	try
	{
		auto status = application.Lookup(entry->application, cancellation);
		if (!status)
			throw std::runtime_error("Control application is unavailable");
		ValidateTarget(*status, entry->endpoint,
			entry->process, entry->session, entry->generation, entry->epoch);
	}
	catch (...)
	{
		Disconnect(connection);
		throw;
	}
	std::unique_lock operation(entry->exchange, std::defer_lock);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!operation.try_lock_for(std::chrono::milliseconds(50)))
	{
		if (cancellation->Requested())
			throw std::runtime_error("Control request was cancelled");
		if (std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("Control connection is busy");
	}
	if (!cancellation->TryAdmit())
		throw std::runtime_error("Control request was cancelled");
	return operation;
}

static void ValidateExportRead(const wire::Message& response, std::uint64_t offset, std::uint64_t length)
{
	const auto* error = response.Find(tag::Error);
	if (!error || !error->AsU32())
		throw DebugTransportFailure("Control response omitted its error code");
	if (*error->AsU32())
		return;
	const auto* bytes = response.Find(tag::Bytes);
	const auto* returnedOffset = response.Find(tag::Offset);
	const auto* returnedLength = response.Find(tag::Length);
	if (!bytes || !returnedOffset || returnedOffset->AsU64() != offset || !returnedLength ||
		returnedLength->AsU64() != length || bytes->value.size() != length)
		throw DebugTransportFailure("Runtime returned an invalid artifact range");
}

control::Message McpDebug::Request(McpApplication& application, const std::string& connection,
	wire::OpCode opcode, std::vector<wire::Field> fields,
	const std::shared_ptr<McpCancellation>& requestedCancellation, std::optional<std::string_view> program)
{
	const bool imageWindow = opcode == wire::OpCode::BeginImageWindow || opcode == wire::OpCode::ReadImageWindow ||
		opcode == wire::OpCode::RetainWindowImage || opcode == wire::OpCode::ReleaseImageWindow;
	const bool sequence = opcode == wire::OpCode::UploadSequence || opcode == wire::OpCode::StartSequence ||
		opcode == wire::OpCode::ReadSequence || opcode == wire::OpCode::StopSequence || opcode == wire::OpCode::ReleaseSequenceCapture;
	const bool debugger = opcode == wire::OpCode::Acquire || opcode == wire::OpCode::Renew ||
		opcode == wire::OpCode::Relinquish || opcode == wire::OpCode::Submit || opcode == wire::OpCode::Operation;
	if (opcode != wire::OpCode::ReadExport && opcode != wire::OpCode::ExportInfo &&
		opcode != wire::OpCode::StartVideoExport && opcode != wire::OpCode::ReleaseExport &&
		opcode != wire::OpCode::Capture && opcode != wire::OpCode::RetainImage && opcode != wire::OpCode::ImageInfo &&
		opcode != wire::OpCode::ReleaseImage && opcode != wire::OpCode::StartExport && !sequence &&
		opcode != wire::OpCode::Status && opcode != wire::OpCode::ReadProjectionHistory &&
		opcode != wire::OpCode::SimulationStatus && opcode != wire::OpCode::ReadDiagnostics && !imageWindow && !debugger &&
		opcode != wire::OpCode::RetainPreview && opcode != wire::OpCode::InspectFrame && opcode != wire::OpCode::FeatureRevision)
		throw std::invalid_argument("Unsupported control request");
	if (program && (opcode != wire::OpCode::StartSequence || program->size() > 512 * 1024 || program->find('\0') != std::string_view::npos))
		throw std::invalid_argument("Sequence program must be JSONL text of at most 512 KiB without NUL bytes");
	wire::Message request;
	request.fields = std::move(fields);
	const auto* session = request.Find(tag::Session);
	const auto* generation = request.Find(tag::Generation);
	const auto offset = opcode == wire::OpCode::ReadExport ? U64(request, tag::Offset) : 0;
	const auto length = opcode == wire::OpCode::ReadExport ? U64(request, tag::Length) : 0;
	if (opcode == wire::OpCode::ReadExport && (!U64(request, tag::ExportId) || !length || length > wire::MaxPayload - 4096))
		throw std::invalid_argument("Invalid artifact range");
	const auto entry = FindConnection(connection);
	if (entry->gameOnly)
		throw std::invalid_argument("This connection only supports game bridge tools");
	if (bool(session) != bool(generation))
		throw std::invalid_argument("Supply both session and generation, or omit both");
	if (session && (session->AsU64() != entry->session || generation->AsU64() != entry->generation))
		throw std::invalid_argument("Session or generation does not match this debugger connection");
	if (sequence || debugger || opcode == wire::OpCode::BeginImageWindow || opcode == wire::OpCode::Capture ||
		opcode == wire::OpCode::RetainImage || opcode == wire::OpCode::RetainPreview || opcode == wire::OpCode::InspectFrame ||
		opcode == wire::OpCode::StartVideoExport ||
		opcode == wire::OpCode::Status || opcode == wire::OpCode::ReadProjectionHistory ||
		opcode == wire::OpCode::SimulationStatus || opcode == wire::OpCode::ReadDiagnostics)
	{
		if (!session)
		{
			request.fields.push_back(wire::Field::U64(tag::Session, entry->session, true));
			request.fields.push_back(wire::Field::U64(tag::Generation, entry->generation, true));
		}
	}
	else
		std::erase_if(request.fields, [](const auto& field)
		{
			return field.tag == tag::Session || field.tag == tag::Generation;
		});
	const auto cancellation = requestedCancellation ? requestedCancellation : std::make_shared<McpCancellation>();
	if (opcode == wire::OpCode::StartVideoExport &&
		!(entry->capabilities & static_cast<std::uint64_t>(wire::Capability::VideoExport)))
		throw std::runtime_error("The connected runtime does not support video export");
	if (sequence && !(entry->capabilities & static_cast<std::uint64_t>(wire::Capability::PoseSequences)))
		throw std::runtime_error("The connected runtime does not support pose sequences");
	if (opcode == wire::OpCode::StartExport && request.Find(tag::ImageRegion) &&
		!(entry->capabilities & static_cast<std::uint64_t>(wire::Capability::ImageRegionExport)))
		throw std::runtime_error("The connected runtime does not support image region export");
	if (((sequence && opcode != wire::OpCode::ReleaseSequenceCapture) ||
		opcode == wire::OpCode::SimulationStatus || opcode == wire::OpCode::ReadDiagnostics) && !request.Find(tag::SourceKind))
		request.fields.push_back(wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Mcp)));
	for (const auto& [operation, capability] : {std::pair{wire::OpCode::ReadProjectionHistory, wire::Capability::ProjectionHistory},
		{wire::OpCode::SimulationStatus, wire::Capability::SimulationCommands}, {wire::OpCode::ReadDiagnostics, wire::Capability::FrameDiagnostics}})
		if (opcode == operation && !(entry->capabilities & static_cast<std::uint64_t>(capability)))
			throw std::runtime_error("The connected runtime does not support this diagnostic operation");
	auto operation = LockConnection(application, connection, entry, cancellation);
	const auto client = entry->Client();
	wire::Message response;
	try
	{
		if (program)
		{
			std::size_t offset = 0;
			do
			{
				if (cancellation->Requested())
					throw DebugTransportFailure("Sequence upload was cancelled");
				const auto length = std::min<std::size_t>(256 * 1024, program->size() - offset);
				mcp::SequenceRequest chunk{wire::OpCode::UploadSequence,
					{wire::Field::U64(tag::Session, entry->session, true), wire::Field::U64(tag::Generation, entry->generation, true),
					 wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Mcp)),
					 wire::Field::U64(tag::ProgramId, U64(request, tag::ProgramId), true),
					 wire::Field::U64(tag::Offset, offset, true), wire::Field::U64(tag::TotalBytes, program->size(), true),
					 wire::Field::Bytes(tag::Bytes, std::as_bytes(std::span(program->data() + offset, length)), true)}};
				const auto uploaded = Exchange(client, chunk.opcode, chunk.fields);
				try
				{
					if (!mcp::SequenceDocument(uploaded, chunk).at("ok").get<bool>())
						return uploaded;
				}
				catch (const std::exception& error)
				{
					throw DebugTransportFailure(error.what());
				}
				offset += length;
			} while (offset < program->size());
			if (cancellation->Requested())
				throw DebugTransportFailure("Sequence upload was cancelled");
		}
		response = Exchange(client, opcode, std::move(request.fields));
		const auto* error = response.Find(tag::Error);
		if (!error || !error->AsU32())
			throw DebugTransportFailure("Control response omitted its error code");
		if (opcode == wire::OpCode::ReadExport)
			ValidateExportRead(response, offset, length);
		return response;
	}
	catch (const DebugTransportFailure&)
	{
		Disconnect(connection);
		throw;
	}
}

std::vector<std::byte> McpDebug::ReadArtifact(McpApplication& application, const std::string& connection,
	std::uint64_t ticket, std::uint64_t offset, std::uint64_t length,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	auto response = Request(application, connection, wire::OpCode::ReadExport,
		{wire::Field::U64(tag::ExportId, ticket, true), wire::Field::U64(tag::Offset, offset, true),
		 wire::Field::U64(tag::Length, length, true)}, cancellation);
	if (response.Find(tag::Error)->AsU32() != 0)
		throw std::runtime_error("Artifact export is unavailable or released");
	return std::move(response.Find(tag::Bytes)->value);
}

mcp::ImageRenderResult McpDebug::Render(McpApplication& application, const std::string& connection,
	const mcp::ImageRenderRequest& render, const std::shared_ptr<McpCancellation>& cancellation)
{
	if (!render.ticket || render.quality < 1 || render.quality > 100 || bool(render.session) != bool(render.generation))
		throw std::invalid_argument("Invalid image render request");
	const auto entry = FindConnection(connection);
	if (entry->gameOnly)
		throw std::invalid_argument("This connection only supports game bridge tools");
	if (render.session && (render.session != entry->session || render.generation != entry->generation))
		throw std::invalid_argument("Session or generation does not match this debugger connection");
	auto operation = LockConnection(application, connection, entry, cancellation);
	const auto client = entry->Client();
	try
	{
		mcp::ImageRenderResult result{Exchange(client, wire::OpCode::ExportInfo,
			{wire::Field::U64(tag::ExportId, render.ticket, true)}), {}};
		const auto& info = result.info;
		const auto* error = info.Find(tag::Error);
		if (!error || !error->AsU32())
			throw DebugTransportFailure("Control response omitted its error code");
		if (*error->AsU32())
			return result;
		if (U64(info, tag::ExportId) != render.ticket)
			throw DebugTransportFailure("Runtime returned a different image export");
		const auto metadata = mcp::ImageDocument(info);
		if (metadata.value("phase", "") != "complete")
			throw std::runtime_error("Export is not complete; poll debug_export_info first");
		const auto* descriptor = info.Find(tag::ImageDescriptor);
		if (!descriptor)
			throw std::runtime_error("This export is not a raw image");
		const auto description = wire::DecodeImageDescription(*descriptor);
		const auto pixelBytes = ColorPixelBytes(description.format);
		const auto total = U64(info, tag::TotalBytes);
		if (!pixelBytes || !description.width || !description.height || description.arrayLayers != 1 ||
			description.width > 4096 || description.height > 4096 ||
			std::uint64_t{description.width} * description.height > MaxImageInput / pixelBytes ||
			total != std::uint64_t{description.width} * description.height * pixelBytes)
			throw std::runtime_error("Choose a supported single-layer image within the 32 MiB encoding limit");
		const auto check = [&]
		{
			if (cancellation->Requested() || entry->closed.load())
				throw std::runtime_error("Image rendering was interrupted");
		};
		check();
		std::string cacheKey;
		// An export ticket is connection-owned; validate it above before sharing
		// encoded content identified by the immutable source and selection.
		if (metadata.contains("frame") && metadata.contains("target") && metadata.contains("capture") &&
			metadata.contains("image_description"))
		{
			const auto* tickField = info.Find(tag::ImageTick);
			const auto tick = tickField ? tickField->AsU64() : std::optional<std::uint64_t>{};
			// A live display can change while the submitted frame stays the same.
			if (mcp::CacheableImageSource(metadata.at("target").at("kind").get<std::uint32_t>(), tickField != nullptr, tick.has_value()))
			{
				// Older providers can omit the selected layer or region. In that
				// case reuse only this validated ticket, not another export.
				const bool selectionKnown = metadata.contains("layer") && metadata.contains("region");
				const Json key = Json::array({entry->application, entry->endpoint, entry->process, entry->epoch,
					entry->session, entry->generation, metadata.at("frame"), metadata.at("target"), metadata.at("capture"),
					tick ? Json(std::to_string(*tick)) : Json(nullptr), metadata.value("layer", Json(nullptr)),
					metadata.value("region", Json(nullptr)), metadata.at("image_description"), render.png, render.quality,
					selectionKnown ? Json(nullptr) : Json(connection), selectionKnown ? Json(nullptr) : Json(render.ticket)});
				cacheKey = key.dump();
				if (const auto cached = s_imageCache.Find(cacheKey))
				{
					operation.unlock();
					check();
					result.image = cached;
					return result;
				}
			}
		}
		ImageEncodingPermit permit;
		std::vector<std::byte> pixels;
		pixels.reserve(static_cast<std::size_t>(total));
		control::ExportReader::Read([&](auto opcode, auto fields)
		{
			check();
			auto chunk = Exchange(client, opcode, std::move(fields));
			const auto* error = chunk.Find(tag::Error);
			if (!error || !error->AsU32())
				throw DebugTransportFailure("Control response omitted its error code");
			if (*error->AsU32())
				throw std::runtime_error("Image export is unavailable or released");
			return chunk;
		}, render.ticket, total, [&](std::span<const std::byte> bytes)
		{
			pixels.insert(pixels.end(), bytes.begin(), bytes.end());
		});
		operation.unlock();
		check();
		const auto encoded = EncodeImage(pixels, description.format, description.width, description.height, render.png, render.quality);
		check();
		auto image = std::make_shared<mcp::CachedImage>();
		image->mediaType = encoded.mediaType;
		image->lossy = encoded.lossy;
		image->encodedBytes = encoded.bytes.size();
		image->data = mcp::EncodeBase64(encoded.bytes);
		check();
		if (!cacheKey.empty())
			s_imageCache.Insert(std::move(cacheKey), image);
		result.image = std::move(image);
		return result;
	}
	catch (const DebugTransportFailure&)
	{
		Disconnect(connection);
		throw;
	}
}

std::optional<Json> McpDebug::Call(McpApplication& application, const std::string& name, const Json& arguments,
									  const std::shared_ptr<McpCancellation>& cancellation)
{
	std::shared_ptr<Entry> entry;
	std::string identity;
	const auto retire = [&]
	{
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_entries.find(identity);
			if (found != m_entries.end() && found->second == entry)
				m_entries.erase(found);
		}
		if (entry)
			entry->Close();
	};
	try
	{
		if (name == "read_logs" || name == "clear_logs")
			return mcp::LogCall(application, arguments, name == "clear_logs", cancellation);
		if (name == "get_headset_profiles")
		{
			Keys(arguments, {});
			return Result(mcp::HeadsetProfilesDocument());
		}
		if (name == "capture_screenshot")
			return mcp::CaptureScreenshot(application, arguments, cancellation);
		if (name == "validate_stereo")
			return mcp::ValidateStereo(application, arguments, cancellation);
		if (name == "analyze_stereo_consistency")
			return mcp::CaptureStereoConsistency(application, arguments, cancellation);
		if (name == "debug_render_export")
		{
			const auto render = mcp::PrepareImageRender(arguments);
			const auto rendered = Render(application, Text(arguments, "connection"), render, cancellation);
			if (cancellation->Requested())
				return {};
			auto metadata = mcp::ImageDocument(rendered.info);
			if (!metadata.at("ok").get<bool>())
				return Result(metadata, true);
			const auto& image = *rendered.image;
			metadata["media_type"] = image.mediaType;
			metadata["lossy"] = image.lossy;
			metadata["encoded_bytes"] = image.encodedBytes;
			if (image.lossy && render.png)
				metadata["conversion"] = "clamped to 8-bit RGBA";
			return Json{{"content", Json::array({{{"type", "text"}, {"text", metadata.dump()}},
				{{"type", "image"}, {"mimeType", image.mediaType}, {"data", image.data}}})}, {"isError", false}};
		}
		if (name == "debug_save_video")
			return mcp::SaveVideo(application, *this, arguments, cancellation);
		if (name == "debug_save_image")
			return mcp::SaveImage(application, *this, arguments, cancellation);
		if (name == "capture_pose_sequence")
			return mcp::CaptureSequence(application, *this, arguments, cancellation);
		if (mcp::SequenceHandles(name))
		{
			identity = Text(arguments, "connection");
			if (arguments.contains("session") != arguments.contains("generation"))
				throw std::invalid_argument("Supply both session and generation, or omit both");
			auto request = mcp::PrepareSequence(name, arguments,
				arguments.contains("session") ? Number(arguments, "session") : 0,
				arguments.contains("generation") ? Number(arguments, "generation") : 0);
			if (!arguments.contains("session"))
				std::erase_if(request.fields, [](const auto& field) { return field.tag == tag::Session || field.tag == tag::Generation; });
			const auto response = Request(application, identity, request.opcode, request.fields, cancellation,
				arguments.contains("program") ? std::optional<std::string_view>{arguments.at("program").get_ref<const std::string&>()} : std::nullopt);
			try
			{
				return mcp::SequenceReply(response, request);
			}
			catch (...)
			{
				Disconnect(identity);
				throw;
			}
		}
		if (mcp::DiagnosticsHandles(name))
			return mcp::RunDiagnostics(application, name, arguments, cancellation);
		if (mcp::IncidentHandles(name))
			return mcp::CaptureIncident(application, name, arguments, cancellation);
		if (name == "debug_connect" || name == "game_bridge_connect")
		{
			Keys(arguments, {"application_id"});
			const auto target = Text(arguments, "application_id");
			const bool gameOnly = name == "game_bridge_connect";
			const auto opened = Connect(application, target, cancellation, gameOnly);
			if (!opened)
				return {};
			Json result = {{"ok", true}, {"connection", opened->id}, {"application_id", target},
				{"capabilities", std::to_string(opened->capabilities)}};
			if (gameOnly)
				result["process_id"] = std::to_string(opened->process);
			else
			{
				result["runtime_process_id"] = std::to_string(opened->process);
				result["runtime_epoch"] = std::to_string(opened->epoch);
				result["session"] = std::to_string(opened->session);
				result["generation"] = std::to_string(opened->generation);
				result["sessions"] = Json::array({{{"session", result["session"]}, {"generation", result["generation"]}}});
			}
			return Result(result);
		}

		identity = Text(arguments, "connection");
		{
			std::lock_guard lock(m_mutex);
			const auto found = m_entries.find(identity);
			if (m_closed || found == m_entries.end())
				throw std::runtime_error("Unknown debugger connection for this MCP session");
			entry = found->second;
		}
		if (arguments.contains("session") != arguments.contains("generation"))
			throw std::invalid_argument("Supply both session and generation, or omit both");
		if (arguments.contains("session") &&
			(Number(arguments, "session") != entry->session || Number(arguments, "generation") != entry->generation))
			throw std::invalid_argument("Session or generation does not match this debugger connection");
		if (name == "debug_disconnect")
		{
			Keys(arguments, {"connection", "session", "generation"});
			if (!cancellation->TryAdmit())
				return {};
			retire();
			return Result({{"ok", true}, {"connection", identity}, {"disconnected", true}});
		}
		if (entry->gameOnly && !mcp::GameHandles(name))
			throw std::invalid_argument("This connection only supports game bridge tools");
		try
		{
			const auto status = application.Lookup(entry->application, cancellation);
			if (!status)
				return {};
			if (entry->gameOnly)
				ValidateGameTarget(*status, entry->endpoint, entry->process, entry->epoch, entry->launched);
			else
				ValidateTarget(*status, entry->endpoint,
							   entry->process, entry->session, entry->generation, entry->epoch);
		}
		catch (...)
		{
			retire();
			throw;
		}
		std::unique_lock operation(entry->exchange, std::defer_lock);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!operation.try_lock_for(std::chrono::milliseconds(50)))
		{
			if (cancellation->Requested())
				return {};
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("Debugger connection is busy; retry explicitly");
		}
		std::shared_ptr<wire::LocalControlClient> client;
		{
			std::lock_guard lock(entry->owner);
			if (entry->closed.load() || !entry->client)
				throw std::runtime_error("Debugger connection is closed");
			client = entry->client;
		}
		if (mcp::GameHandles(name))
		{
			auto request = mcp::PrepareGame(name, arguments, entry->session, entry->generation);
			if (!cancellation->TryAdmit())
				return {};
			try
			{
				const auto response = Exchange(client, request.opcode, std::move(request.fields));
				try
				{
					return mcp::GameReply(response, request.opcode);
				}
				catch (const std::exception& error)
				{
					throw DebugTransportFailure(std::string("Invalid runtime response; the request may have applied: ") + error.what());
				}
			}
			catch (...)
			{
				retire();
				throw;
			}
		}
		if (mcp::HistoryHandles(name))
		{
			const auto required = name == "debug_frame_history" ? wire::Capability::FrameHistory : wire::Capability::ProjectionHistory;
			if (!(entry->capabilities & static_cast<std::uint64_t>(required)))
				throw std::runtime_error("The connected runtime does not support this history operation");
			auto request = mcp::PrepareHistory(name, arguments, entry->session, entry->generation);
			if (!cancellation->TryAdmit())
				return {};
			try
			{
				const auto response = Exchange(client, request.opcode, std::move(request.fields));
				return mcp::HistoryReply(response, request, entry->generation);
			}
			catch (...)
			{
				retire();
				throw;
			}
		}
		if (mcp::ImageHandles(name))
		{
			if (name == "debug_start_video_export" && !(entry->capabilities & static_cast<std::uint64_t>(wire::Capability::VideoExport)))
				throw std::runtime_error("The connected runtime does not support video export");
			if (arguments.contains("region") && !(entry->capabilities & static_cast<std::uint64_t>(wire::Capability::ImageRegionExport)))
				throw std::runtime_error("The connected runtime does not support image region export");
			auto request = mcp::PrepareImage(name, arguments, entry->session, entry->generation);
			if (!cancellation->TryAdmit())
				return {};
			try
			{
				const auto response = Exchange(client, request.opcode, std::move(request.fields));
				try
				{
					return mcp::ImageReply(response);
				}
				catch (const std::exception& error)
				{
					throw DebugTransportFailure(std::string("Invalid runtime response; the request may have applied: ") + error.what());
				}
			}
			catch (...)
			{
				retire();
				throw;
			}
		}
		if (mcp::ControlsHandles(name))
		{
			if (!(entry->capabilities & static_cast<std::uint64_t>(wire::Capability::SimulationCommands)))
				throw std::runtime_error("The connected runtime does not support simulation commands");
			if (entry->minor < 17)
				throw std::runtime_error("Controls controls require runtime control protocol 1.17");
			const auto transaction = Number(arguments, "transaction_id");
			const auto read = [&](std::optional<std::uint64_t> id = {})
			{
				std::vector<wire::Field> fields{wire::Field::U64(tag::Session, entry->session, true),
					wire::Field::U64(tag::Generation, entry->generation, true),
					wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Mcp))};
				if (id)
					fields.push_back(wire::Field::U64(tag::TransactionId, *id, true));
				try
				{
					const auto result = mcp::SimulationDocument(Exchange(client, wire::OpCode::SimulationStatus, std::move(fields)));
					if (!result.at("ok").get<bool>())
						throw DebugTransportFailure("Could not read simulation state; the connection was closed");
					return result;
				}
				catch (...)
				{
					retire();
					throw;
				}
			};
			for (const auto& [id, saved] : entry->controlEditRequests)
			{
				if (id != transaction)
					continue;
				if (saved != Json{{"name", name}, {"arguments", arguments}})
					throw std::invalid_argument("Transaction ID was already used for different control arguments");
				auto result = read(transaction);
				result["transaction_id"] = std::to_string(transaction);
				result["replayed_receipt"] = true;
				return Result(result);
			}
			if (transaction <= entry->controlEditLast)
				throw std::invalid_argument("Transaction ID is retired; use a new increasing ID");
			if (entry->controlEditPending)
			{
				const auto prior = read(*entry->controlEditPending);
				const auto phase = prior.value("phase", "");
				if (phase == "queued" || phase == "rescheduled")
					throw std::runtime_error("The previous input edit is pending; advance the application frame and retry explicitly");
				entry->controlEditPending.reset();
				if (phase != "applied")
					throw std::runtime_error("The previous input edit did not apply; inspect its transaction and retry explicitly");
			}
			if (cancellation->Requested())
				return {};
			const auto observed = read();
			const auto& document = observed.at("document");
			if (Number(document, "pending", true))
				throw std::runtime_error("Simulation transactions are pending; wait for them before editing applied state");
			const auto frame = Number(document, "frame_id", true);
			if (frame == UINT64_MAX)
				throw std::runtime_error("Simulation frame identity is exhausted");
			const auto commands = mcp::ControlsCommands(name, arguments, document.at("configuration"));
			const auto expected = mcp::ControlsExpectedInputs(name, document.at("configuration"));
			std::vector<wire::Field> fields{wire::Field::U64(tag::Session, entry->session, true),
				wire::Field::U64(tag::Generation, entry->generation, true),
				wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Mcp)),
				wire::Field::U64(tag::TransactionId, transaction, true), wire::Field::U64(tag::ApplyFrame, frame + 1, true),
				wire::Field::U32(tag::LatePolicy, static_cast<std::uint32_t>(CommandLatePolicy::RescheduleNext))};
			const auto encode = [&](std::uint16_t tagValue, const auto& values)
			{
				std::vector<wire::Field> encoded;
				for (const auto& command : values)
					encoded.push_back(wire::EncodeSimulationCommand(command, static_cast<std::uint16_t>(encoded.size() + 1)));
				fields.push_back(wire::Field::Bytes(tagValue, wire::EncodeFields(encoded), true));
			};
			encode(tag::Commands, commands);
			if (!expected.empty())
				encode(tag::ExpectedInputs, expected);
			(void)wire::EncodeFields(fields);
			if (!cancellation->TryAdmit())
				return {};
			try
			{
				auto result = mcp::SimulationDocument(Exchange(client, wire::OpCode::SubmitSimulation, std::move(fields)));
				if (result.at("ok").get<bool>())
				{
					const auto phase = result.value("phase", "");
					if (phase == "queued" || phase == "rescheduled")
					{
						entry->controlEditPending = transaction;
						entry->controlEditLast = transaction;
						entry->controlEditRequests.emplace_back(transaction, Json{{"name", name}, {"arguments", arguments}});
						if (entry->controlEditRequests.size() > 64)
							entry->controlEditRequests.pop_front();
					}
				}
				return Result(result, !result.at("ok").get<bool>());
			}
			catch (...)
			{
				retire();
				throw;
			}
		}
		if (mcp::SimulationHandles(name))
		{
			const auto required = name == "simulation_diagnostics" ? wire::Capability::FrameDiagnostics
				: (name == "simulation_start_recording" || name == "simulation_stop_recording" || name == "simulation_read_recording" || name == "simulation_acknowledge_recording")
					? wire::Capability::CommandRecording : wire::Capability::SimulationCommands;
			if (!(entry->capabilities & static_cast<std::uint64_t>(required)))
				throw std::runtime_error("The connected runtime does not support this simulation operation");
			auto request = mcp::PrepareSimulation(name, arguments, entry->session, entry->generation);
			if (!cancellation->TryAdmit())
				return {};
			try
			{
				const auto response = Exchange(client, request.opcode, std::move(request.fields));
				try
				{
					return mcp::SimulationReply(response);
				}
				catch (const std::exception& error)
				{
					throw DebugTransportFailure(std::string("Invalid runtime response; the request may have applied: ") + error.what());
				}
			}
			catch (...)
			{
				retire();
				throw;
			}
		}
		if (name == "debug_inspect" || name == "debug_revision" || name == "debug_release_inspection")
		{
			const bool inspect = name == "debug_inspect";
			const bool revision = name == "debug_revision";
			const auto key = inspect ? "capture" : revision ? "revision" : "inspection";
			if (inspect)
				Keys(arguments, {"connection", "session", "generation", "capture"});
			else if (revision)
				Keys(arguments, {"connection", "session", "generation", "revision"});
			else
				Keys(arguments, {"connection", "session", "generation", "inspection"});
			const auto requested = Number(arguments, key);
			auto fields = inspect ? SessionFields(entry->session, entry->generation) : std::vector<wire::Field>{};
			const auto field = inspect ? tag::CaptureId : revision ? tag::RevisionId : tag::InspectionId;
			fields.push_back(wire::Field::U64(field, requested, true));
			const auto opcode = inspect ? wire::OpCode::InspectFrame
										: revision ? wire::OpCode::FeatureRevision : wire::OpCode::ReleaseInspection;
			if (!cancellation->TryAdmit())
				return {};
			try
			{
				const auto response = Exchange(client, opcode, std::move(fields));
				const auto parsed = DebugReply(response);
				if (parsed.value("isError", true))
					return parsed;
				if (U64(response, field) != requested)
					throw std::runtime_error("Runtime returned a different inspection identity");
				Json result = {{"ok", true}, {key, std::to_string(requested)}};
				if (inspect)
				{
					result["inspection"] = std::to_string(U64(response, tag::InspectionId));
					const auto frame = Nested(response.Find(tag::Frame));
					result["frame"] = {{"generation", std::to_string(U64(frame, tag::Generation))},
										   {"frame_id", std::to_string(U64(frame, tag::FrameId))}};
				}
				if (revision)
				{
					const auto* kind = response.Find(tag::RevisionKind);
					if (!kind || !kind->AsText())
						throw std::runtime_error("Runtime omitted the feature revision kind");
					result["kind"] = std::string(*kind->AsText());
				}
				if (inspect || revision)
				{
					const auto* document = response.Find(tag::Document);
					if (!document)
						throw std::runtime_error("Runtime omitted the inspection document");
					result["document"] = mcp::InspectionJson(document->value);
				}
				else
				{
					const auto* released = response.Find(tag::Released);
					if (!released || !released->AsBool())
						throw std::runtime_error("Runtime omitted the inspection release result");
					result["released"] = *released->AsBool();
				}
				return Result(result);
			}
			catch (...)
			{
				retire();
				throw;
			}
		}
		auto fields = SessionFields(entry->session, entry->generation);
		wire::OpCode opcode;
		if (name == "debug_status" || name == "debug_acquire")
		{
			Keys(arguments, {"connection", "session", "generation"});
			opcode = name == "debug_status" ? wire::OpCode::Status : wire::OpCode::Acquire;
		}
		else if (name == "debug_renew" || name == "debug_relinquish")
		{
			Keys(arguments, {"connection", "session", "generation", "lease"});
			fields.push_back(wire::Field::U64(tag::Lease, Number(arguments, "lease"), true));
			opcode = name == "debug_renew" ? wire::OpCode::Renew : wire::OpCode::Relinquish;
		}
		else if (name == "debug_operation" || name == "debug_capture")
		{
			const bool operationQuery = name == "debug_operation";
			Keys(arguments, operationQuery ? std::initializer_list<std::string_view>{"connection", "session", "generation", "operation"}
										  : std::initializer_list<std::string_view>{"connection", "session", "generation", "capture"});
			fields.push_back(wire::Field::U64(operationQuery ? tag::OperationId : tag::CaptureId,
												Number(arguments, operationQuery ? "operation" : "capture"), true));
			opcode = operationQuery ? wire::OpCode::Operation : wire::OpCode::Capture;
		}
		else if (name == "debug_acknowledge")
		{
			Keys(arguments, {"connection", "session", "generation", "lease", "through"});
			fields.push_back(wire::Field::U64(tag::Lease, Number(arguments, "lease"), true));
			fields.push_back(wire::Field::U64(tag::CommandId, Number(arguments, "through"), true));
			opcode = wire::OpCode::Acknowledge;
		}
		else
		{
			Keys(arguments, {"connection", "session", "generation", "lease", "command_id", "operation", "capture", "lookahead", "target", "layer", "view", "advance_display", "capacity", "frames", "budget_mib"});
			const bool step = name == "debug_step" || name == "debug_strict_admission";
			const bool recording = name == "debug_start_image_recording";
			const bool captureCommand = name == "debug_release" || name == "debug_pin_snapshot" || name == "debug_unpin_snapshot";
			if ((!step && (arguments.contains("lookahead") || arguments.contains("advance_display"))) ||
				(!step && !recording && (arguments.contains("target") || arguments.contains("layer") || arguments.contains("view"))) ||
				(!recording && (arguments.contains("capacity") || arguments.contains("frames"))) ||
				(arguments.contains("budget_mib") != (name == "debug_configure_snapshots")))
				throw std::invalid_argument("Options do not match this debugger command");
			if (arguments.contains("operation") != (name == "debug_cancel") ||
				arguments.contains("capture") != captureCommand)
				throw std::invalid_argument("Operation or capture does not match this command");
			wire::CommandKind kind;
			if (name == "debug_pause") kind = wire::CommandKind::Pause;
			else if (name == "debug_resume") kind = wire::CommandKind::Resume;
			else if (name == "debug_step") kind = wire::CommandKind::Step;
			else if (name == "debug_strict_admission") kind = wire::CommandKind::StrictAdmission;
			else if (name == "debug_display_step") kind = wire::CommandKind::DisplayStep;
			else if (name == "debug_cancel") kind = wire::CommandKind::Cancel;
			else if (name == "debug_release") kind = wire::CommandKind::Release;
			else if (name == "debug_pin_snapshot") kind = wire::CommandKind::PinSnapshot;
			else if (name == "debug_unpin_snapshot") kind = wire::CommandKind::UnpinSnapshot;
			else if (name == "debug_configure_snapshots") kind = wire::CommandKind::ConfigureSnapshots;
			else if (recording) kind = wire::CommandKind::StartImageRecording;
			else if (name == "debug_stop_image_recording") kind = wire::CommandKind::StopImageRecording;
			else throw std::invalid_argument("Unknown debugger tool");
			fields.push_back(wire::Field::U64(tag::Lease, Number(arguments, "lease"), true));
			fields.push_back(wire::Field::U64(tag::CommandId, Number(arguments, "command_id"), true));
			fields.push_back(wire::Field::U32(tag::CommandKind, static_cast<std::uint32_t>(kind), true));
			if (name == "debug_cancel" || captureCommand)
				fields.push_back(wire::Field::U64(name == "debug_cancel" ? tag::OperationId : tag::CaptureId,
												Number(arguments, name == "debug_cancel" ? "operation" : "capture"), true));
			if (step || recording)
			{
				const auto bounded = [&](const char* key, std::uint32_t fallback, std::uint32_t maximum)
				{
					const auto found = arguments.find(key);
					if (found == arguments.end())
						return fallback;
					if (!found->is_number_unsigned() && !found->is_number_integer())
						throw std::invalid_argument(std::string(key) + " must be a bounded integer");
					if (*found < 0 || *found > maximum)
						throw std::invalid_argument(std::string(key) + " is out of range");
					return found->get<std::uint32_t>();
				};
				const auto lookahead = bounded("lookahead", 1, 4);
				if (!lookahead)
					throw std::invalid_argument("lookahead must be at least one");
				const auto layer = bounded("layer", 0, UINT32_MAX), view = bounded("view", 0, UINT32_MAX);
				const auto target = arguments.contains("target") ? Text(arguments, "target") : "composed_eyes";
				wire::TargetKind capture;
				if (target == "composed_eyes") capture = wire::TargetKind::ComposedEyes;
				else if (target == "projection_layers") capture = wire::TargetKind::ProjectionLayers;
				else if (target == "quad_layers") capture = wire::TargetKind::QuadLayers;
				else if (target == "layer_view") capture = wire::TargetKind::LayerView;
				else if (target == "layer_depth") capture = wire::TargetKind::LayerDepth;
				else if (target == "layer_depth_linear") capture = wire::TargetKind::LayerDepthLinear;
				else throw std::invalid_argument("Unknown capture target");
				if ((capture == wire::TargetKind::ComposedEyes || capture == wire::TargetKind::ProjectionLayers ||
					 capture == wire::TargetKind::QuadLayers) && (layer || view))
					throw std::invalid_argument("This capture target has no layer or view index");
				if (step)
					fields.push_back(wire::Field::U32(tag::Lookahead, lookahead));
				const std::array targetFields{
					wire::Field::U32(tag::CaptureKind, static_cast<std::uint32_t>(capture), true),
					wire::Field::U32(tag::Layer, layer), wire::Field::U32(tag::View, view)};
				fields.push_back(wire::Field::Bytes(tag::Target, wire::EncodeFields(targetFields)));
				if (const auto found = arguments.find("advance_display"); found != arguments.end())
				{
					if (!found->is_boolean())
						throw std::invalid_argument("advance_display must be boolean");
					fields.push_back(wire::Field::Bool(tag::AdvanceDisplay, found->get<bool>()));
				}
				if (recording)
				{
					const auto capacity = bounded("capacity", 2, 4);
					if (!capacity)
						throw std::invalid_argument("capacity must be at least one");
					fields.push_back(wire::Field::U32(tag::Capacity, capacity, true));
					fields.push_back(wire::Field::U64(tag::Length, arguments.contains("frames") ? Number(arguments, "frames", true) : 0, true));
				}
			}
			if (name == "debug_configure_snapshots")
			{
				const auto budget = Number(arguments, "budget_mib");
				if (!arguments.at("budget_mib").is_number_integer() || budget < 128 || budget > 8192)
					throw std::invalid_argument("budget_mib must be an integer from 128 to 8192");
				fields.push_back(wire::Field::U32(tag::SnapshotBudgetMiB, static_cast<std::uint32_t>(budget)));
			}
			opcode = wire::OpCode::Submit;
		}
		if (!cancellation->TryAdmit())
			return {};
		try
		{
			return DebugReply(Exchange(client, opcode, std::move(fields)), name == "debug_capture");
		}
		catch (...)
		{
			retire();
			throw;
		}
	}
	catch (const std::exception& error)
	{
		if ((mcp::SequenceHandles(name) || name == "debug_render_export") && cancellation->Requested())
			return {};
		return Failure(error.what());
	}
}
} // namespace simxr::app
