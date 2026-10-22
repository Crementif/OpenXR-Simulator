#include "Math.h"
#include "app/Theme.h"
#include "app/widgets/SpatialWidgets.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace simxr::app
{
FrameView RigCamera(const RigViewState& state, const SpatialScene& scene, float aspect)
{
	if (state.eye && scene.eye)
		return {scene.devices[0], scene.eye->fov};
	const auto rotation = EulerAngles(state.yaw, state.pitch, 0);
	const auto offset = Rotate(rotation, {0, 0, state.distance});
	const float halfFov = std::atan(std::tan(.55f) * aspect);
	return {{rotation,
			 {scene.root.position.x + state.pan.x + offset.x, scene.root.position.y + state.pan.y + offset.y,
			  scene.root.position.z + state.pan.z + offset.z}},
			{-halfFov, halfFov, .55f, -.55f}};
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
		state.pitch = 0;
		state.yaw = std::numbers::pi_v<float> / 2;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Back"))
	{
		state.eye = false;
		state.pitch = -.2f;
		state.yaw = std::numbers::pi_v<float>;
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
	const bool headerHovered = ImGui::IsAnyItemHovered();
	const auto camera = RigCamera(state, scene, size.x / size.y);
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
	draw->PushClipRect({minimum.x, minimum.y + 30 * scale}, maximum, true);
	for (int cell = -3; cell <= 3; ++cell)
	{
		const auto root = scene.root.position;
		line({root.x + cell, root.y, root.z - 3}, {root.x + cell, root.y, root.z + 3}, theme::Fade(theme::Line, .9f));
		line({root.x - 3, root.y, root.z + cell}, {root.x + 3, root.y, root.z + cell}, theme::Fade(theme::Line, .9f));
	}
	line({-.1f, 0, 0}, {.1f, 0, 0}, theme::TextDim);
	line({0, 0, -.1f}, {0, 0, .1f}, theme::TextDim);
	line({}, {.3f, 0, 0}, theme::AxisX, 2);
	line({}, {0, .3f, 0}, theme::AxisY, 2);
	line({}, {0, 0, -.3f}, theme::AxisZ, 2);
	const auto renderScene = [&](const SpatialScene& value, float alpha)
	{
		for (std::size_t joint = 0; joint < BodyJointCount; ++joint)
		{
			const auto& sample = value.body.joints[joint];
			if (sample.flags && value.body.skeleton[joint].parent)
				line(sample.pose.position,
					 value.body.joints[static_cast<std::size_t>(*value.body.skeleton[joint].parent)].pose.position,
					 theme::Fade(theme::TextDim, alpha * .75f));
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
			if (device == 0 && value.eye)
			{
				const auto fov = value.eye->fov;
				const std::array corners{
					XrVector3f{std::tan(fov.angleLeft) * .2f, std::tan(fov.angleUp) * .2f, -.2f},
					XrVector3f{std::tan(fov.angleRight) * .2f, std::tan(fov.angleUp) * .2f, -.2f},
					XrVector3f{std::tan(fov.angleRight) * .2f, std::tan(fov.angleDown) * .2f, -.2f},
					XrVector3f{std::tan(fov.angleLeft) * .2f, std::tan(fov.angleDown) * .2f, -.2f}};
				for (int corner = 0; corner < 4; ++corner)
				{
					const auto end = Compose(pose, {{0, 0, 0, 1}, corners[corner]}).position;
					line(pose.position, end, color);
					line(end, Compose(pose, {{0, 0, 0, 1}, corners[(corner + 1) % 4]}).position, color);
				}
			}
			else if (device == 1 || device == 2)
			{
				for (int segment = 0; segment < 12; ++segment)
				{
					const auto capsule = [&](int step)
					{
						const float angle = step * std::numbers::pi_v<float> / 6;
						return Compose(pose, {{0, 0, 0, 1}, {.045f * std::cos(angle), 0, .10f * std::sin(angle)}})
							.position;
					};
					line(capsule(segment), capsule(segment + 1), color, 2);
				}
				line(pose.position, Compose(pose, {{0, 0, 0, 1}, {0, 0, -.3f}}).position, color, 2);
			}
			else if (device > 2)
				draw->AddRect({position.x - 5 * scale, position.y - 5 * scale},
							  {position.x + 5 * scale, position.y + 5 * scale}, color);
			else
				draw->AddCircle(position, 6 * scale, color);
			if (device == selected)
				draw->AddCircle(position, 12 * scale, color, 32, 2 * scale);
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
		result =
			DrawPoseGizmo("Pose", state.gizmo, camera, minimum, maximum, scene.devices[selected], editable[selected]);
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
