#pragma once
#include "app/HistoryStore.h"
#include <optional>

namespace simxr::app
{
struct TimelineGesture
{
	std::optional<std::size_t> first, last, marker;
	bool stopFollowing = false, goLive = false;
};
class TimelineCanvas
{
  public:
	TimelineGesture Draw(const HistoryStore& history, bool following);
	bool DrawMarker(double time, std::uint64_t frame);
	void DrawEvent(double time, double observed, const char* text, bool failed);
	void Fit() noexcept
	{
		m_fit = true;
	}

  private:
	ShellRect m_bounds;
	double m_start = 0, m_span = 10;
	bool m_fit = false;
	std::optional<double> m_dragStart;
};
} // namespace simxr::app
