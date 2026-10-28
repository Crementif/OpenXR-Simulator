#include "math/Math.h"
#include "app/UiPch.h"
#include "app/widgets/Theme.h"
#include "app/widgets/LiveField.h"
#include "app/widgets/SpatialWidgets.h"
#include <imgui_internal.h>

namespace simxr::app
{
static constexpr float s_degrees = 180 / std::numbers::pi_v<float>;
static XrVector3f Cross(XrVector3f a, XrVector3f b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static float Dot(XrVector3f a, XrVector3f b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
static XrVector3f Normalize(XrVector3f a)
{
	const float length = std::sqrt(Dot(a, a));
	return length > 1e-6f ? XrVector3f{a.x / length, a.y / length, a.z / length} : XrVector3f{1, 0, 0};
}
static XrVector3f Lift(XrVector2f point)
{
	const float squared = point.x * point.x + point.y * point.y;
	if (squared <= 1)
		return {point.x, point.y, std::sqrt(1 - squared)};
	const float length = std::sqrt(squared);
	return {point.x / length, point.y / length, 0};
}
static float AngleAbout(XrVector3f axis, XrVector2f from, XrVector2f to)
{
	auto project = [&](XrVector3f value)
	{
		const float along = Dot(value, axis);
		return XrVector3f{value.x - axis.x * along, value.y - axis.y * along, value.z - axis.z * along};
	};
	const auto first = project(Lift(from)), second = project(Lift(to));
	return std::atan2(Dot(Cross(first, second), axis), Dot(first, second));
}
struct OrbCanvas
{
	ImDrawList* draw;
	ImVec2 center;
	float radius, scale;
	ImVec2 Point(XrVector3f view) const
	{
		return {center.x + view.x * radius, center.y - view.y * radius};
	}
};
static void DrawModel(const OrbCanvas& canvas, OrbModel model, XrQuaternionf local, ImU32 color)
{
	auto* draw = canvas.draw;
	const auto view = [&](XrVector3f point)
	{
		return Rotate(local, point);
	};
	const auto ink = theme::Ink;
	if (model == OrbModel::Controller)
	{
		// Parts toward the viewer are brighter and thicker; the ring's far half is drawn behind the handle.
		const auto tone = [&](float depth)
		{
			return theme::Mix(theme::Mix(ink, color, .35f), color, std::clamp(.55f + depth * .6f, 0.0f, 1.0f));
		};
		const auto bottom = view({0, -.28f, .38f}), top = view({0, .04f, -.24f});
		std::array<XrVector3f, 32> ring;
		for (std::size_t step = 0; step < ring.size(); ++step)
		{
			const float angle = step * 2 * std::numbers::pi_v<float> / ring.size();
			const float c = std::cos(angle) * .32f, s = std::sin(angle) * .32f;
			ring[step] = view({c, .12f - .866f * s, -.45f - .5f * s});
		}
		const float handleDepth = (bottom.z + top.z) / 2;
		const auto drawRing = [&](bool front)
		{
			for (const bool outline : {true, false})
				for (std::size_t step = 0; step < ring.size(); ++step)
				{
					const auto& a = ring[step];
					const auto& b = ring[(step + 1) % ring.size()];
					const float depth = (a.z + b.z) / 2;
					if ((depth >= handleDepth) != front)
						continue;
					const float width = (4 + 2.5f * depth) * canvas.scale;
					draw->AddLine(canvas.Point(a), canvas.Point(b), outline ? ink : tone(depth),
								  outline ? width + 3 * canvas.scale : width);
				}
		};
		drawRing(false);
		const auto a = canvas.Point(bottom), b = canvas.Point(top);
		const float base = .11f * canvas.radius;
		const float ra = base * (1 + .3f * bottom.z), rb = base * (1 + .3f * top.z);
		const float length = std::max(.001f, std::hypot(b.x - a.x, b.y - a.y));
		const ImVec2 across{-(b.y - a.y) / length, (b.x - a.x) / length};
		for (const float grow : {1.5f * canvas.scale, 0.0f})
		{
			const auto fill = grow > 0 ? ink : tone(handleDepth);
			draw->AddQuadFilled({a.x + across.x * (ra + grow), a.y + across.y * (ra + grow)},
								{b.x + across.x * (rb + grow), b.y + across.y * (rb + grow)},
								{b.x - across.x * (rb + grow), b.y - across.y * (rb + grow)},
								{a.x - across.x * (ra + grow), a.y - across.y * (ra + grow)}, fill);
			draw->AddCircleFilled(a, ra + grow, grow > 0 ? ink : tone(bottom.z));
			draw->AddCircleFilled(b, rb + grow, grow > 0 ? ink : tone(top.z));
		}
		drawRing(true);
		return;
	}
	if (model == OrbModel::Puck)
	{
		std::array<ImVec2, 32> rim;
		const float facing = std::abs(view({0, 1, 0}).z);
		for (std::size_t step = 0; step < rim.size(); ++step)
		{
			const float angle = step * 2 * std::numbers::pi_v<float> / rim.size();
			rim[step] = canvas.Point(view({std::cos(angle) * .42f, 0, std::sin(angle) * .42f}));
		}
		draw->AddConvexPolyFilled(rim.data(), static_cast<int>(rim.size()),
								  theme::Mix(theme::Ink, color, .5f + facing * .5f));
		draw->AddPolyline(rim.data(), static_cast<int>(rim.size()), color, ImDrawFlags_Closed, 2 * canvas.scale);
		draw->AddCircleFilled(canvas.Point(view({0, .05f, -.3f})), 3.5f * canvas.scale, theme::Ok);
		return;
	}
	const std::array<XrVector3f, 8> corners{XrVector3f{-.45f, -.25f, -.35f},
											{.45f, -.25f, -.35f},
											{.45f, .25f, -.35f},
											{-.45f, .25f, -.35f},
											{-.45f, -.25f, .2f},
											{.45f, -.25f, .2f},
											{.45f, .25f, .2f},
											{-.45f, .25f, .2f}};
	struct Face
	{
		std::array<int, 4> corners;
		XrVector3f normal;
		bool front;
	};
	const std::array<Face, 6> faces{Face{{0, 1, 2, 3}, {0, 0, -1}, true}, Face{{5, 4, 7, 6}, {0, 0, 1}, false},
									Face{{1, 5, 6, 2}, {1, 0, 0}, false}, Face{{4, 0, 3, 7}, {-1, 0, 0}, false},
									Face{{3, 2, 6, 7}, {0, 1, 0}, false}, Face{{4, 5, 1, 0}, {0, -1, 0}, false}};
	for (const auto& face : faces)
	{
		const float facing = view(face.normal).z;
		if (facing <= 0)
			continue;
		std::array<ImVec2, 4> points;
		for (std::size_t index = 0; index < 4; ++index)
			points[index] = canvas.Point(view(corners[face.corners[index]]));
		const auto fill = theme::Mix(theme::Ink, face.front ? color : theme::TextDim, .35f + facing * .65f);
		draw->AddConvexPolyFilled(points.data(), 4, fill);
		draw->AddPolyline(points.data(), 4, ink, ImDrawFlags_Closed, 1.5f * canvas.scale);
	}
}
void CancelRotationOrb(ArcballState& state)
{
	if (state.activeField && ImGui::GetActiveID() == state.activeField)
		ImGui::ClearActiveID();
	state.dragging = false;
	state.activeField = 0;
}
SpatialEditResult RotationOrb(const char* id, ArcballState& state, XrQuaternionf& orientation, XrQuaternionf view,
							  XrQuaternionf reference, bool enabled, OrbModel model, ImU32 color, float width,
							  float size)
{
	if (!enabled)
		CancelRotationOrb(state);
	state.drawnFrame = ImGui::GetFrameCount();
	state.activeField = 0;
	if (state.dragging)
		view = state.view;
	const float scale = ImGui::GetFontSize() / 16;
	size *= scale;
	ImGui::PushID(id);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - size) / 2));
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 center{minimum.x + size / 2, minimum.y + size / 2};
	const float radius = size * .46f;
	ImGui::InvisibleButton("Orb", {size, size});
	const bool hovered = ImGui::IsItemHovered();
	// The wheel rolls the device here instead of scrolling the page.
	if (hovered && enabled)
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
	const auto mouse = ImGui::GetIO().MousePos;
	const XrVector2f point{(mouse.x - center.x) / radius, (center.y - mouse.y) / radius};
	const float distance = std::sqrt(point.x * point.x + point.y * point.y);
	auto* draw = ImGui::GetWindowDrawList();
	for (int ring = 24; ring > 0; --ring)
	{
		// Lit from the upper left: each smaller disc moves toward the light, so the ball reads as a sphere.
		const float t = ring / 24.0f, shift = (1 - t) * radius * .32f;
		draw->AddCircleFilled(
			{center.x - shift, center.y - shift}, radius * t,
			theme::Mix(theme::Mix(theme::Ink, theme::SurfaceRaised, .75f), theme::Text, (1 - t) * .2f), 64);
	}
	draw->AddCircle(center, radius, theme::Line, 64, scale);
	const OrbCanvas canvas{draw, center, radius, scale};
	const auto local = RelativePose({orientation, {}}, {view, {}}).orientation;
	const auto referenceLocal = RelativePose({reference, {}}, {view, {}}).orientation;
	// Yaw turns about the anchor's up axis; pitch and roll about the device's own right and forward axes.
	const std::array axes{Normalize(Rotate(referenceLocal, {0, 1, 0})), Normalize(Rotate(local, {1, 0, 0})),
						  Normalize(Rotate(local, {0, 0, -1}))};
	const std::array colors{theme::AxisY, theme::AxisX, theme::AxisZ};
	int hoveredRing = -1;
	float nearest = 7 * scale;
	const auto drawRings = [&](bool front)
	{
		for (std::size_t ring = 0; ring < axes.size(); ++ring)
		{
			const auto axis = state.dragging && state.ring == static_cast<int>(ring) ? state.axis : axes[ring];
			const auto u = Normalize(Cross(axis, std::abs(axis.y) < .9f ? XrVector3f{0, 1, 0} : XrVector3f{1, 0, 0}));
			const auto w = Cross(axis, u);
			const bool active = state.dragging && state.ring == static_cast<int>(ring);
			ImVec2 previous{};
			for (int step = 0; step <= 64; ++step)
			{
				const float angle = step * 2 * std::numbers::pi_v<float> / 64;
				const XrVector3f onRing{(u.x * std::cos(angle) + w.x * std::sin(angle)) * .94f,
										(u.y * std::cos(angle) + w.y * std::sin(angle)) * .94f,
										(u.z * std::cos(angle) + w.z * std::sin(angle)) * .94f};
				const auto screen = canvas.Point(onRing);
				// The far half is dashed and faint so the near half reads as in front.
				if (step && (onRing.z >= 0) == front && (front || active || step % 2))
					draw->AddLine(previous, screen, front ? colors[ring] : theme::Fade(colors[ring], .35f),
								  (active  ? 4.0f
								   : front ? 2.5f
										   : 1.5f) *
									  scale);
				if (front && onRing.z >= 0)
				{
					const float gap = std::hypot(screen.x - mouse.x, screen.y - mouse.y);
					if (gap < nearest)
					{
						nearest = gap;
						hoveredRing = static_cast<int>(ring);
					}
				}
				previous = screen;
			}
		}
	};
	drawRings(false);
	DrawModel(canvas, model, local, color);
	drawRings(true);
	const auto forward = axes[2];
	const ImVec2 nose{center.x + forward.x * radius * .75f, center.y - forward.y * radius * .75f};
	const bool onNose = std::hypot(mouse.x - nose.x, mouse.y - nose.y) < 9 * scale;
	draw->AddCircleFilled(nose, 5 * scale, forward.z < 0 ? theme::Fade(theme::AxisZ, .45f) : theme::AxisZ);
	if (hovered && !state.dragging)
	{
		if (onNose)
			ImGui::SetTooltip("Turn this device toward the rig camera");
		else if (hoveredRing >= 0)
			ImGui::SetTooltip("%s", hoveredRing == 0   ? "Drag to turn (yaw). Ctrl snaps to 15 degrees"
									: hoveredRing == 1 ? "Drag to tilt (pitch). Ctrl snaps to 15 degrees"
													   : "Drag to roll. Ctrl snaps to 15 degrees");
		else if (distance <= 1)
			ImGui::SetTooltip("Drag to rotate. The wheel rolls.\nDouble-click resets. Ctrl snaps.");
	}
	bool changed = false;
	if (enabled && hovered && distance <= 1 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		orientation = reference;
		state.dragging = false;
		ImGui::PopID();
		return {true, true, false, true};
	}
	if (const float wheel = ImGui::GetIO().MouseWheel; enabled && hovered && wheel != 0 && !state.dragging)
	{
		// 5 degrees a notch about the device's own pointing axis, 15 with Ctrl.
		const float step = (ImGui::GetIO().KeyCtrl ? 15 : 5) / s_degrees;
		orientation = Multiply(orientation, AxisAngle({0, 0, 1}, wheel * step));
		ImGui::PopID();
		return {true, true, false, true};
	}
	if (enabled && hovered && distance <= 1.05f && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (onNose)
		{
			const auto delta = RotationBetween(forward, {0, 0, 1});
			orientation = Multiply(view, Multiply(delta, local));
			ImGui::PopID();
			return {true, true, false, true};
		}
		state.start = point;
		state.orientation = orientation;
		state.view = view;
		state.dragging = true;
		state.ring = hoveredRing;
		state.axis = hoveredRing >= 0 ? axes[hoveredRing] : XrVector3f{};
	}
	if (state.dragging && enabled && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		XrQuaternionf delta;
		const bool snap = ImGui::GetIO().KeyCtrl;
		if (state.ring >= 0)
		{
			float angle = AngleAbout(state.axis, state.start, point);
			if (snap)
				angle = std::round(angle / (std::numbers::pi_v<float> / 12)) * (std::numbers::pi_v<float> / 12);
			delta = AxisAngle(state.axis, angle);
		}
		else
		{
			delta = ArcballRotation(state.start, point);
			if (snap)
				delta = SnapRotation(delta, std::numbers::pi_v<float> / 12);
		}
		const auto start = RelativePose({state.orientation, {}}, {view, {}}).orientation;
		orientation = Multiply(view, Multiply(delta, start));
		changed = true;
	}
	const bool released = state.dragging && (!enabled || !ImGui::IsMouseDown(ImGuiMouseButton_Left));
	if (released)
		state.dragging = false;
	auto angles = YawPitchRoll(RelativePose({orientation, {}}, {reference, {}}).orientation);
	angles = {angles.x * s_degrees, angles.y * s_degrees, angles.z * s_degrees};
	const bool row = width >= 270 * scale;
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float field = row ? (width - 2 * spacing) / 3 : std::max(size, width);
	ImGui::BeginDisabled(!enabled);
	bool fieldEdited = false, fieldActive = false, fieldFinished = false;
	const std::array<std::pair<const char*, const char*>, 3> labels{
		std::pair{"##Yaw", "Yaw %.1f°"}, {"##Pitch", "Pitch %.1f°"}, {"##Roll", "Roll %.1f°"}};
	constexpr ImU32 angleColors[] = {theme::AxisTextY, theme::AxisTextX, theme::AxisTextZ};
	for (std::size_t index = 0; index < labels.size(); ++index)
	{
		if (row && index)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(field);
		float* value = index == 0 ? &angles.x : index == 1 ? &angles.y : &angles.z;
		ImGui::PushStyleColor(ImGuiCol_Text, angleColors[index]);
		fieldEdited |=
			ImGui::DragFloat(labels[index].first, value, .5f, index == 1 ? -89.9f : -180.0f,
							 index == 1 ? 89.9f : 180.0f, labels[index].second, ImGuiSliderFlags_AlwaysClamp);
		ImGui::PopStyleColor();
		fieldActive |= ImGui::IsItemActive();
		if (ImGui::IsItemActive())
			state.activeField = ImGui::GetItemID();
		fieldFinished |= ImGui::IsItemDeactivatedAfterEdit();
		const float limit = index == 1 ? 89.9f : 180.0f;
		if (FieldActions(labels[index].first, NumberValue(*value, 0, 1, -limit, limit, "%.1f")))
		{
			fieldEdited = true;
			fieldFinished = true;
		}
		ImGui::SetItemTooltip("%s\nShift+right-click resets. Wheel adjusts, Shift faster, Ctrl slower.",
							  index == 0   ? "Turn left or right relative to what it follows"
							  : index == 1 ? "Tilt up or down"
										   : "Roll around its pointing axis");
	}
	ImGui::EndDisabled();
	if (fieldEdited)
	{
		orientation =
			Multiply(reference, EulerAngles(angles.x / s_degrees, angles.y / s_degrees, angles.z / s_degrees));
		changed = true;
	}
	ImGui::PopID();
	return {changed, released || fieldFinished, state.dragging || fieldActive, hovered && distance <= 1};
}
} // namespace simxr::app
