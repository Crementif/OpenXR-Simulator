#include "platform/Clock.h"
#include "Pch.h"

namespace simxr
{
HostTime::duration HostTimeDifference(HostTime end, HostTime begin)
{
	using Rep = HostTime::duration::rep;
	const auto first = begin.time_since_epoch().count();
	const auto last = end.time_since_epoch().count();
	if ((first > 0 && last < std::numeric_limits<Rep>::min() + first) ||
		(first < 0 && last > std::numeric_limits<Rep>::max() + first))
		throw std::overflow_error("Host time interval exhausted");
	return end - begin;
}
} // namespace simxr
