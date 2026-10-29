#pragma once

#include "automation/RigProgress.h"
#include "automation/RigRuntime.h"
#include "automation/RigTypes.h"

#include <thread>

namespace simxr::automation
{
void ValidateRigOptions(const RigOptions& options);

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
	RigPhase Phase() const;
	RigSnapshot Snapshot() const;
	std::string JsonReport() const;
	std::string JunitReport() const;

  private:
	void Run();
	RigOptions m_options;
	RigProgress m_progress;
	RigRuntime m_runtime;
	std::thread m_worker;
};
} // namespace simxr::automation
