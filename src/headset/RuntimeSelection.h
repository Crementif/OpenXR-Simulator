#pragma once

#include <filesystem>
#include <string_view>

namespace simxr::headset
{
// An empty manifest selects the loader's system default, ignoring inherited overrides.
std::filesystem::path ValidateRuntimeManifest(const std::filesystem::path& manifest);
bool IsSimulatorRuntime(std::string_view name);
// Called once in the dedicated host, before creating threads or calling the loader.
void PrepareHostEnvironment(const std::filesystem::path& manifest);
} // namespace simxr::headset
