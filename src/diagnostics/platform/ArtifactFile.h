#pragma once

#include <cstdint>
#include <filesystem>

namespace flicker
{
std::uint64_t ProcessId();
std::filesystem::path CreateTemporaryArtifact(const std::filesystem::path& destination);
void CommitArtifact(const std::filesystem::path& temporary, const std::filesystem::path& destination);
} // namespace flicker
