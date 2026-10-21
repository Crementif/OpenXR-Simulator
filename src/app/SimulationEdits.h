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

control::ControlPacket EncodeSimulationEdit(const SimulationEdit& edit, std::uint64_t session, std::uint64_t generation,
											std::uint64_t transaction, const control::Message& observedState);
} // namespace simxr::app
