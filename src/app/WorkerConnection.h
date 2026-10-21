#pragma once

#include "control/WireProtocol.h"

#include <memory>
#include <string>

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
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
