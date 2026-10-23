#include "Pch.h"
#include "app/services/LaunchAttachment.h"
#include "app/RuntimeConnection.h"
#include "control/ControlSchema.h"

#include <stdexcept>

namespace simxr::app
{
static std::optional<LaunchedSession> UniqueSession(const control::Message& message, const std::string& endpoint)
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

LaunchAttachment::LaunchAttachment(RuntimeConnection& discovery, const LaunchProfile& profile)
	: m_discovery(discovery), m_deadline(std::chrono::steady_clock::now() + std::chrono::seconds{30})
{
	Initialize(profile);
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
