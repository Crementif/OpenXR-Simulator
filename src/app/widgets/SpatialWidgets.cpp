#include "app/widgets/SpatialWidgets.h"
#include "app/Theme.h"
#include "Math.h"
#include <ImGuizmo.h>
#include <cmath>
#include <numbers>

namespace simxr::app
{
ImU32 DeviceColor(int device, float alpha)
{
	const auto color = device == 1 ? theme::Left : device == 2 ? theme::Right : device == 0 ? theme::Text : theme::Head;
	return theme::Fade(color, alpha);
}
void CancelPoseGizmo(GizmoState& state)
{
	if (state.usingGizmo)
	{
		ImGuizmo::PushID(state.id);
		ImGuizmo::Enable(false);
		ImGuizmo::PopID();
		state.usingGizmo = false;
	}
}
SpatialEditResult DrawPoseGizmo(const char* id, GizmoState& state, const FrameView& camera, ImVec2 minimum,
								ImVec2 maximum, XrPosef& pose, bool enabled)
{
	state.id = static_cast<int>(ImGui::GetID(id));
	ImGuizmo::PushID(state.id);
	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetRect(minimum.x, minimum.y, maximum.x - minimum.x, maximum.y - minimum.y);
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::Enable(enabled);
	if (!enabled)
	{
		state.usingGizmo = false;
		ImGuizmo::PopID();
		return {};
	}
	const bool hovering = ImGui::IsMouseHoveringRect(minimum, maximum);
	if (hovering && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space))
		state.operation = (state.operation + 1) % 3;
	const auto operation = state.operation == 0 ? ImGuizmo::TRANSLATE
						   : state.operation == 1
							   ? ImGuizmo::ROTATE
							   : static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE | ImGuizmo::ROTATE);
	const auto view = ViewMatrix(camera.pose), projection = ProjectionMatrix(camera.fov);
	auto matrix = PoseMatrix(pose);
	if (!state.usingGizmo)
		state.start = pose;
	const bool changed = ImGuizmo::Manipulate(view.data(), projection.data(), operation,
											  state.local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, matrix.data());
	const bool active = ImGuizmo::IsUsing();
	const bool finished = state.usingGizmo && !active;
	state.usingGizmo = active;
	if (changed)
	{
		pose = MatrixPose(matrix);
		if (ImGui::GetIO().KeyCtrl)
		{
			auto delta = RelativePose(pose, state.start);
			delta.orientation = SnapRotation(delta.orientation, std::numbers::pi_v<float> / 12);
			pose.orientation = Compose(state.start, delta).orientation;
			if (!state.local)
				delta.position = {pose.position.x - state.start.position.x, pose.position.y - state.start.position.y,
								  pose.position.z - state.start.position.z};
			delta.position = {std::round(delta.position.x / .05f) * .05f, std::round(delta.position.y / .05f) * .05f,
							  std::round(delta.position.z / .05f) * .05f};
			pose.position = state.local ? Compose(state.start, delta).position
										: XrVector3f{state.start.position.x + delta.position.x,
													 state.start.position.y + delta.position.y,
													 state.start.position.z + delta.position.z};
		}
	}
	const bool over = hovering && ImGuizmo::IsOver();
	ImGuizmo::PopID();
	return {changed, finished, active, over};
}
} // namespace simxr::app
