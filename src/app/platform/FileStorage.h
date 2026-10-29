#pragma once

#include <filesystem>
#include <cstdint>

namespace simxr::app
{
class SettingsFileLock
{
public:
	explicit SettingsFileLock(const std::filesystem::path& path);
	~SettingsFileLock();
	SettingsFileLock(const SettingsFileLock&) = delete;
	SettingsFileLock& operator=(const SettingsFileLock&) = delete;

private:
	std::intptr_t m_handle = -1;
};

std::filesystem::path CreatePrivateTemporaryFile(const std::filesystem::path& destination);
void CommitTemporaryFile(const std::filesystem::path& temporary, const std::filesystem::path& destination);
void RenameFileWithoutReplacing(const std::filesystem::path& source, const std::filesystem::path& destination);
bool CanExecuteFile(const std::filesystem::path& path);
}
