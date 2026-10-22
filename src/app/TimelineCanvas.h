#pragma once
#include "app/HistoryStore.h"
#include <array>
#include <imgui.h>
#include <optional>
#include <string>
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
	Opened,
	Play
};
// Ruler, lanes, the lane-header column on the left and the LIVE column on the
// right. Begin() lays the regions out; the caller then draws transport controls
// into TransportRect() and LiveRect() and Draw() submits the canvas between them.
class PreviewHistory;
class TimelineCanvas
{
  public:
	void Begin(float headerWidth, float liveWidth);
	TimelineGesture Draw(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop, bool following, std::optional<double> cursor = {},
						 std::optional<std::pair<double, double>> selection = {});
	void DrawThumbnails(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop);
	void DrawEvent(double time, double observed, const char* text, bool failed, bool dim);
	// A recording (red) or replay (blue) tints every lane over its frames, with a
	// name tab at its top-left; a playable clip's tab carries a play button.
	ClipHit DrawClip(double first, double last, const char* label, bool replay, bool playable = false);
	// A paused span: a solid band on the Frame time lane, labelled when wide enough.
	void DrawPause(double first, double last);
	// Extra lines for the Input lane's tooltip, such as the connected tracker roles. Set before Begin().
	void SetInputDetail(std::string detail)
	{
		m_inputDetail = std::move(detail);
	}
	// The frame budget in milliseconds (the display period); 0 scales bars to 30 fps.
	void SetBudget(float milliseconds) noexcept
	{
		m_budget = milliseconds;
	}
	// Tints every lane under frames that missed their display time.
	void SetTint(bool tint) noexcept
	{
		m_tint = tint;
	}
	// Draws the source frames of a replayed take over the replay's Input lane,
	// frame for frame from anchor, so recorded and applied input can be compared.
	void DrawGhost(const HistoryStore& history, std::size_t source, std::size_t anchor, std::size_t count);
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
		Input,
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
	float m_budget = 0;
	bool m_fit = false, m_compact = false, m_tint = false;
	std::string m_inputDetail;
	std::optional<double> m_dragStart;
	bool m_rangeDrag = false;
};
} // namespace simxr::app
