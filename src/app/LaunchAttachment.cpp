#include "app/LaunchAttachment.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimeEndpoints.h"
#include "control/ControlSchema.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace simxr::app
{
namespace
{
std::string Environment(const LaunchProfile& profile, const char* name)
{
	for (auto entry = profile.environment.rbegin(); entry != profile.environment.rend(); ++entry)
	{
#ifdef _WIN32
		if (_stricmp(entry->first.c_str(), name) == 0)
#else
		if (entry->first == name)
#endif
			return entry->second;
	}
#ifdef _WIN32
	return windows::Environment(name);
#else
	const auto value = std::getenv(name);
	return value ? value : "";
#endif
}
#ifndef _WIN32
std::filesystem::path DataDirectory(const LaunchProfile& profile)
{
	if (const auto directory = Environment(profile, "SIMXR_DATA_DIR"); !directory.empty())
		return directory;
	if (const auto directory = Environment(profile, "XDG_DATA_HOME"); !directory.empty())
		return std::filesystem::path(directory) / "OpenXR-Simulator";
	if (const auto home = Environment(profile, "HOME"); !home.empty())
		return std::filesystem::path(home) / ".local/share/OpenXR-Simulator";
	throw std::runtime_error("Launch attachment needs an explicit absolute SIMXR_DATA_DIR or HOME");
}
#endif
std::optional<LaunchedSession> UniqueSession(const control::Message& message, const std::string& endpoint)
{
	const auto* error = message.Find(control::tag::Error);
	if (!error || error->AsU32() != static_cast<std::uint32_t>(control::ErrorCode::Ok))
		throw std::runtime_error("Launched runtime rejected session discovery");
	const auto* catalog = message.Find(control::tag::Sessions);
	if (!catalog)
		throw std::runtime_error("Launched runtime omitted its session catalog");
	const auto sessions = control::DecodeFields(catalog->value);
	if (sessions.error != control::DecodeError::None)
		throw std::runtime_error("Launched runtime returned a malformed session catalog");
	std::optional<LaunchedSession> selected;
	for (const auto& entry : sessions.fields)
	{
		auto decoded = control::DecodeFields(entry.value);
		if (decoded.error != control::DecodeError::None)
			throw std::runtime_error("Launched runtime returned a malformed session");
		control::Message session;
		session.fields = std::move(decoded.fields);
		const auto* running = session.Find(control::tag::Running);
		if (!running || !running->AsBool())
			throw std::runtime_error("Invalid launched session state");
		if (!*running->AsBool())
			continue;
		const auto* id = session.Find(control::tag::Session);
		const auto* generation = session.Find(control::tag::Generation);
		if (!id || !generation || !id->AsU64().value_or(0) || !generation->AsU64().value_or(0))
			throw std::runtime_error("Invalid launched session identity");
		if (selected)
			throw std::runtime_error("Launched runtime has multiple running sessions; select one explicitly");
		selected = LaunchedSession{endpoint, *id->AsU64(), *generation->AsU64()};
	}
	return selected;
}
} // namespace

LaunchAttachment::LaunchAttachment(RuntimeConnection& discovery, const LaunchProfile& profile)
	: m_discovery(discovery), m_customEndpoint(Environment(profile, "SIMXR_CONTROL_SOCKET")),
	  m_deadline(std::chrono::steady_clock::now() + std::chrono::seconds{30})
{
#ifdef _WIN32
	for (const auto& endpoint : DiscoverRuntimeEndpoints(false))
		m_previous.insert(endpoint);
	if (!m_customEndpoint.empty())
	{
		(void)LocalPipePath(m_customEndpoint);
		if (RuntimeEndpointExists(m_customEndpoint))
			throw std::runtime_error("Launch attachment requires a new local control endpoint");
	}
#else
	m_directory = DataDirectory(profile);
	if (!m_directory.is_absolute())
		throw std::runtime_error("The launched runtime data directory must be absolute");
	std::error_code error;
	for (std::filesystem::directory_iterator entry(m_directory, error), end; !error && entry != end;
		 entry.increment(error))
	{
		if (m_previous.size() == 4096)
			throw std::runtime_error("Too many existing launch endpoints");
		m_previous.insert(entry->path().string());
	}
	if (!m_customEndpoint.empty() &&
		(!std::filesystem::path(m_customEndpoint).is_absolute() || std::filesystem::exists(m_customEndpoint)))
		throw std::runtime_error("Launch attachment requires a new absolute local control endpoint");
#endif
}
LaunchAttachment::~LaunchAttachment()
{
	if (const auto* connecting = std::get_if<Connecting>(&m_state))
		m_discovery.Disconnect(connecting->epoch);
	else if (const auto* waiting = std::get_if<WaitingForSession>(&m_state))
		m_discovery.Disconnect(waiting->epoch);
}
void LaunchAttachment::Observe(const LaunchSnapshot& process)
{
	if (std::holds_alternative<Complete>(m_state))
		return;
	if (process.state == LaunchState::Failed || process.state == LaunchState::Exited)
		throw std::runtime_error("Launched application ended before attachment: " + process.detail);
	if (process.state != LaunchState::Running || !process.process)
		return;
	if (m_process && *m_process != process.process)
		throw std::runtime_error("Launched process identity changed during attachment");
	m_process = process.process;
	if (std::holds_alternative<WaitingForProcess>(m_state))
		m_state = WaitingForEndpoint{process.process};
}
std::string LaunchAttachment::ExpectedEndpoint(std::uint64_t process) const
{
	if (!m_customEndpoint.empty())
		return m_customEndpoint;
#ifdef _WIN32
	return std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(process);
#else
	return (m_directory / ("control." + std::to_string(process) + ".sock")).string();
#endif
}
bool LaunchAttachment::EndpointExists(const std::string& endpoint) const
{
#ifdef _WIN32
	return RuntimeEndpointExists(endpoint);
#else
	std::error_code error;
	return std::filesystem::is_socket(endpoint, error);
#endif
}
std::optional<LaunchedSession> LaunchAttachment::Poll()
{
	if (std::holds_alternative<Complete>(m_state))
		return {};
	if (std::chrono::steady_clock::now() >= m_deadline)
		throw std::runtime_error("Launched application did not expose a unique running session before the attach "
								 "deadline; attach explicitly");
	if (std::holds_alternative<WaitingForProcess>(m_state))
		return {};
	if (const auto* waiting = std::get_if<WaitingForEndpoint>(&m_state))
	{
		const auto endpoint = ExpectedEndpoint(waiting->process);
		if (m_previous.contains(endpoint))
			throw std::runtime_error("Refusing to attach a launch to a pre-existing endpoint");
		if (!EndpointExists(endpoint))
			return {};
		m_state = Connecting{endpoint, m_discovery.Connect(endpoint)};
	}
	const auto connection = m_discovery.Snapshot();
	if (const auto* connecting = std::get_if<Connecting>(&m_state))
	{
		if (connection.epoch != connecting->epoch)
			throw std::runtime_error("Launch discovery connection changed unexpectedly");
		if (connection.state == RuntimeConnectionState::Failed)
			throw std::runtime_error("Cannot connect to the launched runtime: " + connection.detail);
		if (connection.state != RuntimeConnectionState::Connected)
			return {};
		m_state = WaitingForSession{connecting->endpoint, connecting->epoch, {}};
	}
	auto& waiting = std::get<WaitingForSession>(m_state);
	if (connection.epoch != waiting.epoch)
		throw std::runtime_error("Launch discovery connection changed unexpectedly");
	if (connection.state != RuntimeConnectionState::Connected)
		throw std::runtime_error("Launch discovery connection closed: " + connection.detail);
	for (auto& reply : m_discovery.Poll())
	{
		if (!waiting.request || reply.ticket != *waiting.request || reply.epoch != waiting.epoch)
			continue;
		waiting.request.reset();
		if (!reply.error.empty())
			throw std::runtime_error(reply.error);
		if (auto session = UniqueSession(reply.packet.message, waiting.endpoint))
		{
			const auto epoch = waiting.epoch;
			m_state = Complete{};
			m_discovery.Disconnect(epoch);
			return session;
		}
	}
	if (!waiting.request)
	{
		control::ControlPacket request;
		request.message.opcode = static_cast<std::uint16_t>(control::OpCode::Sessions);
		if (const auto ticket = m_discovery.Submit(std::move(request)))
			waiting.request = ticket;
	}
	return {};
}
} // namespace simxr::app
