#pragma once
#include "app/HistoryStore.h"
#include <array>
#include <optional>
#include <utility>

namespace simxr::app
{
struct TimelineGesture
{
	std::optional<std::size_t> first, last, marker, hovered, pin;
	bool stopFollowing = false, goLive = false;
};
enum class ClipHit
{
	None,
	Clicked,
	Opened
};
// Ruler, lanes, the lane-header column on the left and the LIVE column on the
// right. Begin() lays the regions out; the caller then draws transport controls
// into TransportRect() and LiveRect() and Draw() submits the canvas between them.
class TimelineCanvas
{
  public:
	void Begin(float headerWidth, float liveWidth);
	TimelineGesture Draw(const HistoryStore& history, bool following, std::optional<double> cursor = {},
						 std::optional<std::pair<double, double>> selection = {});
	void DrawEvent(double time, double observed, const char* text, bool failed, bool dim);
	ClipHit DrawClip(double first, double last, const char* label, bool replay);
	void DrawPause(double first, double last);
	void Fit() noexcept
	{
		m_fit = true;
	}
	void Zoom(double first, double last) noexcept;
	std::pair<double, double> VisibleRange() const noexcept
	{
		return {m_start, m_start + m_span};
	}
	ShellRect Bounds() const noexcept
	{
		return m_bounds;
	}
	// Top-left of the header cell that holds the transport buttons.
	ShellRect TransportRect() const noexcept
	{
		return m_transport;
	}
	// Right of the ruler row: LIVE while following, the button back to it otherwise.
	ShellRect LiveRect() const noexcept
	{
		return {m_bounds.x + m_bounds.width, m_transport.y, m_live, m_transport.height};
	}
	bool Compact() const noexcept
	{
		return m_compact;
	}

  private:
	enum Lane
	{
		Frames,
		Images,
		Recording,
		Events,
		LaneCount
	};
	float LaneY(Lane lane) const noexcept
	{
		return m_laneY[lane];
	}
	float X(double seconds) const noexcept
	{
		return m_bounds.x + float((seconds - m_start) / m_span) * m_bounds.width;
	}
	ShellRect m_bounds, m_transport;
	std::array<float, LaneCount> m_laneY{};
	float m_header = 0, m_live = 0, m_lane = 0, m_top = 0, m_height = 0;
	int m_lanes = 0;
	double m_start = 0, m_span = 10;
	bool m_fit = false, m_compact = false;
	std::optional<double> m_dragStart;
	bool m_rangeDrag = false;
};
} // namespace simxr::app
