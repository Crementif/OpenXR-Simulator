#pragma once

#include "control/ControlPacket.h"
#include "simulation/CommandTypes.h"

#include <string>
#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
struct SimulationRequest
{
	control::OpCode opcode;
	std::vector<control::Field> fields;
};
bool SimulationHandles(const std::string& name);
nlohmann::json SimulationDefinitions();
SimulationRequest PrepareSimulation(const std::string& name, const nlohmann::json& arguments,
									std::uint64_t session, std::uint64_t generation);
nlohmann::json SimulationReply(const control::Message& message);
SimulationCommand SimulationCommandFromJson(const nlohmann::json& command);
nlohmann::json InspectionJson(std::span<const std::byte> document);
} // namespace simxr::app::mcp
