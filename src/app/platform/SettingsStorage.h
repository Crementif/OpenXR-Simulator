#pragma once

#include <filesystem>

namespace simxr::app
{
std::filesystem::path CreateSettingsTemporaryFile(const std::filesystem::path& destination);
void ReplaceSettingsFile(const std::filesystem::path& temporary, const std::filesystem::path& destination);
bool CanExecuteFile(const std::filesystem::path& path);
}
