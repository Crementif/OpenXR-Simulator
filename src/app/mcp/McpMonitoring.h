#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace simxr::app
{
class ApplicationCatalog;
nlohmann::json MonitorDefinitions();
bool HandlesMonitor(const std::string& name);
nlohmann::json CallMonitor(ApplicationCatalog& applications, std::uint64_t application, const std::string& name,
						   const nlohmann::json& arguments);
}
