#include "app/UiPch.h"
#include "app/services/HeadlessApplications.h"
#include "app/mcp/McpInstance.h"
#include "app/services/LaunchAttachment.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/RuntimeConnection.h"
#include "app/services/Utf8Path.h"
#include "control/ControlSchema.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/services/LuauAutomation.h"
#include "app/services/RigController.h"
#endif

namespace simxr::app
{
struct HeadlessApplications::Entry
{
	std::uint64_t id;
	std::string name, endpoint, detail;
	LaunchProfile profile;
	ProcessLauncher launcher;
	std::unique_ptr<RuntimeConnection> discovery;
	std::unique_ptr<LaunchAttachment> attachment;
	struct PendingRestart
	{
		std::uint64_t process;
		std::unique_ptr<RuntimeConnection> discovery;
		std::unique_ptr<LaunchAttachment> attachment;
	};
	std::optional<PendingRestart> restart;
	std::optional<LaunchedSession> session;
	std::optional<std::uint64_t> sessionsTicket;
	std::chrono::steady_clock::time_point nextSessions{};
	bool retired = false;
#ifdef SIMXR_COMPANION_AUTOMATION
	RigController rigs;
#endif

	Entry(std::uint64_t identity, ProcessSupervisor& processes, const LaunchProfile& profile)
		: id(identity), name(profile.name), profile(profile), launcher(processes),
		  discovery(std::make_unique<RuntimeConnection>()),
		  attachment(std::make_unique<LaunchAttachment>(*discovery, profile))
	{
		if (!launcher.Launch(profile))
			throw std::runtime_error("Application launch was not admitted");
	}
};

HeadlessApplications::HeadlessApplications(ProcessSupervisor& processes) : m_processes(processes) {}
HeadlessApplications::~HeadlessApplications() = default;

std::uint64_t HeadlessApplications::LaunchApplication(const LaunchProfile& profile)
{
	if (m_shutdown)
		throw std::runtime_error("The simulator is shutting down");
	if (m_applications.size() >= 64)
	{
		const auto terminal = std::ranges::find_if(m_applications, [](const auto& entry)
		{
			const auto state = entry->launcher.Snapshot().state;
			return state == LaunchState::Exited || state == LaunchState::Failed;
		});
		if (terminal == m_applications.end())
			throw std::runtime_error("Application capacity reached");
		m_applications.erase(terminal);
	}
	const auto resolved = ResolveLaunchProfile(profile, McpOwnExecutable().parent_path());
	auto entry = std::make_unique<Entry>(m_nextId, m_processes, resolved);
	const auto id = m_nextId++;
	m_applications.push_back(std::move(entry));
	return id;
}

void HeadlessApplications::RequestApplicationClose(std::uint64_t id)
{
	for (const auto& entry : m_applications)
		if (entry->id == id)
		{
			if (!entry->launcher.RequestClose())
				throw std::runtime_error("No running simulator-launched process matches this application, or a close is pending");
			return;
		}
	throw std::runtime_error("Application is no longer available");
}

void HeadlessApplications::RequestApplicationRestart(std::uint64_t id, std::uint64_t expectedProcess)
{
	if (m_shutdown)
		throw std::runtime_error("The simulator is shutting down");
	for (const auto& entry : m_applications)
		if (entry->id == id)
		{
			if (entry->restart || entry->retired || entry->launcher.Snapshot().process != expectedProcess)
				throw std::runtime_error("The launched application changed or a restart is pending");
			auto discovery = std::make_unique<RuntimeConnection>();
			const auto replaced = entry->endpoint.empty() ? LaunchRuntimeEndpoint(entry->profile, expectedProcess) : entry->endpoint;
			auto attachment = std::make_unique<LaunchAttachment>(*discovery, entry->profile, replaced);
			if (!entry->launcher.Restart(expectedProcess, entry->profile))
				throw std::runtime_error("The application no longer accepts this restart request");
			entry->restart = Entry::PendingRestart{expectedProcess, std::move(discovery), std::move(attachment)};
			entry->detail.clear();
			return;
		}
	throw std::runtime_error("Application is no longer available");
}

std::vector<ApplicationSnapshot> HeadlessApplications::Applications() const
{
	std::vector<ApplicationSnapshot> result;
	result.reserve(m_applications.size());
	for (const auto& entry : m_applications)
	{
		auto launch = entry->launcher.Snapshot();
		const auto connection = entry->discovery->Snapshot();
		const bool current = !launch.restartPending &&
			(!entry->restart || entry->restart->process == launch.process);
		const bool connected = current && connection.state == RuntimeConnectionState::Connected;
		const bool running = connected && entry->session.has_value();
		const auto launchEndpoint = current && launch.state == LaunchState::Running && launch.process
			? LaunchRuntimeEndpoint(entry->profile, launch.process) : std::string{};
		const auto directory = connected && launch.process && launch.process == connection.peerProcess
			? LaunchRuntimeDataDirectory(entry->profile) : std::nullopt;
		result.push_back({entry->id, running ? entry->session->session : 0,
						  running ? entry->session->generation : 0, connected ? connection.peerProcess : 0,
						  connected ? connection.epoch : 0, std::move(launch), entry->name,
						  current ? entry->endpoint : std::string{}, entry->detail, connected, running, launchEndpoint,
						  directory ? PathText(*directory) : std::string{}});
	}
	return result;
}

bool HeadlessApplications::RequestShutdown()
{
	m_shutdown = true;
	return true;
}
#ifdef SIMXR_COMPANION_AUTOMATION
std::uint64_t HeadlessApplications::StartLuau(std::uint64_t id, LuauRunRequest request,
											 const std::shared_ptr<AutomationOwner>& owner)
{
	for (const auto& entry : m_applications)
		if (entry->id == id)
		{
			const auto applications = Applications();
			const auto application = std::ranges::find_if(applications, [id](const auto& item) { return item.id == id; });
			if (application == applications.end())
				break;
			return StartLuauRun(entry->rigs, *application, std::move(request), owner);
		}
	throw std::runtime_error("Application is no longer available");
}
LuauRunStatus HeadlessApplications::LuauStatus(std::uint64_t id, std::uint64_t run,
											const std::shared_ptr<AutomationOwner>& owner) const
{
	for (const auto& entry : m_applications)
		if (entry->id == id)
			return ReadLuauRun(entry->rigs, run, owner);
	throw std::runtime_error("Application is no longer available");
}
void HeadlessApplications::ControlLuau(std::uint64_t id, std::uint64_t run, LuauControl action,
										const std::shared_ptr<AutomationOwner>& owner)
{
	for (const auto& entry : m_applications)
		if (entry->id == id)
		{
			ControlLuauRun(entry->rigs, run, action, owner);
			return;
		}
	throw std::runtime_error("Application is no longer available");
}
#endif
bool HeadlessApplications::ShutdownRequested() const noexcept
{
	return m_shutdown;
}

void HeadlessApplications::Poll()
{
	for (const auto& entry : m_applications)
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		entry->rigs.CancelDisconnected();
#endif
		const auto launch = entry->launcher.Snapshot();
		if (entry->restart && !launch.restartPending)
		{
			if (launch.state == LaunchState::Starting ||
				(launch.state == LaunchState::Running && launch.process != entry->restart->process))
			{
				entry->attachment.reset();
				entry->discovery->Close();
				entry->discovery = std::move(entry->restart->discovery);
				entry->attachment = std::move(entry->restart->attachment);
				entry->endpoint.clear();
				entry->session.reset();
				entry->sessionsTicket.reset();
				entry->nextSessions = {};
			}
			else
				entry->detail = launch.detail;
			entry->restart.reset();
		}
		if (launch.state == LaunchState::Exited || launch.state == LaunchState::Failed)
		{
			if (!entry->retired)
			{
				entry->attachment.reset();
				entry->discovery->Close();
				entry->session.reset();
				entry->retired = true;
			}
			continue;
		}
		if (entry->attachment)
		{
			try
			{
				entry->attachment->Observe(launch);
				if (auto session = entry->attachment->Poll())
				{
					entry->attachment.reset();
					entry->endpoint = session->endpoint;
					entry->discovery->Connect(entry->endpoint, session->process);
				}
			}
			catch (const std::exception& error)
			{
				entry->detail = error.what();
				entry->attachment.reset();
			}
		}
		if (entry->endpoint.empty())
			continue;
		const auto connection = entry->discovery->Snapshot();
		if (connection.state != RuntimeConnectionState::Connected)
		{
			entry->session.reset();
			entry->sessionsTicket.reset();
			continue;
		}
		for (auto& reply : entry->discovery->Poll())
		{
			if (!entry->sessionsTicket || reply.ticket != *entry->sessionsTicket || reply.epoch != connection.epoch)
				continue;
			entry->sessionsTicket.reset();
			entry->session.reset();
			const auto* error = reply.packet.message.Find(control::tag::Error);
			const auto* catalog = reply.packet.message.Find(control::tag::Sessions);
			if (!reply.error.empty() || !error || error->AsU32() != static_cast<std::uint32_t>(control::ErrorCode::Ok) || !catalog)
				continue;
			const auto decoded = control::DecodeFields(catalog->value);
			if (decoded.error != control::DecodeError::None)
				continue;
			bool ambiguous = false;
			for (const auto& item : decoded.fields)
			{
				const auto fields = control::DecodeFields(item.value);
				if (fields.error != control::DecodeError::None)
				{
					ambiguous = true;
					break;
				}
				control::Message session;
				session.fields = fields.fields;
				const auto* running = session.Find(control::tag::Running);
				const auto* id = session.Find(control::tag::Session);
				const auto* generation = session.Find(control::tag::Generation);
				if (!running || !running->AsBool())
				{
					ambiguous = true;
					break;
				}
				if (!*running->AsBool())
					continue;
				if (!id || !generation || !id->AsU64().value_or(0) || !generation->AsU64().value_or(0) || entry->session)
				{
					ambiguous = true;
					break;
				}
				entry->session = LaunchedSession{entry->endpoint, *id->AsU64(), *generation->AsU64()};
			}
			if (ambiguous)
				entry->session.reset();
		}
		const auto now = std::chrono::steady_clock::now();
		if (!entry->sessionsTicket && now >= entry->nextSessions)
		{
			control::ControlPacket request;
			request.message.opcode = static_cast<std::uint16_t>(control::OpCode::Sessions);
			if (const auto ticket = entry->discovery->Submit(std::move(request)))
				entry->sessionsTicket = ticket;
			entry->nextSessions = now + std::chrono::milliseconds(250);
		}
	}
	PollMonitors();
}
} // namespace simxr::app
