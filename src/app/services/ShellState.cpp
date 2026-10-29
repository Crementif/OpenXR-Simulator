#include "app/services/ShellState.h"
#include "app/UiPch.h"
#include "app/services/SessionView.h"

namespace simxr::app
{
SessionState DeriveRunState(const SessionView& snapshot, bool launching, const std::string& launchError)
{
	SessionState result;
	result.endpoint = snapshot.endpoint;
	result.session = snapshot.session;
	result.generation = snapshot.generation;
	result.run = snapshot.generation;
	result.fps = snapshot.fps;
	result.error = launchError.empty() ? snapshot.error : launchError;
	if (!result.error.empty())
		result.state = RunState::Failed;
	else if (launching)
		result.state = RunState::Launching;
	else if (!snapshot.connected)
		result.state = snapshot.epoch ? RunState::Disconnected : RunState::NotConnected;
	else if (!snapshot.sessionAvailable || !snapshot.sessionRunning)
		result.state = RunState::WaitingForSession;
	else
		result.state = snapshot.manual ? RunState::Paused : RunState::Running;
	return result;
}

void LeaveHistory(ShellState& shell, bool range)
{
	if (std::holds_alternative<FrameSel>(shell.selection) ||
		(range && std::holds_alternative<RangeSel>(shell.selection)))
		shell.selection = shell.device;
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
