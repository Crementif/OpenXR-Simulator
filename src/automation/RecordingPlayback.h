#pragma once

#include <cstdint>
#include <filesystem>

namespace simxr::automation
{
class RigRuntime;
class RigProgress;
// Validates the complete archive before taking input ownership, then reads one page at a time.
void ReplayRecordingArchive(const std::filesystem::path& directory, std::uint64_t seed, RigRuntime& runtime,
							RigProgress& progress);
} // namespace simxr::automation
