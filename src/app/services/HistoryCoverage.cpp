#include "app/services/HistoryCoverage.h"
#include "Pch.h"
#include "app/services/HistoryStore.h"
#include <tuple>

namespace simxr::app
{
void HistoryCoverage::Begin()
{
	m_next.clear();
}

void HistoryCoverage::Add(FrameIdentity identity, std::uint64_t epoch, std::uint64_t capture, std::uint32_t flags)
{
	m_next.push_back({identity, epoch, capture, flags & (HistoryPreview | HistorySnapshot | HistoryPinned)});
}

void HistoryCoverage::Apply(HistoryStore& history)
{
	if (m_store != &history)
		m_previous.clear();
	const auto unsorted = std::is_sorted_until(m_next.begin(), m_next.end(), Less);
	if (unsorted != m_next.end())
	{
		std::stable_sort(unsorted, m_next.end(), Less);
		std::inplace_merge(m_next.begin(), unsorted, m_next.end(), Less);
	}
	std::size_t merged = 0;
	for (const auto& entry : m_next)
	{
		if (merged && m_next[merged - 1].identity == entry.identity && m_next[merged - 1].epoch == entry.epoch)
		{
			auto& previous = m_next[merged - 1];
			previous.flags |= entry.flags;
			if (entry.capture)
				previous.capture = entry.capture;
		}
		else
			m_next[merged++] = entry;
	}
	m_next.resize(merged);
	auto current = m_next.begin();
	for (const auto& previous : m_previous)
	{
		while (current != m_next.end() && Less(*current, previous))
			++current;
		if (current == m_next.end() || Less(previous, *current))
			if (const auto index = history.Find(previous.identity, previous.epoch))
			{
				history.SetCoverage(*index, 0, 0);
				history.SetFlags(*index, HistoryPinned, false);
			}
	}
	const bool sameIdentities = m_store == &history && m_identityRevision == history.IdentityRevision() &&
								m_identityRevision != std::numeric_limits<std::uint64_t>::max();
	auto previous = m_previous.begin();
	for (auto& entry : m_next)
	{
		while (previous != m_previous.end() && Less(*previous, entry))
			++previous;
		if (sameIdentities && previous != m_previous.end() && !Less(entry, *previous) && previous->applied &&
			entry.capture == previous->capture && entry.flags == previous->flags)
			entry.applied = true;
		else if (const auto index = history.Find(entry.identity, entry.epoch))
		{
			history.SetCoverage(*index, entry.capture, entry.flags);
			history.SetFlags(*index, HistoryPinned, (entry.flags & HistoryPinned) != 0);
			entry.applied = true;
		}
	}
	m_store = &history;
	m_identityRevision = history.IdentityRevision();
	m_previous.swap(m_next);
}

bool HistoryCoverage::Less(const Entry& left, const Entry& right)
{
	return std::tie(left.epoch, left.identity.session, left.identity.run, left.identity.frame) <
		   std::tie(right.epoch, right.identity.session, right.identity.run, right.identity.frame);
}
} // namespace simxr::app
