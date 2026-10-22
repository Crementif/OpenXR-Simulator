#include "Math.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"
#include "app/widgets/SpatialWidgets.h"
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace simxr::app
{
namespace
{
constexpr RigViewState s_defaultView{};
// Shown when the displayed image carries no eye metadata yet.
constexpr XrFovf s_defaultFov{-.8f, .8f, .8f, -.8f};
float Yaw(XrQuaternionf q)
{
	const auto forward = Rotate(q, {0, 0, -1});
	return std::atan2(-forward.x, -forward.z);
}
} // namespace
FrameView RigCamera(const RigViewState& state, const SpatialScene& scene, float aspect, float header)
{
	if (state.eye && scene.eye)
		return {scene.devices[0], scene.eye->fov};
	const auto rotation = EulerAngles(Yaw(scene.root.orientation) + state.yaw, state.pitch, 0);
	const auto offset = Rotate(rotation, {0, 0, state.distance});
	// The player is centred in the area below the view buttons; the buttons get the extra field above.
	const float extent = std::tan(.55f), below = 1 - std::clamp(header, 0.0f, .5f);
	const float horizontal = std::atan(extent * aspect / below);
	const float up = std::atan(extent * (1 + 2 * (1 - below) / below));
	const auto target = Compose(scene.root, {{0, 0, 0, 1}, {0, .9f, 0}}).position;
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
	ImGui::SetCursorScreenPos({minimum.x + 5 * scale, minimum.y + 5 * scale});
	if (ImGui::SmallButton("Top"))
	{
		state.eye = false;
		state.pitch = -1.56f;
		state.yaw = 0;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Side"))
	{
		state.eye = false;
		state.pitch = -.1f;
		state.yaw = std::numbers::pi_v<float> / 2;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Back"))
	{
		state.eye = false;
		state.pitch = s_defaultView.pitch;
		state.yaw = 0;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!scene.eye);
	if (ImGui::SmallButton("Eye"))
		state.eye = true;
	ImGui::EndDisabled();
	if (!scene.eye && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("The displayed image has no eye metadata.");
	ImGui::SameLine();
	if (ImGui::SmallButton(state.expanded ? "Restore" : "Expand"))
		state.expanded = !state.expanded;
	{
		// Bottom-right corner, clear of the view buttons at any width.
		const float side = ImGui::GetFrameHeight() - 2 * ImGui::GetStyle().FramePadding.y;
		const ImVec2 position{maximum.x - side - 10 * scale, maximum.y - side - 4 * scale};
		ImGui::SetCursorScreenPos(position);
		if (ImGui::InvisibleButton("Reset view", {side + 6 * scale, side}))
		{
			const bool expanded = state.expanded;
			const auto gizmo = state.gizmo;
			state = s_defaultView;
			state.expanded = expanded;
			state.gizmo = gizmo;
		}
		const bool hovered = ImGui::IsItemHovered();
		if (hovered)
			draw->AddRectFilled(position, {position.x + side + 6 * scale, position.y + side},
								ImGui::GetColorU32(ImGuiCol_ButtonHovered), theme::Radius * scale);
		DrawIcon(draw, Icon::Refresh, {position.x + side / 2 + 3 * scale, position.y + side / 2}, side * .8f,
				 hovered ? theme::Text : theme::TextDim);
		ImGui::SetItemTooltip("Reset view: behind the player");
	}
	ImGui::SetCursorScreenPos({minimum.x + 5 * scale, maximum.y - ImGui::GetFrameHeight() - 4 * scale});
	if (ImGui::SmallButton("?##Rig help"))
		ImGui::OpenPopup("Rig help");
	ImGui::SetItemTooltip("Camera controls, device handles and undo");
	if (ImGui::BeginPopup("Rig help"))
	{
		ImGui::TextUnformatted("Placing devices");
		ImGui::TextDisabled("Drag empty space to orbit; middle-drag pans.");
		ImGui::TextDisabled("Wheel zooms. Top / Side / Back / Eye set the view.");
		ImGui::TextDisabled("Select a device, then drag its handles. Ctrl snaps.");
		ImGui::TextDisabled("Ctrl+Z undoes. Reset view only moves the camera.");
		ImGui::EndPopup();
	}
	const bool headerHovered = ImGui::IsAnyItemHovered();
	const auto camera = RigCamera(state, scene, size.x / size.y, 30 * scale / size.y);
	const auto project = [&](XrVector3f world)
	{
		const auto point = ProjectPoint(camera, world);
		return std::pair{ImVec2{minimum.x + point.uv.x * size.x, minimum.y + point.uv.y * size.y}, point.behind};
	};
	const auto line = [&](XrVector3f first, XrVector3f second, ImU32 color, float thickness = 1.0f)
	{
		const auto [a, aBehind] = project(first);
		const auto [b, bBehind] = project(second);
		if (!aBehind && !bBehind)
			draw->AddLine(a, b, color, thickness * scale);
	};
	// Pixel size of a world length at a point, for glyphs that scale with distance.
	const auto pixels = [&](XrVector3f at, float length)
	{
		const auto right = Rotate(camera.pose.orientation, {length, 0, 0});
		const auto [a, aBehind] = project(at);
		const auto [b, bBehind] = project({at.x + right.x, at.y + right.y, at.z + right.z});
		return aBehind || bBehind ? 0.0f : std::hypot(b.x - a.x, b.y - a.y);
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
	const auto drawController = [&](XrPosef pose, ImU32 color, float alpha, bool highlight)
	{
		// At least 20 px long so a far or small view still shows the controller clearly.
		const float perMeter = pixels(pose.position, 1);
		const float glyph = std::max(1.6f, 20 * scale / std::max(perMeter * .12f, .001f));
		const auto point = [&](XrVector3f local)
		{
			return project(Compose(pose, {{0, 0, 0, 1}, {local.x * glyph, local.y * glyph, local.z * glyph}}).position);
		};
		const auto [bottom, bottomBehind] = point({0, -.035f, .05f});
		const auto [top, topBehind] = point({0, .005f, -.03f});
		if (bottomBehind || topBehind)
			return;
		const float radius = std::max(3.5f * scale, perMeter * .03f * glyph);
		const auto ink = theme::Fade(theme::Ink, alpha);
		draw->AddLine(bottom, top, ink, radius * 2 + 2 * scale);
		draw->AddCircleFilled(bottom, radius + scale, ink);
		draw->AddCircleFilled(top, radius + scale, ink);
		draw->AddLine(bottom, top, color, radius * 2);
		draw->AddCircleFilled(bottom, radius, color);
		draw->AddCircleFilled(top, radius, color);
		std::array<ImVec2, 24> ring;
		for (std::size_t step = 0; step < ring.size(); ++step)
		{
			const float angle = step * 2 * std::numbers::pi_v<float> / ring.size();
			const float c = std::cos(angle) * .045f, s = std::sin(angle) * .045f;
			ring[step] = point({c, .015f - .866f * s, -.065f - .5f * s}).first;
		}
		draw->AddPolyline(ring.data(), static_cast<int>(ring.size()), ink, ImDrawFlags_Closed, radius + 3 * scale);
		draw->AddPolyline(ring.data(), static_cast<int>(ring.size()), color, ImDrawFlags_Closed, radius + scale);
		line(Compose(pose, {{0, 0, 0, 1}, {0, 0, -.08f * glyph}}).position,
			 Compose(pose, {{0, 0, 0, 1}, {0, 0, -.3f * glyph}}).position, theme::Fade(color, .8f), 1.5f);
		if (highlight)
			draw->AddCircle(top, radius * 2 + 10 * scale, color, 32, 2 * scale);
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
				drawController(pose, color, alpha, false);
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
	ImGui::EndDisabled();
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
		state.distance = std::clamp(state.distance * std::pow(.85f, input.MouseWheel), .5f, 10.0f);
	}
	ImGui::PopID();
	return result;
}
} // namespace simxr::app
