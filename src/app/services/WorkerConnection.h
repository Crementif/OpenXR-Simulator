#pragma once

#include "control/WireProtocol.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace simxr::control
{
class LocalControlClient;
}

namespace simxr::app
{
// Owns a synchronous protocol connection used by one worker. Stop is the sole
// concurrent operation; it interrupts retry slices without changing request IDs.
class WorkerConnection
{
  public:
	WorkerConnection(std::string endpoint, std::string name, std::uint64_t required);
	~WorkerConnection();
	WorkerConnection(const WorkerConnection&) = delete;
	WorkerConnection& operator=(const WorkerConnection&) = delete;
	control::Message Exchange(control::OpCode opcode, std::vector<control::Field> fields);
	void Stop() noexcept;
	bool Failed() const noexcept;

  private:
	control::Message Request(control::OpCode code, std::vector<control::Field> fields);
	std::string m_endpoint, m_name;
	std::uint64_t m_required;
	std::atomic<bool> m_stopping{false};
	bool m_failed = false;
#if defined(__linux__) || defined(_WIN32)
	std::unique_ptr<control::LocalControlClient> m_client;
#endif
};
} // namespace simxr::app
