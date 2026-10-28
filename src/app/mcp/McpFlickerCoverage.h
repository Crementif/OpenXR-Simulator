#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
class FlickerCoverage
{
  public:
	void AddWindow(const nlohmann::json& entries, const nlohmann::json& analysis);
	nlohmann::json Report() const;

  private:
	std::uint64_t m_last = 0, m_analyzed = 0, m_skipped = 0, m_unobserved = 0, m_eventsTotal = 0, m_eventsDropped = 0;
	std::map<std::uint64_t, bool> m_recentFrames;
	std::set<std::pair<std::string, std::string>> m_seenEvents;
	nlohmann::json m_events = nlohmann::json::array();
};
}
