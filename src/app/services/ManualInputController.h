#pragma once

#include "app/services/SimulationEdits.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace simxr::app
{
class WorkerConnection;
struct ManualInputTarget
{
	std::string endpoint;
	std::uint64_t session = 0, generation = 0;
	bool operator==(const ManualInputTarget&) const = default;
};
struct ManualInputSnapshot
{
	control::Message state, result;
	std::optional<CommandSource> source;
	std::uint64_t observation = 0, appliedPoses = 0;
	std::string error;
	bool posePending = false, editPending = false;
};
// One persistent manual source. Pose delivery and acknowledgement never wait for a UI render.
class ManualInputController
{
  public:
	ManualInputController();
	~ManualInputController();
	ManualInputController(const ManualInputController&) = delete;
	ManualInputController& operator=(const ManualInputController&) = delete;
	void SetTarget(ManualInputTarget target);
	bool Submit(SimulationEdit edit, bool continuous = false);
	void DiscardUnsentPose();
	void Refresh();
	ManualInputSnapshot Snapshot() const;

  private:
	void Run() noexcept;
	void RunChecked();
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	ManualInputTarget m_target;
	ManualInputSnapshot m_snapshot;
	std::deque<SimulationEdit> m_edits;
	std::optional<SimulationEdit> m_pose;
	std::shared_ptr<WorkerConnection> m_current;
	std::uint64_t m_revision = 0;
	bool m_stopping = false, m_refresh = false;
	std::string m_workerFailure = "Manual input worker stopped unexpectedly; restart the companion";
	std::thread m_worker;
};
} // namespace simxr::app
