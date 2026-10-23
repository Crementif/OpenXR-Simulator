#pragma once

#include <string>

namespace simxr::app
{
std::wstring LocalPipePath(const std::string& endpoint);
bool RuntimeEndpointExists(const std::string& endpoint);
} // namespace simxr::app
