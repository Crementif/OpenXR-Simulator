#pragma once

#include "app/HistoryStore.h"
#include "app/RuntimePanelProtocol.h"

namespace simxr::app
{
class FrameHistoryFeed
{
  public:
	void SetScope(PanelScope scope);
	void Accept(const control::Message& message);
	std::uint64_t After() const noexcept
	{
		return m_after;
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
};
} // namespace simxr::app
