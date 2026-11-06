#include "app/widgets/Icons.h"
#include "app/UiPch.h"

#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>

namespace simxr::app
{
static constexpr std::pair<std::string_view, std::string_view> s_sources[]{
#include "IconSources.inc"
};
// SVG file stem per Icon, in enum order.
static constexpr std::array<std::string_view, 46> s_files{
	"play",		 "pause",	   "step-forward", "chevron-left", "chevron-right", "record", "stop",	 "library",
	"more",		 "settings",   "help",		   "globe",		   "axes",			"app",	  "refresh", "close",
	"player",	 "head",	   "hand-left",	   "hand-right",   "body",			"eye",	  "tracker", "headset",
	"scenarios", "pin",		   "export",	   "live",		   "warning",		"rename", "delete",	 "script",
	"step-back", "chevron-up", "chevron-down", "focus",		   "crop",			"zoom",	  "gauge",	 "lock",
	"unlock",	 "reset-pose", "keyboard",	   "history",	   "fullscreen",	"point"};
static_assert(s_files.size() == static_cast<std::size_t>(Icon::Point) + 1, "Every icon needs a file");

// One path of an icon in its 24-unit grid, centred on the origin.
struct IconPath
{
	std::vector<ImVec2> points;
	// Stroke width in grid units; zero for a filled area.
	float stroke = 0;
	bool closed = false;
};

static void AppendCubic(const float* curve, std::vector<ImVec2>& points)
{
	const auto distance = [&](int a, int b) {
		return std::hypot(curve[b * 2] - curve[a * 2], curve[b * 2 + 1] - curve[a * 2 + 1]);
	};
	// nanosvg stores straight segments as cubics with their control points on the line.
	const float hull = distance(0, 1) + distance(1, 2) + distance(2, 3), chord = distance(0, 3);
	const int steps = hull - chord < .01f ? 1 : std::clamp(static_cast<int>(std::ceil(hull * 1.5f)), 2, 24);
	for (int step = 1; step <= steps; ++step)
	{
		const float t = static_cast<float>(step) / steps, u = 1 - t;
		const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
		points.push_back({a * curve[0] + b * curve[2] + c * curve[4] + d * curve[6] - 12,
						  a * curve[1] + b * curve[3] + c * curve[5] + d * curve[7] - 12});
	}
}

// Repeated points make zero-length segments that ImGui cannot join or fill.
static void RemoveRepeats(IconPath& path)
{
	const auto same = [](ImVec2 a, ImVec2 b) {
		return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f;
	};
	path.points.erase(std::unique(path.points.begin(), path.points.end(), same), path.points.end());
	if (path.closed && path.points.size() > 2 && same(path.points.front(), path.points.back()))
		path.points.pop_back();
}

// ImGui's anti-aliased fill expects clockwise points on screen.
static void MakeClockwise(std::vector<ImVec2>& points)
{
	float area = 0;
	for (std::size_t index = 0; index < points.size(); ++index)
	{
		const auto& a = points[index];
		const auto& b = points[(index + 1) % points.size()];
		area += a.x * b.y - b.x * a.y;
	}
	if (area < 0)
		std::reverse(points.begin(), points.end());
}

static std::vector<IconPath> Parse(std::string_view source)
{
	std::string text(source);
	std::vector<IconPath> paths;
	std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(nsvgParse(text.data(), "px", 96), nsvgDelete);
	if (!image)
		return paths;
	for (auto* shape = image->shapes; shape; shape = shape->next)
	{
		if (!(shape->flags & NSVG_FLAGS_VISIBLE))
			continue;
		const bool filled = shape->fill.type != NSVG_PAINT_NONE;
		const bool stroked = shape->stroke.type != NSVG_PAINT_NONE && shape->strokeWidth > 0;
		for (auto* outline = shape->paths; outline; outline = outline->next)
		{
			IconPath path;
			path.closed = outline->closed;
			path.points.push_back({outline->pts[0] - 12, outline->pts[1] - 12});
			for (int point = 0; point + 3 < outline->npts; point += 3)
				AppendCubic(&outline->pts[point * 2], path.points);
			RemoveRepeats(path);
			if (path.points.size() < 2)
				continue;
			if (filled && path.points.size() > 2)
			{
				auto area = path;
				MakeClockwise(area.points);
				paths.push_back(std::move(area));
			}
			if (stroked)
			{
				path.stroke = shape->strokeWidth;
				paths.push_back(std::move(path));
			}
		}
	}
	return paths;
}

static const std::vector<IconPath>& Paths(Icon icon)
{
	static const auto s_paths = [] {
		std::array<std::vector<IconPath>, s_files.size()> paths;
		for (std::size_t index = 0; index < s_files.size(); ++index)
			for (const auto& [name, source] : s_sources)
				if (name == s_files[index])
					paths[index] = Parse(source);
		return paths;
	}();
	return s_paths[static_cast<std::size_t>(icon)];
}

void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color)
{
	const float unit = size / 24;
	const auto map = [&](ImVec2 point) {
		return ImVec2{center.x + point.x * unit, center.y + point.y * unit};
	};
	for (const auto& path : Paths(icon))
	{
		draw->PathClear();
		for (const auto& point : path.points)
			draw->PathLineTo(map(point));
		if (!path.stroke)
		{
			draw->PathFillConcave(color);
			continue;
		}
		const float width = std::max(1.0f, path.stroke * unit);
		draw->PathStroke(color, path.closed ? ImDrawFlags_Closed : ImDrawFlags_None, width);
		// Round caps, as the SVGs specify.
		if (!path.closed)
		{
			draw->AddCircleFilled(map(path.points.front()), width / 2, color);
			draw->AddCircleFilled(map(path.points.back()), width / 2, color);
		}
	}
}
} // namespace simxr::app
