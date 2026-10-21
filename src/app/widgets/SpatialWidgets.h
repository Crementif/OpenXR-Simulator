#pragma once
#include "app/SpatialMath.h"
#include "simulation/TrackingTypes.h"
#include <imgui.h>
#include <optional>

namespace simxr::app
{
struct SpatialEditResult
{
	bool changed = false, finished = false, active = false, hovered = false;
};
struct GizmoState
{
	int operation = 2, id = 0;
	bool local = false, usingGizmo = false;
	XrPosef start{};
};
void CancelPoseGizmo(GizmoState& state);
SpatialEditResult DrawPoseGizmo(const char* id, GizmoState& state, const FrameView& camera, ImVec2 minimum,
								ImVec2 maximum, XrPosef& pose, bool enabled);
struct ArcballState
{
	XrVector2f start{};
	XrQuaternionf orientation{0, 0, 0, 1}, view{0, 0, 0, 1};
	bool dragging = false, ring = false;
};
SpatialEditResult ArcballWidget(const char* id, ArcballState& state, XrQuaternionf& orientation, XrQuaternionf view,
								XrQuaternionf reset, bool enabled, float size = 160);
struct ReachMapState
{
	int hand = 1;
	bool dragging = false, height = false;
};
SpatialEditResult ReachMap(const char* id, ReachMapState& state, XrPosef root, XrPosef head, float shoulderWidth,
						   float armReach, std::array<XrPosef, 2>& hands, const std::array<bool, 2>& editable,
						   float width);
struct SpatialScene
{
	XrPosef root{};
	std::array<XrPosef, 7> devices{};
	std::array<bool, 7> available{};
	BodySample body;
	std::optional<XrPosef> gaze;
	std::optional<FrameView> eye;
};
struct RigViewState
{
	float yaw = -.5f, pitch = -.3f, distance = 4.5f;
	XrVector3f pan{};
	bool eye = false, expanded = false, orbiting = false, panning = false;
	GizmoState gizmo;
};
FrameView RigCamera(const RigViewState& state, const SpatialScene& scene, float aspect);
SpatialEditResult RigView(const char* id, RigViewState& state, SpatialScene& scene, int& selected,
						  const std::array<bool, 7>& editable, ImVec2 size, const SpatialScene* ghost = nullptr);
ImU32 DeviceColor(int device, float alpha = 1);
} // namespace simxr::app
