#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace simxr::app::mcp
{
struct LogFileSnapshot
{
	std::vector<std::byte> bytes;
	std::uint64_t size = 0;
	bool truncated = false, changed = false;
};
std::optional<LogFileSnapshot> ReadLogFile(const std::filesystem::path& path);
bool ClearLogFile(const std::filesystem::path& path);
}
