#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace simxr::automation
{
struct MonitorFrame
{
	std::uint64_t frame = 0;
	bool skipped = false;
	std::string reason;
};

struct MonitorReport
{
	std::uint64_t windows = 0, observed = 0, skipped = 0, gaps = 0, lastFrame = 0;
	std::uint64_t eventsTotal = 0, eventsDropped = 0, missedCadences = 0;
	std::uint64_t lastAnalyzed = 0, lastSkipped = 0, lastPairs = 0, lastGaps = 0;
	std::vector<MonitorFrame> events;
};

class MonitorCoverage
{
public:
	void Add(const std::vector<MonitorFrame>& frames);
	MonitorReport report;

private:
	std::map<std::uint64_t, bool> m_recent;
};
}
