#include "app/UiPch.h"
#include "app/mcp/McpHost.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpConfiguration.h"
#include "app/mcp/McpInstance.h"
#include "app/mcp/McpServer.h"

namespace simxr::app
{
using Json = nlohmann::json;

struct McpHost::Running
{
	std::unique_ptr<McpInstanceRegistration> registration;
	std::unique_ptr<McpApplication> application;
	std::unique_ptr<McpServer> server;
	std::string token;
	std::string httpConfiguration;
};

McpHost::McpHost(bool preference, std::optional<std::uint16_t> initialPort, std::string initialToken,
				 std::intptr_t bootstrapDescriptor, std::unique_ptr<McpInstanceRegistration> startupRegistration,
				 bool windowless)
	: m_startupRegistration(std::move(startupRegistration)), m_initialPort(initialPort),
	  m_initialToken(std::move(initialToken)), m_bootstrapDescriptor(bootstrapDescriptor), m_preference(preference),
	  m_desired(preference || initialPort.has_value()), m_sessionOverride(initialPort.has_value() && !preference),
	  m_initialPending(initialPort.has_value()), m_windowless(windowless)
{
	if (!m_desired)
		m_startupRegistration.reset();
	try
	{
		const auto app = McpOwnExecutable();
#ifdef _WIN32
		const auto connector = app.parent_path() / "simxr_mcp_connector.exe";
#else
		const auto connector = app.parent_path() / "simxr_mcp_connector";
#endif
		if (!std::filesystem::is_regular_file(connector))
			throw std::runtime_error("The MCP connector was not found beside this app.");
		m_configuration = McpConfiguration(connector, app, false);
		m_codexConfiguration = McpConfiguration(connector, app, false, McpConfigurationClient::Codex);
	}
	catch (const std::exception& error)
	{
		m_configurationError = error.what();
	}
}

McpHost::~McpHost()
{
	m_running.reset();
	if (m_stopper.joinable())
		m_stopper.join();
}

void McpHost::SetPreference(bool enabled)
{
	if (m_preference == enabled)
		return;
	m_preference = enabled;
	m_desired = enabled;
	m_sessionOverride = false;
	m_error.clear();
}

void McpHost::StopForSession()
{
	m_desired = false;
	m_sessionOverride = false;
	m_error.clear();
}

void McpHost::Retry()
{
	m_desired = true;
	m_sessionOverride = !m_preference;
	m_error.clear();
}

void McpHost::Update()
{
	if (m_stopper.joinable() && m_stopped.load())
		m_stopper.join();
	if (!m_desired && m_running)
		Retire();
	if (m_desired && !m_running && !m_stopper.joinable() && m_error.empty())
	{
		try
		{
			Start();
		}
		catch (const std::exception& error)
		{
			m_error = error.what();
		}
	}
}

void McpHost::Poll(ApplicationCatalog& tabs)
{
	Update();
	if (m_running)
		m_running->application->Poll(tabs);
}

McpUiState McpHost::State() const
{
	McpUiState state;
	state.available = true;
	state.listening = static_cast<bool>(m_running);
	state.stopping = m_stopper.joinable();
	state.startedForSession = m_sessionOverride;
	state.error = m_error;
	state.configuration = m_configuration;
	state.codexConfiguration = m_codexConfiguration;
	state.configurationError = m_configurationError;
	if (m_running)
	{
		state.discoverable = static_cast<bool>(m_running->registration);
		state.sessions = m_running->server->ClientSessions();
		state.httpConfiguration = m_running->httpConfiguration;
	}
	return state;
}

void McpHost::Start()
{
	auto running = std::make_unique<Running>();
	const bool initial = std::exchange(m_initialPending, false);
	if (m_startupRegistration)
		running->registration = std::move(m_startupRegistration);
	else if (initial && m_bootstrapDescriptor != -2)
		running->registration = std::make_unique<McpInstanceRegistration>(m_bootstrapDescriptor);
	else if (!initial)
		running->registration = std::make_unique<McpInstanceRegistration>(-1);
	const auto port = initial ? *m_initialPort : std::uint16_t{0};
	running->token = initial ? std::move(m_initialToken) : NewMcpIdentifier();
	running->application = std::make_unique<McpApplication>(m_windowless);
	running->server = std::make_unique<McpServer>(*running->application, port, running->token);
	if (running->registration)
		running->registration->Publish({running->server->Port(), running->token});
	running->httpConfiguration = Json({{"url", "http://127.0.0.1:" + std::to_string(running->server->Port()) + "/mcp"},
									   {"headers", {{"Authorization", "Bearer " + running->token}}}})
									 .dump(2);
	std::printf("MCP listening on 127.0.0.1:%u\n", running->server->Port());
	std::fflush(stdout);
	m_running = std::move(running);
}

void McpHost::Retire()
{
	m_stopped.store(false);
	m_stopper = std::thread([this, running = std::move(m_running)]() mutable {
		running.reset();
		m_stopped.store(true);
	});
}
} // namespace simxr::app
