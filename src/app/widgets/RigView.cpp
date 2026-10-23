#include "Math.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"
#include "app/widgets/SpatialWidgets.h"
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

namespace simxr::app
{
namespace
{
constexpr RigViewState s_defaultView{};
// Shown when the displayed image carries no eye metadata yet.
constexpr XrFovf s_defaultFov{-.8f, .8f, .8f, -.8f};
// Lines are cut at this distance in front of the camera; nearer points would project far off the view.
constexpr float s_near = .05f;
// Orbiting a single device starts closer than orbiting the whole player.
constexpr float s_deviceDistance = 1.2f;
float DefaultDistance(int focus)
{
	return focus >= 0 ? s_deviceDistance : s_defaultView.distance;
}
float Yaw(XrQuaternionf q)
{
	const auto forward = Rotate(q, {0, 0, -1});
	return std::atan2(-forward.x, -forward.z);
}
} // namespace
FrameView RigCamera(const RigViewState& state, const SpatialScene& scene, float aspect, float header)
{
	if (state.eye && scene.eye)
		return *scene.eye;
	const auto rotation = EulerAngles(Yaw(scene.root.orientation) + state.yaw, state.pitch, 0);
	const auto offset = Rotate(rotation, {0, 0, state.distance});
	// The player is centred in the area below the view buttons; the buttons get the extra field above.
	const float extent = std::tan(.55f), below = 1 - std::clamp(header, 0.0f, .5f);
	const float horizontal = std::atan(extent * aspect / below);
	const float up = std::atan(extent * (1 + 2 * (1 - below) / below));
	const bool device = state.focus >= 0 && state.focus < 7 && scene.available[state.focus];
	const auto target = device ? scene.devices[state.focus].position
							   : Compose(scene.root, {{0, 0, 0, 1}, {0, .9f, 0}}).position;
	return {{rotation,
			 {target.x + state.pan.x + offset.x, target.y + state.pan.y + offset.y, target.z + state.pan.z + offset.z}},
			{-horizontal, horizontal, up, -.55f}};
}
SpatialEditResult RigView(const char* id, RigViewState& state, SpatialScene& scene, int& selected,
						  const std::array<bool, 7>& editable, ImVec2 size, const SpatialScene* ghost)
{
	const float scale = ImGui::GetFontSize() / 16;
	size = {size.x * scale, size.y * scale};
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(minimum, maximum, theme::Bg, theme::Radius * scale);
	state.minimum = minimum;
	state.size = size;
	state.points = {};
	ImGui::PushID(id);
	// A hand or tracker page orbits that device; a new focus starts from its own distance.
	const int focus = selected >= 1 && selected < 7 ? selected : -1;
	if (focus != state.focus)
	{
		state.focus = focus;
		state.pan = {};
		state.distance = DefaultDistance(focus);
	}
	// Each view button also re-centres and resets the zoom, so Back is the way home.
	const auto view = [&](float yaw, float pitch)
	{
		state.eye = false;
		state.yaw = yaw;
		state.pitch = pitch;
		state.pan = {};
		state.distance = DefaultDistance(state.focus);
	};
	// One view menu at the top left and Expand at the top right keep the view itself clear.
	constexpr float Pi = std::numbers::pi_v<float>;
	const auto near = [](float a, float b)
	{
		return std::abs(a - b) < .01f;
	};
	const char* current = state.eye								  ? "Eye"
						  : near(state.pitch, -1.56f) && near(state.yaw, 0) ? "Top"
						  : near(state.pitch, -.1f) && near(state.yaw, Pi / 2) ? "Side"
						  : near(state.pitch, s_defaultView.pitch) && near(state.yaw, 0) ? "Back"
																					  : "Free";
	const float height = ImGui::GetFrameHeight(), padding = ImGui::GetStyle().FramePadding.x;
	const float textWidth = ImGui::CalcTextSize(current).x;
	ImGui::SetCursorScreenPos({minimum.x + 5 * scale, minimum.y + 5 * scale});
	if (ImGui::Button("##View", {textWidth + padding * 2 + height * .6f, height}))
		ImGui::OpenPopup("Rig views");
	ImGui::SetItemTooltip("Camera view; double-click the view for Back");
	{
		auto* header = ImGui::GetWindowDrawList();
		const auto button = ImGui::GetItemRectMin();
		header->AddText({button.x + padding, button.y + (height - ImGui::GetFontSize()) / 2}, theme::Text, current);
		DrawIcon(header, Icon::ChevronDown, {button.x + padding + textWidth + height * .35f, button.y + height / 2},
				 height * .5f, theme::TextDim);
	}
	if (ImGui::BeginPopup("Rig views"))
	{
		if (ImGui::MenuItem("Back", nullptr, current == std::string_view("Back")))
			view(0, s_defaultView.pitch);
		ImGui::SetItemTooltip("Behind the player");
		if (ImGui::MenuItem("Top", nullptr, current == std::string_view("Top")))
			view(0, -1.56f);
		ImGui::SetItemTooltip("Look down on the player");
		if (ImGui::MenuItem("Side", nullptr, current == std::string_view("Side")))
			view(Pi / 2, -.1f);
		ImGui::SetItemTooltip("Look from the player's right");
		if (ImGui::MenuItem("Eye", nullptr, state.eye, scene.eye.has_value()))
			state.eye = true;
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s",
							  scene.eye ? "Through the app's left eye" : "The displayed image has no eye metadata.");
		ImGui::EndPopup();
	}
	ImGui::SetCursorScreenPos({maximum.x - height - 5 * scale, minimum.y + 5 * scale});
	if (IconButton("Expand", Icon::Fullscreen,
				   state.expanded ? "Back to the inspector" : "Show the rig in the image area", state.expanded))
		state.expanded = !state.expanded;
	ImGui::SetCursorScreenPos({minimum.x + 5 * scale, maximum.y - ImGui::GetFrameHeight() - 4 * scale});
	if (ImGui::SmallButton("?##Rig help"))
		ImGui::OpenPopup("Rig help");
	ImGui::SetItemTooltip("Camera controls, device handles and undo");
	if (ImGui::BeginPopup("Rig help"))
	{
		ImGui::TextUnformatted("Placing devices");
		ImGui::TextDisabled("Drag empty space to orbit; middle-drag pans; wheel zooms.");
		ImGui::TextDisabled("The view menu resets the camera; double-click is Back.");
		ImGui::TextDisabled("Select a device, then drag its handles. Ctrl snaps.");
		ImGui::TextDisabled("Ctrl+Z undoes. Camera moves never change devices.");
		ImGui::EndPopup();
	}
	const bool headerHovered = ImGui::IsAnyItemHovered();
	const auto camera = RigCamera(state, scene, size.x / size.y, 30 * scale / size.y);
	const float tanLeft = std::tan(camera.fov.angleLeft), tanRight = std::tan(camera.fov.angleRight);
	const float tanUp = std::tan(camera.fov.angleUp), tanDown = std::tan(camera.fov.angleDown);
	const float focal = size.x / (tanRight - tanLeft);
	const auto toCamera = [&](XrVector3f world)
	{
		return RelativePose({{0, 0, 0, 1}, world}, camera.pose).position;
	};
	// A camera-space point in front of the near plane, on screen.
	const auto screen = [&](XrVector3f local)
	{
		const float depth = -local.z;
		return ImVec2{minimum.x + (local.x / depth - tanLeft) / (tanRight - tanLeft) * size.x,
					  minimum.y + (tanUp - local.y / depth) / (tanUp - tanDown) * size.y};
	};
	const auto project = [&](XrVector3f world)
	{
		const auto local = toCamera(world);
		return local.z > -s_near ? std::pair{ImVec2{}, true} : std::pair{screen(local), false};
	};
	const auto line = [&](XrVector3f first, XrVector3f second, ImU32 color, float thickness = 1.0f)
	{
		auto a = toCamera(first), b = toCamera(second);
		if (a.z > -s_near && b.z > -s_near)
			return;
		// Keep only the part in front of the near plane.
		const auto cut = [](XrVector3f inside, XrVector3f outside)
		{
			const float t = (inside.z + s_near) / (inside.z - outside.z);
			return XrVector3f{inside.x + (outside.x - inside.x) * t, inside.y + (outside.y - inside.y) * t, -s_near};
		};
		if (a.z > -s_near)
			a = cut(b, a);
		else if (b.z > -s_near)
			b = cut(a, b);
		draw->AddLine(screen(a), screen(b), color, thickness * scale);
	};
	draw->PushClipRect({minimum.x, minimum.y + 30 * scale}, maximum, true);
	const auto root = scene.root.position;
	for (int cell = -3; cell <= 3; ++cell)
	{
		const float fade = 1 - std::abs(cell) / 4.0f;
		line({root.x + cell, root.y, root.z - 3}, {root.x + cell, root.y, root.z + 3}, theme::Fade(theme::Line, fade));
		line({root.x - 3, root.y, root.z + cell}, {root.x + 3, root.y, root.z + cell}, theme::Fade(theme::Line, fade));
	}
	line({-.1f, 0, 0}, {.1f, 0, 0}, theme::TextDim);
	line({0, 0, -.1f}, {0, 0, .1f}, theme::TextDim);
	line({}, {.3f, 0, 0}, theme::AxisX, 2);
	line({}, {0, .3f, 0}, theme::AxisY, 2);
	line({}, {0, 0, -.3f}, theme::AxisZ, 2);
	{
		// Facing arrow on the floor at the root.
		const auto at = [&](float x, float z)
		{
			return Compose(scene.root, {{0, 0, 0, 1}, {x, 0, z}}).position;
		};
		const auto color = theme::Fade(theme::TextDim, .8f);
		line(at(0, 0), at(0, -.45f), color, 1.5f);
		line(at(0, -.45f), at(-.08f, -.33f), color, 1.5f);
		line(at(0, -.45f), at(.08f, -.33f), color, 1.5f);
	}
	// A hand page dims the body so the hands stand out.
	const bool handPage = selected == 1 || selected == 2;
	const auto drawController = [&](XrPosef pose, ImU32 color, float alpha)
	{
		// Through the eye it is true to scale; from outside it grows to stay readable, within limits.
		const auto centre = toCamera(pose.position);
		if (centre.z > -s_near)
			return;
		const float perMeter = focal / -centre.z;
		const float glyph = state.eye ? 1.0f : std::clamp(20 * scale / (perMeter * .12f), 1.0f, 4.0f);
		const auto local = [&](XrVector3f offset)
		{
			return toCamera(Compose(pose, {{0, 0, 0, 1}, {offset.x * glyph, offset.y * glyph, offset.z * glyph}}).position);
		};
		const auto bottom = local({0, -.035f, .05f}), top = local({0, .005f, -.03f});
		std::array<XrVector3f, 24> ring;
		for (std::size_t step = 0; step < ring.size(); ++step)
		{
			const float angle = step * 2 * std::numbers::pi_v<float> / ring.size();
			const float c = std::cos(angle) * .045f, s = std::sin(angle) * .045f;
			ring[step] = local({c, .015f - .866f * s, -.065f - .5f * s});
		}
		const bool clipped = bottom.z > -s_near || top.z > -s_near ||
							 std::ranges::any_of(ring, [](XrVector3f point) { return point.z > -s_near; });
		// Each end is as thick as its own depth makes it, so the near end reads as near.
		const auto radiusAt = [&](XrVector3f point)
		{
			return std::max(2.5f * scale, .015f * glyph * focal / -point.z);
		};
		const auto ink = theme::Fade(theme::Ink, alpha);
		if (!clipped)
		{
			const auto a = screen(bottom), b = screen(top);
			const float ra = radiusAt(bottom), rb = radiusAt(top);
			const float length = std::max(.001f, std::hypot(b.x - a.x, b.y - a.y));
			const ImVec2 across{-(b.y - a.y) / length, (b.x - a.x) / length};
			const auto capsule = [&](float grow, ImU32 fill)
			{
				draw->AddQuadFilled({a.x + across.x * (ra + grow), a.y + across.y * (ra + grow)},
									{b.x + across.x * (rb + grow), b.y + across.y * (rb + grow)},
									{b.x - across.x * (rb + grow), b.y - across.y * (rb + grow)},
									{a.x - across.x * (ra + grow), a.y - across.y * (ra + grow)}, fill);
				draw->AddCircleFilled(a, ra + grow, fill);
				draw->AddCircleFilled(b, rb + grow, fill);
			};
			capsule(scale, ink);
			capsule(0, color);
			std::array<ImVec2, 24> points;
			float depth = 0;
			for (std::size_t step = 0; step < ring.size(); ++step)
			{
				points[step] = screen(ring[step]);
				depth += -ring[step].z / ring.size();
			}
			const float thickness = std::clamp(.01f * glyph * focal / depth, 1.5f * scale, 8 * scale);
			draw->AddPolyline(points.data(), static_cast<int>(points.size()), ink, ImDrawFlags_Closed,
							  thickness + 2 * scale);
			draw->AddPolyline(points.data(), static_cast<int>(points.size()), color, ImDrawFlags_Closed, thickness);
		}
		line(Compose(pose, {{0, 0, 0, 1}, {0, 0, -.08f * glyph}}).position,
			 Compose(pose, {{0, 0, 0, 1}, {0, 0, -.3f * glyph}}).position, theme::Fade(color, .8f), 1.5f);
	};
	const auto renderScene = [&](const SpatialScene& value, float alpha)
	{
		const float bodyAlpha = alpha * (handPage ? .4f : .9f);
		for (std::size_t joint = 0; joint < BodyJointCount; ++joint)
		{
			const auto& sample = value.body.joints[joint];
			if (sample.flags && value.body.skeleton[joint].parent)
				line(sample.pose.position,
					 value.body.joints[static_cast<std::size_t>(*value.body.skeleton[joint].parent)].pose.position,
					 theme::Fade(theme::TextDim, bodyAlpha), 1.5f);
		}
		const auto renderDevice = [&](int device)
		{
			if (!value.available[device])
				return;
			const auto pose = value.devices[device];
			const auto color = DeviceColor(device, alpha);
			const auto [position, behind] = project(pose.position);
			if (behind)
				return;
			if (alpha == 1)
				state.points[device] = position;
			if (device == 0)
			{
				const auto fov = value.eye ? value.eye->fov : s_defaultFov;
				const std::array corners{
					XrVector3f{std::tan(fov.angleLeft) * .2f, std::tan(fov.angleUp) * .2f, -.2f},
					XrVector3f{std::tan(fov.angleRight) * .2f, std::tan(fov.angleUp) * .2f, -.2f},
					XrVector3f{std::tan(fov.angleRight) * .2f, std::tan(fov.angleDown) * .2f, -.2f},
					XrVector3f{std::tan(fov.angleLeft) * .2f, std::tan(fov.angleDown) * .2f, -.2f}};
				for (int corner = 0; corner < 4; ++corner)
				{
					const auto end = Compose(pose, {{0, 0, 0, 1}, corners[corner]}).position;
					line(pose.position, end, color, 1.5f);
					line(end, Compose(pose, {{0, 0, 0, 1}, corners[(corner + 1) % 4]}).position, color, 1.5f);
				}
			}
			else if (device == 1 || device == 2)
				drawController(pose, color, alpha);
			else
			{
				const float radius = 5 * scale;
				draw->AddCircleFilled(position, radius + scale, theme::Fade(theme::Ink, alpha));
				draw->AddCircleFilled(position, radius, color);
				const char label[2] = {value.labels[device], 0};
				draw->AddText({position.x + radius + 2 * scale, position.y - ImGui::GetFontSize() * .5f},
							  theme::Fade(theme::Head, alpha * .9f), label);
			}
			if (device == selected)
			{
				draw->AddCircle(position, 12 * scale, color, 32, 2 * scale);
				if (device == 1 || device == 2)
				{
					const char label[2] = {value.labels[device], 0};
					draw->AddText({position.x + 14 * scale, position.y - 14 * scale - ImGui::GetFontSize()}, color,
								  label);
				}
			}
		};
		for (int device = 0; device < 7; ++device)
			if (device != selected)
				renderDevice(device);
		if (selected >= 0)
			renderDevice(selected);
		if (value.gaze)
			line(value.gaze->position, Compose(*value.gaze, {{0, 0, 0, 1}, {0, 0, -1}}).position,
				 theme::Fade(theme::Warn, alpha * .7f));
	};
	if (ghost)
		renderScene(*ghost, .3f);
	renderScene(scene, 1);
	SpatialEditResult result;
	if (selected >= 0)
	{
		// A smaller gizmo than in the preview so it does not cover the player.
		ImGuizmo::SetGizmoSizeClipSpace(.07f);
		result =
			DrawPoseGizmo("Pose", state.gizmo, camera, minimum, maximum, scene.devices[selected], editable[selected]);
		ImGuizmo::SetGizmoSizeClipSpace(.1f);
	}
	// Dragging a tracker by its centre slides it onto the nearest joint.
	if (selected >= 3 && (result.active || result.finished) && state.gizmo.screenMove)
	{
		const auto mouse = ImGui::GetIO().MousePos;
		float best = 1e9f;
		if (result.active)
			for (const auto joint : scene.attachable)
			{
				const auto [point, behind] = project(scene.body.joints[static_cast<std::size_t>(joint)].pose.position);
				const float distance = std::hypot(point.x - mouse.x, point.y - mouse.y);
				if (!behind && distance < best)
				{
					best = distance;
					state.joint = static_cast<int>(joint);
				}
			}
		result.joint = state.joint;
		if (state.joint >= 0)
		{
			const auto [point, behind] =
				project(scene.body.joints[static_cast<std::size_t>(state.joint)].pose.position);
			if (!behind)
				draw->AddCircle(point, 9 * scale, theme::Accent, 24, 2.5f * scale);
		}
		if (result.finished)
			state.joint = -1;
	}
	draw->PopClipRect();
	ImGui::SetCursorScreenPos(minimum);
	ImGui::BeginDisabled(headerHovered || result.hovered || result.active);
	ImGui::InvisibleButton("Orbit", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	// The wheel zooms here instead of scrolling the page around the view.
	if (hovered)
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
	ImGui::EndDisabled();
	if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		view(0, s_defaultView.pitch);
	const auto& input = ImGui::GetIO();
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		bool hit = false;
		for (int device = 0; device < 7; ++device)
		{
			const auto [point, behind] = project(scene.devices[device].position);
			if (scene.available[device] && !behind &&
				std::hypot(point.x - input.MousePos.x, point.y - input.MousePos.y) <= 12 * scale)
			{
				selected = device;
				hit = true;
			}
		}
		state.orbiting = !hit;
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		state.panning = true;
	state.orbiting &= ImGui::IsMouseDown(ImGuiMouseButton_Left);
	state.panning &= ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	if (state.orbiting)
	{
		state.eye = false;
		state.yaw -= input.MouseDelta.x * .01f;
		state.pitch = std::clamp(state.pitch - input.MouseDelta.y * .01f, -1.56f, 1.56f);
	}
	if (state.panning)
	{
		state.eye = false;
		const auto delta = Rotate(camera.pose.orientation, {-input.MouseDelta.x * state.distance * .001f,
															input.MouseDelta.y * state.distance * .001f, 0});
		state.pan = {state.pan.x + delta.x, state.pan.y + delta.y, state.pan.z + delta.z};
	}
	if (hovered && input.MouseWheel)
	{
		state.eye = false;
		state.distance = std::clamp(state.distance * std::pow(.85f, input.MouseWheel), .2f, 10.0f);
	}
	ImGui::PopID();
	return result;
}
} // namespace simxr::app
