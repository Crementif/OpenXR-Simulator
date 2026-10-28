#pragma once

#include "app/services/HistoryStore.h"
#include "app/services/InputHistory.h"
#include "app/services/RuntimePanelProtocol.h"
#include <string>

namespace simxr::app
{
class FrameHistoryFeed
{
  public:
	FrameHistoryFeed();
	std::size_t RetainedBytes() const noexcept;
	void SetScope(PanelScope scope);
	// Returns why a page was skipped. A skipped page is never requested again;
	// the next accepted frame starts with a gap instead. An undecodable identity
	// stops reads until the connection or selected run changes.
	std::string Accept(const control::Message& message);
	bool CanRead() const noexcept
	{
		return !m_unreadable;
	}
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
	const InputHistory& Input() const noexcept
	{
		return m_input;
	}

  private:
	HistoryStore m_history;
	InputHistory m_input;
	PanelScope m_scope;
	std::uint64_t m_after = 0;
	std::int64_t m_origin = 0, m_previous = 0;
	double m_offset = 0;
	bool m_newSegment = true, m_skipped = false, m_unreadable = false;
	void Append(const control::Message& message, const control::Message& frames);
};
} // namespace simxr::app
