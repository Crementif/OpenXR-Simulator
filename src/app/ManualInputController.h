#pragma once

#include "app/SimulationEdits.h"
#include <memory>
#include <string>

namespace simxr::app
{
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
	void SetTarget(ManualInputTarget target);
	bool Submit(SimulationEdit edit, bool continuous = false);
	void DiscardUnsentPose();
	void Refresh();
	ManualInputSnapshot Snapshot() const;

  private:
	class State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
