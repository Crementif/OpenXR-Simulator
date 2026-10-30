#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace simxr::control
{
// Each process that loads the runtime serves one control endpoint named after its process ID:
// a socket in the runtime data directory on Linux, a named pipe on Windows.
std::string RuntimeEndpoint(std::uint64_t process);

#if !defined(_WIN32)
std::filesystem::path RuntimeDataDirectory();
std::string RuntimeEndpoint(std::uint64_t process, const std::filesystem::path& directory);
#endif
} // namespace simxr::control
