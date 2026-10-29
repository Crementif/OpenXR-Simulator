#pragma once

#include "simulation/CommandTypes.h"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
bool ControlsHandles(const std::string& name);
nlohmann::json ControlsDefinitions();
nlohmann::json HeadsetProfilesDocument();
std::vector<SimulationCommand> ControlsCommands(const std::string& name, const nlohmann::json& arguments,
												const nlohmann::json& configuration);
std::vector<SimulationCommand> ControlsExpectedInputs(const std::string& name, const nlohmann::json& configuration);
} // namespace simxr::app::mcp
