#include "app/RuntimeConnection.h"
#include "control/ControlSchema.h"
#if defined(__linux__) || defined(_WIN32)
#include "control/LocalControlClient.h"
#endif

#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace simxr::app
{
namespace
{
using Clock = std::chrono::steady_clock;
}
struct RuntimeConnection::State
{
	struct Queued
	{
		std::uint64_t ticket = 0, epoch = 0;
		control::ControlPacket packet;
	};
	const RuntimeConnectionConfig configuration;
	mutable std::mutex mutex;
	std::mutex lifecycle;
	std::condition_variable changed;
	std::thread worker;
	std::thread::id workerId;
	std::vector<Queued> requests;
	std::vector<RuntimeReply> replies;
	RuntimeConnectionSnapshot snapshot;
	std::uint64_t nextTicket = 1;
	bool requested = false, stopping = false, workerExited = false;
	explicit State(const RuntimeConnectionConfig& configuration) : configuration(configuration)
	{
		if (!configuration.maxPending || configuration.maxPending > 64 || !configuration.maxReplies ||
			configuration.maxReplies > 64 || configuration.retrySlice.count() <= 0 ||
			configuration.retrySlice > std::chrono::milliseconds{100} ||
			configuration.exchangeDeadline < configuration.retrySlice ||
			configuration.exchangeDeadline > std::chrono::seconds{30})
			throw std::invalid_argument("Invalid asynchronous runtime connection bounds");
		requests.reserve(configuration.maxPending);
		replies.reserve(configuration.maxReplies);
	}
	bool Current(std::uint64_t epoch)
	{
		std::lock_guard lock(mutex);
		return !stopping && requested && snapshot.epoch == epoch;
	}
	void ConnectionState(std::uint64_t epoch, RuntimeConnectionState state, std::string detail,
						 std::optional<control::ControlPacket> hello = {})
	{
		control::ControlPacket previous;
		std::lock_guard lock(mutex);
		if (stopping || snapshot.epoch != epoch || !requested)
			return;
		snapshot.state = state;
		snapshot.detail = std::move(detail);
		if (hello)
			previous = std::exchange(snapshot.hello, std::move(*hello));
	}
	void Reply(RuntimeReply reply)
	{
		std::lock_guard lock(mutex);
		if (stopping || snapshot.epoch != reply.epoch || !requested)
			return;
		snapshot.inFlight = false;
		replies.push_back(std::move(reply)); // Capacity was reserved before the request began.
		changed.notify_all();
	}
#if defined(__linux__) || defined(_WIN32)
	control::ControlPacket Exchange(control::LocalControlClient& client, control::ControlPacket packet,
									std::uint64_t epoch)
	{
		const auto deadline = Clock::now() + configuration.exchangeDeadline;
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
	void Run() noexcept
	{
		{
			std::lock_guard lock(mutex);
			workerId = std::this_thread::get_id();
		}
		try
		{
			RunChecked();
		}
		catch (...)
		{
			std::lock_guard lock(mutex);
			if (!stopping)
			{
				snapshot.state = RuntimeConnectionState::Failed;
				snapshot.inFlight = false;
				snapshot.detail.clear();
			}
		}
		std::lock_guard lock(mutex);
		workerExited = true;
	}
	void RunChecked()
	{
		std::uint64_t activeEpoch = 0;
#if defined(__linux__) || defined(_WIN32)
		std::unique_ptr<control::LocalControlClient> client;
#endif
		for (;;)
		{
			Queued request;
			std::string path;
			bool connect = false, newEpoch = false, failed = false;
			{
				std::unique_lock lock(mutex);
				changed.wait(lock,
							 [&]
							 {
								 return stopping || snapshot.epoch != activeEpoch ||
										((snapshot.state == RuntimeConnectionState::Connected ||
										  snapshot.state == RuntimeConnectionState::Failed) &&
										 !requests.empty() && replies.size() < configuration.maxReplies);
							 });
				if (stopping)
					return;
				if (snapshot.epoch != activeEpoch)
				{
					activeEpoch = snapshot.epoch;
					newEpoch = true;
					connect = requested;
					path = snapshot.path;
				}
				else
				{
					request = std::move(requests.front());
					requests.erase(requests.begin());
					snapshot.inFlight = true;
					failed = snapshot.state == RuntimeConnectionState::Failed;
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
					client = std::make_unique<control::LocalControlClient>(std::move(path), configuration.retrySlice);
					control::ControlPacket hello;
					hello.message.opcode = static_cast<std::uint16_t>(control::OpCode::Hello);
					hello.message.fields = {control::Field::Text(control::tag::ClientName, "simxr companion")};
					auto response = Exchange(*client, std::move(hello), activeEpoch);
					const auto error = response.message.Find(control::tag::Error);
					const bool accepted = error && error->AsU32() == static_cast<std::uint32_t>(control::ErrorCode::Ok);
					ConnectionState(activeEpoch,
									accepted ? RuntimeConnectionState::Connected : RuntimeConnectionState::Failed,
									accepted ? "" : "Runtime rejected control protocol Hello", std::move(response));
					if (!accepted)
						client.reset();
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
				try
				{
					reply.packet = Exchange(*client, std::move(request.packet), activeEpoch);
				}
				catch (const control::ControlExchangeTimeout& error)
				{
					reply.error = error.what();
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
};
RuntimeConnection::RuntimeConnection(const RuntimeConnectionConfig& configuration)
	: m_state(std::make_unique<State>(configuration))
{
	m_state->worker = std::thread([state = m_state.get()] { state->Run(); });
}
RuntimeConnection::~RuntimeConnection()
{
	Close();
}
std::uint64_t RuntimeConnection::Connect(std::string path)
{
	std::vector<State::Queued> requests;
	std::vector<RuntimeReply> replies;
	control::ControlPacket hello;
	requests.reserve(m_state->configuration.maxPending);
	replies.reserve(m_state->configuration.maxReplies);
	std::lock_guard lock(m_state->mutex);
	if (m_state->stopping || m_state->workerExited ||
		m_state->snapshot.epoch == std::numeric_limits<std::uint64_t>::max())
		return 0;
	m_state->requests.swap(requests);
	m_state->replies.swap(replies);
	hello = std::move(m_state->snapshot.hello);
	m_state->snapshot.path = std::move(path);
	m_state->snapshot.detail.clear();
	m_state->snapshot.state = RuntimeConnectionState::Connecting;
	m_state->snapshot.inFlight = false;
	m_state->requested = true;
	++m_state->snapshot.epoch;
	m_state->changed.notify_all();
	return m_state->snapshot.epoch;
}
void RuntimeConnection::Disconnect(std::optional<std::uint64_t> expectedEpoch)
{
	std::vector<State::Queued> requests;
	std::vector<RuntimeReply> replies;
	control::ControlPacket hello;
	std::lock_guard lock(m_state->mutex);
	if (m_state->stopping || (expectedEpoch && *expectedEpoch != m_state->snapshot.epoch))
		return;
	m_state->requests.swap(requests);
	m_state->replies.swap(replies);
	hello = std::move(m_state->snapshot.hello);
	m_state->requested = false;
	m_state->snapshot.state = RuntimeConnectionState::Disconnected;
	m_state->snapshot.inFlight = false;
	m_state->snapshot.detail.clear();
	if (m_state->snapshot.epoch != std::numeric_limits<std::uint64_t>::max())
		++m_state->snapshot.epoch;
	m_state->changed.notify_all();
}
std::uint64_t RuntimeConnection::Submit(control::ControlPacket request)
{
	std::lock_guard lock(m_state->mutex);
	if (m_state->stopping || m_state->workerExited || m_state->snapshot.state != RuntimeConnectionState::Connected ||
		!m_state->nextTicket ||
		m_state->requests.size() + m_state->replies.size() + (m_state->snapshot.inFlight ? 1 : 0) >=
			m_state->configuration.maxPending)
		return 0;
	const auto ticket = m_state->nextTicket;
	m_state->requests.push_back({ticket, m_state->snapshot.epoch, std::move(request)});
	++m_state->nextTicket;
	m_state->changed.notify_all();
	return ticket;
}
std::vector<RuntimeReply> RuntimeConnection::Poll()
{
	std::vector<RuntimeReply> result;
	{
		std::lock_guard lock(m_state->mutex);
		if (m_state->replies.empty())
			return result;
	}
	result.reserve(m_state->configuration.maxReplies);
	std::lock_guard lock(m_state->mutex);
	result.swap(m_state->replies);
	m_state->changed.notify_all();
	return result;
}
RuntimeConnectionSnapshot RuntimeConnection::Snapshot() const
{
	std::lock_guard lock(m_state->mutex);
	auto result = m_state->snapshot;
	result.queued = m_state->requests.size();
	result.ready = m_state->replies.size();
	return result;
}
void RuntimeConnection::Close() noexcept
{
	std::vector<State::Queued> requests;
	std::vector<RuntimeReply> replies;
	control::ControlPacket hello;
	bool self;
	{
		std::lock_guard lock(m_state->mutex);
		self = !m_state->workerExited && m_state->workerId == std::this_thread::get_id();
	}
	// A packet callback on the worker cannot join itself, and must not wait on
	// the lifecycle mutex while an external owner is joining that worker.
	std::unique_lock lifecycle(m_state->lifecycle, std::defer_lock);
	if (!self)
		lifecycle.lock();
	{
		std::lock_guard lock(m_state->mutex);
		m_state->stopping = true;
		m_state->requested = false;
		m_state->snapshot.state = RuntimeConnectionState::Disconnected;
		m_state->snapshot.inFlight = false;
		m_state->requests.swap(requests);
		m_state->replies.swap(replies);
		hello = std::move(m_state->snapshot.hello);
	}
	m_state->changed.notify_all();
	if (!self && m_state->worker.joinable())
		m_state->worker.join();
	if (lifecycle.owns_lock())
		lifecycle.unlock();
	// Last packet/FD readers may reenter Close or Snapshot after both locks end.
}
} // namespace simxr::app
