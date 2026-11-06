#include "presentation/DisplayTimeline.h"
#include "Pch.h"

namespace simxr
{
DisplayTimeline::DisplayTimeline(RuntimeTime epoch, Duration period) : m_epoch(epoch), m_current{{}, epoch, period}
{
	if (epoch.value.count() < 0 || period.count() <= 0)
		throw std::invalid_argument("Invalid display timeline");
}

RuntimeTime DisplayTimeline::TimeAt(DisplayTick tick) const
{
	const auto& segment = SegmentFor(tick);
	const auto ticks = tick.value - segment.tick.value;
	const auto available = std::numeric_limits<std::int64_t>::max() - segment.time.value.count();
	if (ticks > static_cast<std::uint64_t>(available / segment.period.count()))
		throw std::overflow_error("Display timeline exhausted");
	return {segment.time.value + segment.period * ticks};
}

DisplayTick DisplayTimeline::TickAtOrAfter(RuntimeTime time) const
{
	const Segment* segment = time > m_current.time ? &m_current : nullptr;
	if (!segment && m_earlier)
		for (auto earlier = m_earlier->rbegin(); earlier != m_earlier->rend(); ++earlier)
			if (time > earlier->time)
			{
				segment = &*earlier;
				break;
			}
	if (!segment)
		return m_earlier ? m_earlier->front().tick : m_current.tick;
	const auto elapsed = (time.value - segment->time.value).count();
	const auto ticks = elapsed / segment->period.count();
	return {segment->tick.value + static_cast<std::uint64_t>(ticks + (elapsed % segment->period.count() != 0))};
}

DisplayTick DisplayTimeline::TickAtOrBefore(RuntimeTime time) const
{
	const Segment* segment = &m_current;
	if (time < m_current.time && m_earlier)
		for (auto earlier = m_earlier->rbegin(); earlier != m_earlier->rend(); ++earlier)
		{
			segment = &*earlier;
			if (time >= earlier->time)
				break;
		}
	if (time <= segment->time)
		return segment->tick;
	const auto ticks = (time.value - segment->time.value).count() / segment->period.count();
	return {segment->tick.value + static_cast<std::uint64_t>(ticks)};
}

Duration DisplayTimeline::Period() const
{
	return m_current.period;
}

RuntimeTime DisplayTimeline::Epoch() const
{
	return m_epoch;
}

DisplayTimeline DisplayTimeline::Retimed(DisplayTick boundary, Duration period) const
{
	if (period.count() <= 0 || boundary < m_current.tick)
		throw std::invalid_argument("Invalid display timeline change");
	auto result = *this;
	if (boundary == m_current.tick)
	{
		result.m_current.period = period;
		return result;
	}
	auto earlier =
		m_earlier ? std::make_shared<std::vector<Segment>>(*m_earlier) : std::make_shared<std::vector<Segment>>();
	earlier->push_back(m_current);
	result.m_current = {boundary, TimeAt(boundary), period};
	result.m_earlier = std::move(earlier);
	return result;
}

const DisplayTimeline::Segment& DisplayTimeline::SegmentFor(DisplayTick tick) const
{
	if (tick >= m_current.tick || !m_earlier)
		return m_current;
	for (auto earlier = m_earlier->rbegin(); earlier != m_earlier->rend(); ++earlier)
		if (tick >= earlier->tick)
			return *earlier;
	return m_earlier->front();
}
} // namespace simxr
