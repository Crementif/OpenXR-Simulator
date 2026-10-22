#pragma once

#include "app/HistoryStore.h"
#include "app/RuntimePanelProtocol.h"
#include <string>

namespace simxr::app
{
class FrameHistoryFeed
{
  public:
	void SetScope(PanelScope scope);
	// Returns why a page was skipped. A skipped page is never requested again;
	// the next accepted frame starts with a gap instead.
	std::string Accept(const control::Message& message);
	std::uint64_t After() const noexcept
	{
		return m_after;
	}
	HistoryStore& History() noexcept
	{
		return m_history;
	}
	const HistoryStore& History() const noexcept
	{
		return m_history;
	}

  private:
	HistoryStore m_history;
	PanelScope m_scope;
	std::uint64_t m_after = 0;
	std::int64_t m_origin = 0, m_previous = 0;
	double m_offset = 0;
	bool m_newSegment = true, m_skipped = false;
	void Append(const control::Message& message);
};
} // namespace simxr::app
