#pragma once

#include <filesystem>

namespace simxr::app
{
std::filesystem::path CreatePrivateTemporaryFile(const std::filesystem::path& destination);
void CommitTemporaryFile(const std::filesystem::path& temporary, const std::filesystem::path& destination);
void RenameFileWithoutReplacing(const std::filesystem::path& source, const std::filesystem::path& destination);
bool CanExecuteFile(const std::filesystem::path& path);
}
