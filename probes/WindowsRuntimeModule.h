#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace simxr::probe
{
std::vector<std::string> WindowsArguments(int count, wchar_t** arguments);
std::filesystem::path RuntimeModulePath(std::string_view path);
void RequireRuntimeModule(const std::filesystem::path& path, bool loaded);
void VerifyRuntimeReload(const std::filesystem::path& path);
} // namespace simxr::probe
