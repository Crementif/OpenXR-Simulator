#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
bool CompatibilityHandles(const std::string& name);
nlohmann::json CompatibilityDefinitions();
nlohmann::json HeadsetProfilesDocument();
nlohmann::json CompatibilityCommands(const std::string& name, const nlohmann::json& arguments,
                                     const nlohmann::json& configuration);
nlohmann::json CompatibilityExpectedInputs(const std::string& name, const nlohmann::json& arguments,
                                          const nlohmann::json& configuration);
}
