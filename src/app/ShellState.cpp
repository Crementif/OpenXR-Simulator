#include "app/ShellState.h"
#include "app/RuntimePanels.h"

namespace simxr::app
{
SessionState DeriveRunState(const RuntimePanelSnapshot& snapshot, bool launching, const std::string& error)
{
	SessionState result;
	result.endpoint = snapshot.endpoint;
	result.session = snapshot.session;
	result.generation = snapshot.generation;
	result.run = snapshot.epoch;
	result.error = error.empty() ? snapshot.error : error;
	if (!result.error.empty())
		result.state = RunState::Failed;
	else if (launching)
		result.state = RunState::Launching;
	else if (!snapshot.connected)
		result.state = snapshot.epoch ? RunState::Disconnected : RunState::NotConnected;
	else if (!snapshot.sessionAvailable)
		result.state = RunState::WaitingForSession;
	else
		result.state = snapshot.manual ? RunState::Paused : RunState::Running;
	return result;
}
const char* RunStateLabel(RunState state)
{
	switch (state)
	{
	case RunState::NotConnected:
		return "Not connected";
	case RunState::Launching:
		return "Launching";
	case RunState::WaitingForSession:
		return "Waiting for session";
	case RunState::Running:
		return "Running";
	case RunState::Paused:
		return "Paused";
	case RunState::Disconnected:
		return "Disconnected";
	case RunState::Failed:
		return "Failed";
	}
	return "Not connected";
}
} // namespace simxr::app
