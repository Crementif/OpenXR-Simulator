#include "app/widgets/Icons.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <numbers>

namespace simxr::app
{
namespace
{
// Icons are designed on a 24-unit grid centred on the origin, like common icon sets.
class Pen
{
  public:
	Pen(ImDrawList* draw, ImVec2 center, float size, ImU32 color)
		: m_draw(draw), m_center(center), m_unit(size / 24), m_color(color), m_width(std::max(1.0f, size / 12))
	{
	}
	ImVec2 At(float x, float y) const
	{
		return {m_center.x + x * m_unit, m_center.y + y * m_unit};
	}
	void Line(float x0, float y0, float x1, float y1) const
	{
		m_draw->AddLine(At(x0, y0), At(x1, y1), m_color, m_width);
	}
	void Polyline(std::initializer_list<ImVec2> points, bool closed = false) const
	{
		std::array<ImVec2, 12> mapped{};
		int count = 0;
		for (const auto& point : points)
			mapped[count++] = At(point.x, point.y);
		m_draw->AddPolyline(mapped.data(), count, m_color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, m_width);
	}
	void Fill(std::initializer_list<ImVec2> points) const
	{
		std::array<ImVec2, 12> mapped{};
		int count = 0;
		for (const auto& point : points)
			mapped[count++] = At(point.x, point.y);
		m_draw->AddConvexPolyFilled(mapped.data(), count, m_color);
	}
	void Circle(float x, float y, float radius, bool filled = false) const
	{
		if (filled)
			m_draw->AddCircleFilled(At(x, y), radius * m_unit, m_color);
		else
			m_draw->AddCircle(At(x, y), radius * m_unit, m_color, 0, m_width);
	}
	void Rect(float x0, float y0, float x1, float y1, float rounding = 2, bool filled = false) const
	{
		if (filled)
			m_draw->AddRectFilled(At(x0, y0), At(x1, y1), m_color, rounding * m_unit);
		else
			m_draw->AddRect(At(x0, y0), At(x1, y1), m_color, rounding * m_unit, 0, m_width);
	}
	void Arc(float x, float y, float radius, float from, float to) const
	{
		m_draw->PathArcTo(At(x, y), radius * m_unit, from, to);
		m_draw->PathStroke(m_color, ImDrawFlags_None, m_width);
	}
	void Ellipse(float x, float y, float rx, float ry) const
	{
		m_draw->AddEllipse(At(x, y), {rx * m_unit, ry * m_unit}, m_color, 0, 0, m_width);
	}
	void Text(const char* text) const
	{
		const float size = 15 * m_unit;
		const auto extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
		m_draw->AddText(ImGui::GetFont(), size, {m_center.x - extent.x / 2, m_center.y - extent.y / 2}, m_color, text);
	}

  private:
	ImDrawList* m_draw;
	ImVec2 m_center;
	float m_unit;
	ImU32 m_color;
	float m_width;
};
constexpr float Pi = std::numbers::pi_v<float>;
void DrawPerson(const Pen& pen, bool arms)
{
	pen.Circle(0, -7, 3.5f);
	pen.Line(0, -3, 0, 6);
	pen.Line(0, 6, -4, 11);
	pen.Line(0, 6, 4, 11);
	if (arms)
		pen.Polyline({{-7, 3}, {-4, -1}, {4, -1}, {7, 3}});
}
void DrawController(const Pen& pen, bool left)
{
	const float side = left ? -1.0f : 1.0f;
	pen.Circle(side * 3, -5, 5.5f);
	pen.Circle(side * 3, -5, 2, true);
	pen.Polyline({{side * -1, -1}, {side * -5, 10}, {side * -1, 11}, {side * 3, 1}});
}
} // namespace
void DrawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color)
{
	const Pen pen(draw, center, size, color);
	switch (icon)
	{
	case Icon::Play:
		pen.Fill({{-5, -8}, {8, 0}, {-5, 8}});
		break;
	case Icon::Pause:
		pen.Rect(-7, -8, -2, 8, 1, true);
		pen.Rect(2, -8, 7, 8, 1, true);
		break;
	case Icon::StepForward:
		pen.Fill({{-7, -8}, {4, 0}, {-7, 8}});
		pen.Rect(5, -8, 8, 8, 1, true);
		break;
	case Icon::ChevronLeft:
		pen.Polyline({{4, -8}, {-4, 0}, {4, 8}});
		break;
	case Icon::ChevronRight:
		pen.Polyline({{-4, -8}, {4, 0}, {-4, 8}});
		break;
	case Icon::Record:
		pen.Circle(0, 0, 7, true);
		break;
	case Icon::Stop:
		pen.Rect(-6, -6, 6, 6, 1.5f, true);
		break;
	case Icon::Library:
		pen.Rect(-9, -8, -4, 9, 1);
		pen.Rect(-3, -8, 2, 9, 1);
		pen.Polyline({{4, -6}, {8, -7}, {11, 8}, {7, 9}}, true);
		break;
	case Icon::More:
		for (const float x : {-7.0f, 0.0f, 7.0f})
			pen.Circle(x, 0, 2, true);
		break;
	case Icon::Settings:
		pen.Circle(0, 0, 3.5f);
		for (int tooth = 0; tooth < 8; ++tooth)
		{
			const float angle = tooth * Pi / 4;
			pen.Line(std::cos(angle) * 6.5f, std::sin(angle) * 6.5f, std::cos(angle) * 9.5f, std::sin(angle) * 9.5f);
		}
		pen.Circle(0, 0, 7);
		break;
	case Icon::Help:
		pen.Circle(0, 0, 10);
		pen.Text("?");
		break;
	case Icon::Globe:
		pen.Circle(0, 0, 9);
		pen.Ellipse(0, 0, 4, 9);
		pen.Line(-9, 0, 9, 0);
		break;
	case Icon::Axes:
		pen.Line(-7, 7, 9, 7);
		pen.Line(-7, 7, -7, -9);
		pen.Line(-7, 7, 2, -2);
		break;
	case Icon::App:
		pen.Rect(-10, -8, 10, 8, 2);
		pen.Line(-10, -3, 10, -3);
		break;
	case Icon::Refresh:
		pen.Arc(0, 0, 8, -Pi * .35f, Pi * 1.4f);
		pen.Fill({{4, -10}, {9, -6}, {3, -4}});
		break;
	case Icon::Close:
		pen.Line(-6, -6, 6, 6);
		pen.Line(6, -6, -6, 6);
		break;
	case Icon::Player:
		DrawPerson(pen, false);
		pen.Ellipse(0, 11, 8, 2);
		break;
	case Icon::Head:
		pen.Circle(0, 0, 9);
		pen.Rect(-7, -3, 7, 3, 2, true);
		break;
	case Icon::HandLeft:
		DrawController(pen, true);
		break;
	case Icon::HandRight:
		DrawController(pen, false);
		break;
	case Icon::Body:
		DrawPerson(pen, true);
		break;
	case Icon::Eye:
		pen.Polyline({{-10, 0}, {-5, -5}, {0, -6}, {5, -5}, {10, 0}, {5, 5}, {0, 6}, {-5, 5}}, true);
		pen.Circle(0, 0, 2.5f, true);
		break;
	case Icon::Tracker:
		pen.Polyline({{0, -9}, {8, -4.5f}, {8, 4.5f}, {0, 9}, {-8, 4.5f}, {-8, -4.5f}}, true);
		pen.Circle(0, 0, 3, true);
		break;
	case Icon::Headset:
		pen.Polyline({{-10, -5}, {10, -5}, {10, 5}, {3, 5}, {0, 2}, {-3, 5}, {-10, 5}}, true);
		break;
	case Icon::Scenarios:
		pen.Line(-7, -9, -7, 10);
		pen.Polyline({{-7, -9}, {8, -5}, {-7, 1}});
		break;
	case Icon::Pin:
		pen.Circle(0, -4, 5);
		pen.Line(0, 1, 0, 10);
		break;
	case Icon::Export:
		pen.Polyline({{-4, -3}, {-9, -3}, {-9, 9}, {9, 9}, {9, -3}, {4, -3}});
		pen.Line(0, 3, 0, -10);
		pen.Polyline({{-4, -6}, {0, -10}, {4, -6}});
		break;
	case Icon::Live:
		pen.Circle(0, 0, 3, true);
		pen.Arc(0, 0, 7, -Pi * .3f, Pi * .3f);
		pen.Arc(0, 0, 7, Pi * .7f, Pi * 1.3f);
		break;
	case Icon::Warning:
		pen.Polyline({{0, -9}, {10, 8}, {-10, 8}}, true);
		pen.Line(0, -3, 0, 2);
		pen.Circle(0, 5, 1, true);
		break;
	case Icon::Rename:
		pen.Polyline({{-8, 8}, {-8, 4}, {5, -9}, {9, -5}, {-4, 8}}, true);
		pen.Line(2, -6, 6, -2);
		break;
	case Icon::Delete:
		pen.Line(-9, -6, 9, -6);
		pen.Polyline({{-3, -6}, {-3, -9}, {3, -9}, {3, -6}});
		pen.Polyline({{-7, -6}, {-6, 9}, {6, 9}, {7, -6}});
		pen.Line(-2, -2, -2, 5);
		pen.Line(2, -2, 2, 5);
		break;
	case Icon::Script:
		pen.Polyline({{-4, -8}, {-9, 0}, {-4, 8}});
		pen.Polyline({{4, -8}, {9, 0}, {4, 8}});
		pen.Line(2, -9, -2, 9);
		break;
	case Icon::StepBack:
		pen.Fill({{7, -8}, {-4, 0}, {7, 8}});
		pen.Rect(-8, -8, -5, 8, 1, true);
		break;
	case Icon::ChevronUp:
		pen.Polyline({{-8, 4}, {0, -4}, {8, 4}});
		break;
	case Icon::ChevronDown:
		pen.Polyline({{-8, -4}, {0, 4}, {8, -4}});
		break;
	case Icon::Focus:
		pen.Polyline({{-9, -3}, {-9, -9}, {-3, -9}});
		pen.Polyline({{3, -9}, {9, -9}, {9, -3}});
		pen.Polyline({{9, 3}, {9, 9}, {3, 9}});
		pen.Polyline({{-3, 9}, {-9, 9}, {-9, 3}});
		break;
	case Icon::Crop:
		pen.Polyline({{-5, -10}, {-5, 5}, {10, 5}});
		pen.Polyline({{-10, -5}, {5, -5}, {5, 10}});
		break;
	case Icon::Zoom:
		pen.Circle(-2, -2, 6);
		pen.Line(2.5f, 2.5f, 8, 8);
		break;
	case Icon::Gauge:
		pen.Arc(0, 3, 9, std::numbers::pi_v<float>, 2 * std::numbers::pi_v<float>);
		pen.Line(0, 3, 5, -3);
		pen.Circle(0, 3, 1.5f, true);
		break;
	}
}
} // namespace simxr::app
