#pragma once

#include "simulation/CommandTypes.h"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
bool CompatibilityHandles(const std::string& name);
nlohmann::json CompatibilityDefinitions();
nlohmann::json HeadsetProfilesDocument();
std::vector<SimulationCommand> CompatibilityCommands(const std::string& name, const nlohmann::json& arguments,
                                     const nlohmann::json& configuration);
std::vector<SimulationCommand> CompatibilityExpectedInputs(const std::string& name, const nlohmann::json& arguments,
                                          const nlohmann::json& configuration);
}
