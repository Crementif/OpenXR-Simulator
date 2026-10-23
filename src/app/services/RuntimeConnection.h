#pragma once

#include "control/ControlPacket.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace simxr::control
{
class LocalControlClient;
}

namespace simxr::app
{
enum class RuntimeConnectionState
{
	Disconnected,
	Connecting,
	Connected,
	Failed
};
struct RuntimeConnectionConfig
{
	// Admission includes queued, active and unpolled requests together.
	std::size_t maxPending = 32;
	std::size_t maxReplies = 32;
	std::chrono::milliseconds retrySlice{50};
	std::chrono::milliseconds exchangeDeadline{2000};
};
struct RuntimeReply
{
	std::uint64_t ticket = 0, epoch = 0;
	control::ControlPacket packet;
	// Empty for a valid protocol reply, including structured remote errors.
	std::string error;
	bool timedOut = false;
};
struct RuntimeConnectionSnapshot
{
	RuntimeConnectionState state = RuntimeConnectionState::Disconnected;
	std::string path, detail;
	std::uint64_t epoch = 0;
	std::size_t queued = 0, ready = 0;
	bool inFlight = false;
	control::ControlPacket hello;
};
// A separately owned transport worker. UI calls never perform socket I/O or
// wait for a server. Poll transfers response/FD ownership to its caller.
class RuntimeConnection
{
  public:
	explicit RuntimeConnection(const RuntimeConnectionConfig& configuration = {});
	~RuntimeConnection();
	RuntimeConnection(const RuntimeConnection&) = delete;
	RuntimeConnection& operator=(const RuntimeConnection&) = delete;
	std::uint64_t Connect(std::string path);
	void Disconnect(std::optional<std::uint64_t> expectedEpoch = {});
	// Zero means closed, not connected, or bounded admission exhausted.
	std::uint64_t Submit(control::ControlPacket request);
	std::vector<RuntimeReply> Poll();
	RuntimeConnectionSnapshot Snapshot() const;
	void Close() noexcept;

  private:
	struct Queued
	{
		std::uint64_t ticket = 0, epoch = 0;
		control::ControlPacket packet;
	};
	bool Current(std::uint64_t epoch);
	void ConnectionState(std::uint64_t epoch, RuntimeConnectionState state, std::string detail,
						 std::optional<control::ControlPacket> hello = {});
	void Reply(RuntimeReply reply);
	control::ControlPacket Exchange(control::LocalControlClient& client, control::ControlPacket packet,
									std::uint64_t epoch);
	void Run() noexcept;
	void RunChecked();
	const RuntimeConnectionConfig m_configuration;
	mutable std::mutex m_mutex;
	std::mutex m_lifecycle;
	std::condition_variable m_changed;
	std::thread m_worker;
	std::thread::id m_workerId;
	std::vector<Queued> m_requests;
	std::vector<RuntimeReply> m_replies;
	RuntimeConnectionSnapshot m_snapshot;
	std::uint64_t m_nextTicket = 1;
	bool m_requested = false, m_stopping = false, m_workerExited = false;
};
} // namespace simxr::app
