#pragma once

#include "app/services/ShellState.h"
#include "app/services/HistoryStore.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
	Play,
	Stop
};
class PreviewHistory;

class TimelineCanvas
{
public:
	void Begin(float headerWidth, float liveWidth, bool laneNames = true);
	TimelineGesture Draw(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop, bool following,
						 std::optional<double> cursor = {}, std::optional<std::pair<double, double>> selection = {});
	void DrawThumbnails(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop);
	void DrawEvent(double time, double observed, const char* text, bool failed, bool dim);
	ClipHit DrawClip(const char* id, double first, double last, const char* label, bool replay, bool playable = false,
					 bool stoppable = false);
	void DrawPause(double first, double last);

	void SetInputDetail(std::string detail)
	{
		m_inputDetail = std::move(detail);
	}

	void SetBudget(float milliseconds) noexcept
	{
		m_budget = milliseconds;
	}

	void SetTint(bool tint) noexcept
	{
		m_tint = tint;
	}

	// Inclusive source and replay endpoints. Frame-id offsets preserve gaps while
	// the recorded input is drawn over frames still owned by the replay.
	void DrawGhost(const HistoryStore& history, std::size_t sourceFirst, std::size_t sourceLast,
				   std::size_t targetFirst, std::size_t targetLast);

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

	ShellRect TransportRect() const noexcept
	{
		return m_transport;
	}

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
	double m_zoomSpan = 10;
	float m_budget = 0;
	bool m_fit = false, m_compact = false, m_tint = false;
	std::string m_inputDetail;
	std::vector<std::pair<float, std::string>> m_runLabels;
	enum class FrameTarget
	{
		Frame,
		Snapshot,
		Marker
	};

	struct FramePress
	{
		FrameIdentity frame;
		std::uint64_t epoch;
		double seconds;
		ImVec2 position;
		FrameTarget target = FrameTarget::Frame;
		bool range = false;
	};

	std::optional<FramePress> m_framePress;
	int m_drawnFrame = -1;
	bool m_hovered = false;

	struct ClipPress
	{
		ImGuiID id;
		ClipHit action;
		int drawnFrame;
	};

	std::optional<ClipPress> m_clipPress;
};
} // namespace simxr::app
