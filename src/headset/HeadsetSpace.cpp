#include "headset/HeadsetSpace.h"
#include "Pch.h"
#include "math/Math.h"

namespace simxr::headset
{
void HeadsetSpace::Schedule(XrTime changeTime, bool valid, XrPosef previousFromNew)
{
	if (m_failed || changeTime < 0 || (m_lastTime && changeTime <= m_lastTime) || m_pending.size() == 16)
	{
		m_failed = true;
		throw std::runtime_error("Physical reference-space history lost continuity; realign the headset");
	}
	const auto normalized = valid ? NormalizedPose(previousFromNew) : std::nullopt;
	Change change{changeTime, normalized.has_value(), normalized.value_or(IdentityPose())};
	const auto position = std::upper_bound(m_pending.begin(), m_pending.end(), changeTime,
										   [](XrTime time, const Change& pending)
										   {
											   return time < pending.time;
										   });
	m_pending.insert(position, change);
}
HeadsetSpaceSample HeadsetSpace::At(XrTime time)
{
	if (m_failed || time <= 0 || time < m_lastTime)
		throw std::runtime_error("Physical reference-space sample requires a valid forward timeline");
	while (!m_pending.empty() && m_pending.front().time <= time)
	{
		const auto& change = m_pending.front();
		const auto mapped =
			change.valid ? NormalizedPose(Compose(m_current.initialFromCurrent, change.previousFromNew)) : std::nullopt;
		if (!mapped || m_current.revision == std::numeric_limits<std::uint64_t>::max())
		{
			m_failed = true;
			throw std::runtime_error("Physical reference-space transform unavailable; realign the headset");
		}
		m_current.initialFromCurrent = *mapped;
		++m_current.revision;
		m_pending.pop_front();
	}
	m_lastTime = time;
	return m_current;
}
} // namespace simxr::headset
