#include "presentation/DisplayTimeline.h"
#include "Pch.h"

namespace simxr
{
DisplayTimeline::DisplayTimeline(RuntimeTime epoch, Duration period) : m_epoch(epoch), m_period(period)
{
	if (epoch.value.count() < 0 || period.count() <= 0)
		throw std::invalid_argument("Invalid display timeline");
}

RuntimeTime DisplayTimeline::TimeAt(DisplayTick tick) const
{
	const auto available = std::numeric_limits<std::int64_t>::max() - m_epoch.value.count();
	if (tick.value > static_cast<std::uint64_t>(available / m_period.count()))
		throw std::overflow_error("Display timeline exhausted");
	return {m_epoch.value + m_period * tick.value};
}

DisplayTick DisplayTimeline::TickAtOrAfter(RuntimeTime time) const
{
	if (time <= m_epoch)
		return {};
	const auto elapsed = (time.value - m_epoch.value).count();
	const auto ticks = elapsed / m_period.count();
	return {static_cast<std::uint64_t>(ticks + (elapsed % m_period.count() != 0))};
}

Duration DisplayTimeline::Period() const
{
	return m_period;
}

RuntimeTime DisplayTimeline::Epoch() const
{
	return m_epoch;
}
} // namespace simxr
