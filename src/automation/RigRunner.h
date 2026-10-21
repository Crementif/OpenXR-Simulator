#pragma once

#include "control/WireProtocol.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace simxr::automation
{
enum class RigPhase
{
	Idle,
	Running,
	Paused,
	Complete,
	Cancelled,
	Failed
};
struct RigCheckpoint
{
	std::string name, detail;
	bool passed = false;
};
struct RigSnapshot
{
	RigPhase phase = RigPhase::Idle;
	std::uint64_t seed = 0, transactions = 0, captures = 0;
	std::string error;
	std::vector<RigCheckpoint> checkpoints;
	std::vector<std::string> output;
};
struct RigOptions
{
	control::ControlSessionId session;
	std::uint64_t generation = 0, seed = 0;
	std::string source, name = "rig.luau";
	std::chrono::milliseconds deadline{30000};
	std::size_t memoryLimit = 64 * 1024 * 1024;
};
// The transport belongs to this runner and is called only on its worker. It
// must bound each exchange and preserve retry identity after a timeout.
using RigExchange = std::function<control::Message(control::OpCode, std::vector<control::Field>)>;
class RigRunner
{
  public:
	explicit RigRunner(RigExchange exchange);
	~RigRunner();
	// Start and Join belong to the owning controller thread; progress controls
	// and Snapshot may be used concurrently while the worker runs.
	void Start(RigOptions options);
	void Pause();
	void Resume();
	void Step();
	void Cancel();
	void Join();
	RigSnapshot Snapshot() const;
	std::string JsonReport() const;
	std::string JunitReport() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::automation
