#pragma once

#include <compare>
#include <cstdint>

namespace simxr
{
struct CaptureId
{
	std::uint64_t value = 0;
	auto operator<=>(const CaptureId&) const = default;
};

enum class SnapshotAdmission
{
	Accepted,
	Capacity,
	Closed,
	Unsupported,
	Invalid
};

struct CaptureReservation
{
	SnapshotAdmission result;
	CaptureId id;
};

} // namespace simxr
