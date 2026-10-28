#pragma once

#include <filesystem>
#include <string>

namespace simxr::app
{
#ifndef _WIN32
std::string LocalFileUri(const std::filesystem::path& path);
#endif
// Returns an empty string when the system accepts the request.
std::string OpenLocalPath(const std::filesystem::path& path);
} // namespace simxr::app
