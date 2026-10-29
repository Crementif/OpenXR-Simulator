#include "app/services/RuntimeConnection.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#if defined(__linux__) || defined(_WIN32)
#include "app/platform/RunningApps.h"
#include "control/LocalControlClient.h"
#endif

namespace simxr::app
{
using Clock = std::chrono::steady_clock;

RuntimeConnection::RuntimeConnection(const RuntimeConnectionConfig& configuration) : m_configuration(configuration)
{
	if (!configuration.maxPending || configuration.maxPending > 64 || !configuration.maxReplies ||
		configuration.maxReplies > 64 || configuration.retrySlice.count() <= 0 ||
		configuration.retrySlice > std::chrono::milliseconds{100} ||
		configuration.exchangeDeadline < configuration.retrySlice ||
		configuration.exchangeDeadline > std::chrono::seconds{30})
		throw std::invalid_argument("Invalid asynchronous runtime connection bounds");
	m_requests.reserve(configuration.maxPending);
	m_replies.reserve(configuration.maxReplies);
	m_worker = std::thread([this] { Run(); });
}

bool RuntimeConnection::Current(std::uint64_t epoch)
{
	std::lock_guard lock(m_mutex);
	return !m_stopping && m_requested && m_snapshot.epoch == epoch;
}

void RuntimeConnection::ConnectionState(std::uint64_t epoch, RuntimeConnectionState state, std::string detail,
										std::optional<control::ControlPacket> hello, std::string appName,
										std::uint64_t peerProcess)
{
	control::ControlPacket previous;
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_snapshot.epoch != epoch || !m_requested)
		return;
	m_snapshot.state = state;
	m_snapshot.detail = std::move(detail);
	m_snapshot.appName = std::move(appName);
	m_snapshot.peerProcess = state == RuntimeConnectionState::Connected ? peerProcess : 0;
	if (hello)
		previous = std::exchange(m_snapshot.hello, std::move(*hello));
}

void RuntimeConnection::Reply(RuntimeReply reply)
{
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_snapshot.epoch != reply.epoch || !m_requested)
		return;
	m_snapshot.inFlight = false;
	m_replies.push_back(std::move(reply)); // Capacity was reserved before the request began.
	m_changed.notify_all();
}
#if defined(__linux__) || defined(_WIN32)
static std::string TimeoutDetail(const control::ControlExchangeTimeout& error, std::uint16_t opcode,
								 std::chrono::milliseconds deadline)
{
	return "Control request " + std::to_string(error.Request().value) + " (opcode " + std::to_string(opcode) +
		   ") exceeded the " + std::to_string(deadline.count()) + " ms exchange deadline. Connection closed";
}

control::ControlPacket RuntimeConnection::Exchange(control::LocalControlClient& client, control::ControlPacket packet,
												   std::uint64_t epoch)
{
	const auto deadline = Clock::now() + m_configuration.exchangeDeadline;
	bool retry = false;
	for (;;)
	{
		if (!Current(epoch))
			throw std::runtime_error("Runtime connection changed");
		try
		{
			return retry ? client.Retry() : client.Request(std::move(packet));
		}
		catch (const control::ControlExchangeTimeout&)
		{
			if (Clock::now() >= deadline)
				throw;
			retry = true;
		}
	}
}
#endif
void RuntimeConnection::Run() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		m_workerId = std::this_thread::get_id();
	}
	try
	{
		RunChecked();
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		if (!m_stopping)
		{
			m_snapshot.state = RuntimeConnectionState::Failed;
			m_snapshot.inFlight = false;
			m_snapshot.detail.clear();
		}
	}
	std::lock_guard lock(m_mutex);
	m_workerExited = true;
}

void RuntimeConnection::RunChecked()
{
	std::uint64_t activeEpoch = 0;
#if defined(__linux__) || defined(_WIN32)
	std::unique_ptr<control::LocalControlClient> client;
#endif
	for (;;)
	{
		Queued request;
		std::string path;
		std::optional<std::uint64_t> expectedProcess;
		bool connect = false, newEpoch = false, failed = false;
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock, [&] {
				return m_stopping || m_snapshot.epoch != activeEpoch ||
					   ((m_snapshot.state == RuntimeConnectionState::Connected ||
						 m_snapshot.state == RuntimeConnectionState::Failed) &&
						!m_requests.empty() && m_replies.size() < m_configuration.maxReplies);
			});
			if (m_stopping)
				return;
			if (m_snapshot.epoch != activeEpoch)
			{
				activeEpoch = m_snapshot.epoch;
				newEpoch = true;
				connect = m_requested;
				path = m_snapshot.path;
				expectedProcess = m_expectedProcess;
			}
			else
			{
				request = std::move(m_requests.front());
				m_requests.erase(m_requests.begin());
				m_snapshot.inFlight = true;
				failed = m_snapshot.state == RuntimeConnectionState::Failed;
			}
		}
		if (newEpoch)
		{
#if defined(__linux__) || defined(_WIN32)
			client.reset();
			if (!connect)
				continue;
			try
			{
				const auto connectDeadline = Clock::now() + m_configuration.exchangeDeadline;
				for (;;)
				{
					if (!Current(activeEpoch))
						throw std::runtime_error("Runtime connection changed");
					const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(connectDeadline - Clock::now());
					if (remaining <= std::chrono::milliseconds::zero())
						throw std::runtime_error("Connect local control pipe timed out");
					try
					{
						client = std::make_unique<control::LocalControlClient>(
							path, std::min(m_configuration.retrySlice, remaining));
						break;
					}
					catch (const control::ControlConnectTimeout&)
					{
						if (Clock::now() >= connectDeadline)
							throw;
					}
				}
				if (expectedProcess && client->PeerProcess() != *expectedProcess)
					throw std::runtime_error("Runtime endpoint belongs to a different process");
				control::ControlPacket hello;
				hello.message.opcode = static_cast<std::uint16_t>(control::OpCode::Hello);
				hello.message.fields = {control::Field::Text(control::tag::ClientName, "simxr companion")};
				auto response = Exchange(*client, std::move(hello), activeEpoch);
				const auto error = response.message.Find(control::tag::Error);
				const bool accepted = error && error->AsU32() == static_cast<std::uint32_t>(control::ErrorCode::Ok);
				std::string appName;
				if (accepted)
				{
					try
					{
						if (auto app = ProbeRunningApp(path, m_configuration.retrySlice))
							appName = std::move(app->name);
					}
					catch (const std::exception&)
					{
						// Process details are optional for an otherwise usable endpoint.
					}
				}
				ConnectionState(activeEpoch,
								accepted ? RuntimeConnectionState::Connected : RuntimeConnectionState::Failed,
								accepted ? "" : "Runtime rejected control protocol Hello", std::move(response),
								std::move(appName), accepted ? client->PeerProcess() : 0);
				if (!accepted)
					client.reset();
			}
			catch (const control::ControlExchangeTimeout& error)
			{
				client.reset();
				ConnectionState(activeEpoch, RuntimeConnectionState::Failed,
								TimeoutDetail(error, static_cast<std::uint16_t>(control::OpCode::Hello),
											  m_configuration.exchangeDeadline));
			}
			catch (const std::exception& error)
			{
				client.reset();
				ConnectionState(activeEpoch, RuntimeConnectionState::Failed, error.what());
			}
#else
			if (connect)
				ConnectionState(activeEpoch, RuntimeConnectionState::Failed,
								"Local runtime transport is unavailable on this platform");
#endif
			continue;
		}
		RuntimeReply reply;
		reply.ticket = request.ticket;
		reply.epoch = request.epoch;
		if (failed)
			reply.error = "Runtime connection failed before this queued request could run";
		else
		{
#if defined(__linux__) || defined(_WIN32)
			const auto opcode = request.packet.message.opcode;
			try
			{
				reply.packet = Exchange(*client, std::move(request.packet), activeEpoch);
			}
			catch (const control::ControlExchangeTimeout& error)
			{
				reply.error = TimeoutDetail(error, opcode, m_configuration.exchangeDeadline);
				reply.timedOut = true;
				client.reset();
				ConnectionState(activeEpoch, RuntimeConnectionState::Failed, reply.error);
			}
			catch (const std::exception& error)
			{
				reply.error = error.what();
				client.reset();
				ConnectionState(activeEpoch, RuntimeConnectionState::Failed, reply.error);
			}
#else
			reply.error = "Local runtime transport is unavailable on this platform";
#endif
		}
		Reply(std::move(reply));
	}
}

RuntimeConnection::~RuntimeConnection()
{
	Close();
}

std::uint64_t RuntimeConnection::Connect(std::string path, std::optional<std::uint64_t> expectedProcess)
{
	std::vector<Queued> requests;
	std::vector<RuntimeReply> replies;
	control::ControlPacket hello;
	requests.reserve(m_configuration.maxPending);
	replies.reserve(m_configuration.maxReplies);
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_workerExited || m_snapshot.epoch == std::numeric_limits<std::uint64_t>::max())
		return 0;
	m_requests.swap(requests);
	m_replies.swap(replies);
	hello = std::move(m_snapshot.hello);
	m_snapshot.path = std::move(path);
	m_expectedProcess = expectedProcess;
	m_snapshot.detail.clear();
	m_snapshot.appName.clear();
	m_snapshot.peerProcess = 0;
	m_snapshot.state = RuntimeConnectionState::Connecting;
	m_snapshot.inFlight = false;
	m_requested = true;
	++m_snapshot.epoch;
	m_changed.notify_all();
	return m_snapshot.epoch;
}

void RuntimeConnection::Disconnect(std::optional<std::uint64_t> expectedEpoch)
{
	std::vector<Queued> requests;
	std::vector<RuntimeReply> replies;
	control::ControlPacket hello;
	std::lock_guard lock(m_mutex);
	if (m_stopping || (expectedEpoch && *expectedEpoch != m_snapshot.epoch))
		return;
	m_requests.swap(requests);
	m_replies.swap(replies);
	hello = std::move(m_snapshot.hello);
	m_requested = false;
	m_snapshot.peerProcess = 0;
	m_snapshot.state = RuntimeConnectionState::Disconnected;
	m_snapshot.inFlight = false;
	m_snapshot.detail.clear();
	m_snapshot.appName.clear();
	if (m_snapshot.epoch != std::numeric_limits<std::uint64_t>::max())
		++m_snapshot.epoch;
	m_changed.notify_all();
}

std::uint64_t RuntimeConnection::Submit(control::ControlPacket request)
{
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_workerExited || m_snapshot.state != RuntimeConnectionState::Connected || !m_nextTicket ||
		m_requests.size() + m_replies.size() + (m_snapshot.inFlight ? 1 : 0) >= m_configuration.maxPending)
		return 0;
	const auto ticket = m_nextTicket;
	m_requests.push_back({ticket, m_snapshot.epoch, std::move(request)});
	++m_nextTicket;
	m_changed.notify_all();
	return ticket;
}

std::vector<RuntimeReply> RuntimeConnection::Poll()
{
	std::vector<RuntimeReply> result;
	{
		std::lock_guard lock(m_mutex);
		if (m_replies.empty())
			return result;
	}
	result.reserve(m_configuration.maxReplies);
	std::lock_guard lock(m_mutex);
	result.swap(m_replies);
	m_changed.notify_all();
	return result;
}

RuntimeConnectionSnapshot RuntimeConnection::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	auto result = m_snapshot;
	result.queued = m_requests.size();
	result.ready = m_replies.size();
	return result;
}

void RuntimeConnection::Close() noexcept
{
	std::vector<Queued> requests;
	std::vector<RuntimeReply> replies;
	control::ControlPacket hello;
	bool self;
	{
		std::lock_guard lock(m_mutex);
		self = !m_workerExited && m_workerId == std::this_thread::get_id();
	}
	// A packet callback on the worker cannot join itself, and must not wait on
	// the lifecycle mutex while an external owner is joining that worker.
	std::unique_lock lifecycle(m_lifecycle, std::defer_lock);
	if (!self)
		lifecycle.lock();
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
		m_requested = false;
		m_snapshot.state = RuntimeConnectionState::Disconnected;
		m_snapshot.inFlight = false;
		m_snapshot.appName.clear();
		m_requests.swap(requests);
		m_replies.swap(replies);
		hello = std::move(m_snapshot.hello);
	}
	m_changed.notify_all();
	if (!self && m_worker.joinable())
		m_worker.join();
	if (lifecycle.owns_lock())
		lifecycle.unlock();
	// Last packet/FD readers may reenter Close or Snapshot after both locks end.
}
} // namespace simxr::app
