#pragma once

#include "control/InspectionDocument.h"
#include "simulation/CommandTypes.h"

namespace simxr::control
{
// Commands use explicit v1 kind values and bounded nested TLVs, never native
// object layouts. Source identity is assigned by the service, not decoded here.
Field EncodeSimulationCommand(const SimulationCommand& command, std::uint16_t index = 1);
SimulationCommand DecodeSimulationCommand(const Field& field);
CommandTransaction DecodeSimulationTransaction(const Message& message, std::uint64_t connection);
InspectionValue SimulationOutcome(const CommandOutcome& outcome, bool includeCommands = true);
InspectionValue SimulationState(const CommandSchedulerSnapshot& state, bool includeCommands = false);
InspectionValue SimulationRecording(const CommandRecording& recording);
const char* SimulationResultName(CommandResult result);
} // namespace simxr::control
