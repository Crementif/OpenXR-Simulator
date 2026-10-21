#pragma once

#include "control/ControlPacket.h"
#include "simulation/CommandTypes.h"

namespace simxr::app
{
struct ManualTransaction
{
	// An absent selector uses the next frame after the observed command publication.
	std::optional<FrameId> frame;
	CommandLatePolicy latePolicy = CommandLatePolicy::RescheduleNext;
	std::vector<SimulationCommand> commands;
};
struct ReleaseManualInputs
{
};
using SimulationEdit = std::variant<ManualTransaction, ReleaseManualInputs>;

PoseSample ObservedDevicePose(const control::Message& state, ControlChannel channel);
PoseSample PilotPose(PoseSample pose, XrVector3f movement, float yaw, float pitch, float seconds, float speed = 1);

control::ControlPacket EncodeSimulationEdit(const SimulationEdit& edit, std::uint64_t session, std::uint64_t generation,
											std::uint64_t transaction, const control::Message& observedState);
} // namespace simxr::app
