#pragma once

#include "presentation/FrameTypes.h"
#include "projection_timing.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace simxr
{
class LegacyFileChannel;
class LegacyInput;
struct SessionSnapshot;

class LinuxRuntimeStatus final
{
  public:
	using Clock = std::chrono::steady_clock;
	struct FrameCounts
	{
		std::uint32_t layers;
		std::uint32_t projections;
	};

	LinuxRuntimeStatus(std::shared_ptr<LegacyFileChannel> files, int processId, ExecutionMode mode);
	LinuxRuntimeStatus(const LinuxRuntimeStatus&) = delete;
	LinuxRuntimeStatus& operator=(const LinuxRuntimeStatus&) = delete;
	// Serialized by the instance gate. Optional observation failure cannot change
	// the result of a committed native lifecycle or frame operation.
	void Update(const SessionSnapshot& session, const LegacyInput& input, Clock::time_point now,
				std::optional<FrameCounts> submitted = {}) noexcept;

  private:
	const std::shared_ptr<LegacyFileChannel> m_files;
	const int m_processId;
	const ExecutionMode m_mode;
	std::optional<SessionGeneration> m_generation;
	std::optional<Clock::time_point> m_lastPublication;
	perf::ProjectionTimingTracker m_timing;
};
} // namespace simxr
