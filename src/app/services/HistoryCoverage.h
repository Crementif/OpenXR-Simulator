#pragma once

#include "app/services/FrameIdentity.h"
#include <cstdint>
#include <vector>

namespace simxr::app
{
class HistoryStore;

// Combines independently retained previews, snapshots and pins before changing timeline coverage.
class HistoryCoverage
{
  public:
	void Begin();
	void Add(FrameIdentity identity, std::uint64_t epoch, std::uint64_t capture, std::uint32_t flags);
	void Apply(HistoryStore& history);

  private:
	struct Entry
	{
		FrameIdentity identity;
		std::uint64_t epoch = 0, capture = 0;
		std::uint32_t flags = 0;
		bool applied = false;
	};
	static bool Less(const Entry& left, const Entry& right);
	std::vector<Entry> m_previous, m_next;
	const HistoryStore* m_store = nullptr;
	std::uint64_t m_identityRevision = 0;
};
} // namespace simxr::app
