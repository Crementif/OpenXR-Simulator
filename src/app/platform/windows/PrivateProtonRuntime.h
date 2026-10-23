#pragma once

#include <string>

namespace simxr::app::windows
{
// Registers only in the current Wine prefix after proving its private provisioning and identity.
void RegisterPrivateProtonRuntime(const std::string& prefix, const std::string& manifest);
} // namespace simxr::app::windows
