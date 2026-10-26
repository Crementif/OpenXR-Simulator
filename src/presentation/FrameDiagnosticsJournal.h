#pragma once

#include "presentation/FrameTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <openxr/openxr.h>
#include <optional>
#include <string_view>
#include <vector>

namespace simxr
{
enum class FrameTraceKind
{
	WaitStarted,
	WaitReturned,
	Issued,
	Begun,
	Submitted,
	Discarded,
	Cancelled,
	NativeSubmitted,
	GpuComplete,
	GpuFailed,
	DisplaySelected,
	DisplayRepeated,
	DisplayCleared,
	CaptureReady,
	CaptureFailed,
	PreviewDropped,
	CommandsApplied,
	CommandRejected,
	ApiError,
	Validation,
	DisplayEmpty,
	ImageRetired,
	RuntimeWork
};
enum class FrameDiagnosticCategory
{
	Timing,
	Performance,
	ApiError,
	Validation,
	Command
};
struct FrameDiagnostic
{
	std::uint64_t sequence = 0;
	FrameKey frame;
	RuntimeTime time;
	Duration hostTime{};
	FrameTraceKind kind = FrameTraceKind::WaitStarted;
	FrameDiagnosticCategory category = FrameDiagnosticCategory::Timing;
	XrResult result = XR_SUCCESS;
	// Optional capture, display-tick or command identity, according to kind.
	std::uint64_t related = 0;
	std::array<char, 192> detail{};
	std::uint16_t detailLength = 0;
	bool detailTruncated = false;
	std::string_view Detail() const
	{
		return {detail.data(), detailLength};
	}
};
struct FrameDiagnostics
{
	std::size_t capacity = 0;
	std::uint64_t dropped = 0, oldest = 0, next = 1;
	bool truncated = false;
	std::vector<FrameDiagnostic> events;
};

// A bounded observation journal. It never owns frame/GPU resources and never
// takes a FrameHistory lock. Timestamp sources must be safe for concurrent calls;
// they run outside the journal lock and may inspect Read().
class FrameDiagnosticsJournal
{
  public:
	using TimestampSource = std::function<Duration()>;
	explicit FrameDiagnosticsJournal(std::size_t capacity = 1024, TimestampSource timestamp = {});
	~FrameDiagnosticsJournal();
	// Storage is preallocated. A clock/locking failure returns an unsequenced
	// event (sequence == 0) and does not change retained observations.
	FrameDiagnostic Trace(FrameTraceKind kind, FrameKey frame, RuntimeTime time, XrResult result = XR_SUCCESS,
						  FrameDiagnosticCategory category = FrameDiagnosticCategory::Timing, std::uint64_t related = 0,
						  std::string_view detail = {}) noexcept;
	FrameDiagnostics Read(std::uint64_t after = 0, std::size_t count = 128, std::optional<FrameKey> frame = {}) const;

	// Caller reserves storage before native acceptance. Failure leaves the prior snapshot intact.
	bool ReadInto(FrameDiagnostics& snapshot, std::uint64_t after, std::size_t count,
				  std::optional<FrameKey> frame = {}) const noexcept;

  private:
	void CopyLocked(FrameDiagnostics& snapshot, std::uint64_t after, std::size_t count,
					std::optional<FrameKey> frame) const noexcept;
	static std::size_t ValidateCapacity(std::size_t capacity);
	static void CopyDetail(FrameDiagnostic& event, std::string_view detail) noexcept;
	TimestampSource m_timestamp;
	mutable std::mutex m_mutex;
	std::vector<FrameDiagnostic> m_events;
	std::uint64_t m_next = 1;
	std::size_t m_retained = 0;
};
} // namespace simxr
