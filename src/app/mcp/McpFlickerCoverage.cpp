#include "app/mcp/McpPch.h"
#include "app/mcp/McpFlickerCoverage.h"
#include "app/mcp/McpIncident.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;

void FlickerCoverage::AddWindow(const Json& entries, const Json& analysis)
{
	if (!entries.is_array() || entries.size() > 24 || !analysis.at("skipped_frames").is_array() ||
		analysis.at("skipped_frames").size() > entries.size() || !analysis.at("events").is_array() ||
		analysis.at("events").size() > entries.size())
		throw std::runtime_error("Invalid flicker analysis coverage");
	std::set<std::uint64_t> skipped;
	for (const auto& item : analysis.at("skipped_frames"))
		skipped.insert(IncidentIdentifier(item.at("frame_id"), "skipped frame"));
	for (const auto& entry : entries)
	{
		const auto frame = IncidentIdentifier(entry.at("frame_id"), "frame identifier");
		const bool analyzed = !skipped.contains(frame);
		if (frame <= m_last)
		{
			const auto previous = m_recentFrames.find(frame);
			if (previous != m_recentFrames.end() && !previous->second && analyzed)
			{
				previous->second = true;
				--m_skipped;
				++m_analyzed;
			}
			continue;
		}
		if (m_last && frame > m_last + 1)
			m_unobserved += frame - m_last - 1;
		m_last = frame;
		if (analyzed)
			++m_analyzed;
		else
			++m_skipped;
		m_recentFrames.emplace(frame, analyzed);
		if (m_recentFrames.size() > 48)
			m_recentFrames.erase(m_recentFrames.begin());
	}
	for (const auto& event : analysis.at("events"))
	{
		const auto key = std::pair{event.at("frame_id").get<std::string>(), event.at("reason").get<std::string>()};
		if (m_seenEvents.insert(key).second)
		{
			++m_eventsTotal;
			m_events.push_back(event);
			if (m_events.size() > 64)
			{
				m_seenEvents.erase({m_events[0].at("frame_id").get<std::string>(), m_events[0].at("reason").get<std::string>()});
				m_events.erase(m_events.begin());
				++m_eventsDropped;
			}
		}
	}
}

Json FlickerCoverage::Report() const
{
	return {{"frames_observed", m_analyzed}, {"frames_skipped", m_skipped}, {"unobserved_frames", m_unobserved},
		{"last_frame", m_last ? Json(std::to_string(m_last)) : Json(nullptr)}, {"events", m_events},
		{"events_total", m_eventsTotal}, {"events_dropped", m_eventsDropped}};
}
}
