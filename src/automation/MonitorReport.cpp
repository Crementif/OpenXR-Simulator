#include "Pch.h"
#include "automation/MonitorReport.h"

namespace simxr::automation
{
void MonitorCoverage::Add(const std::vector<MonitorFrame>& frames)
{
	++report.windows;
	for (const auto& item : frames)
	{
		if (item.frame > report.lastFrame)
		{
			if (report.lastFrame)
				report.gaps += item.frame - report.lastFrame - 1;
			report.lastFrame = item.frame;
			++(item.skipped ? report.skipped : report.observed);
			m_recent.emplace(item.frame, !item.skipped);
			if (m_recent.size() > 48)
				m_recent.erase(m_recent.begin());
		}
		else if (const auto previous = m_recent.find(item.frame);
				 previous != m_recent.end() && !previous->second && !item.skipped)
		{
			previous->second = true;
			--report.skipped;
			++report.observed;
		}
		if (item.skipped || item.reason.empty())
			continue;
		if (std::ranges::any_of(report.events, [&](const auto& event) {
				return event.frame == item.frame && event.reason == item.reason;
			}))
			continue;
		++report.eventsTotal;
		report.events.push_back(item);
		if (report.events.size() > 64)
		{
			report.events.erase(report.events.begin());
			++report.eventsDropped;
		}
	}
}
}
