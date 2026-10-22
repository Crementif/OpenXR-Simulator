#pragma once
#include "app/SpatialMath.h"
#include "simulation/TrackingTypes.h"
#include <imgui.h>
#include <optional>
#include <span>

namespace simxr::app
{
struct SpatialEditResult
{
	bool changed = false, finished = false, active = false, hovered = false;
	// A tracker dragged by its centre in the rig view: the joint it snaps to, or -1.
	int joint = -1;
};
struct GizmoState
{
	int operation = 2, id = 0;
	bool local = false, usingGizmo = false, screenMove = false;
	XrPosef start{};
};
void CancelPoseGizmo(GizmoState& state);
SpatialEditResult DrawPoseGizmo(const char* id, GizmoState& state, const FrameView& camera, ImVec2 minimum,
								ImVec2 maximum, XrPosef& pose, bool enabled);
struct ArcballState
{
	XrVector2f start{};
	XrQuaternionf orientation{0, 0, 0, 1}, view{0, 0, 0, 1};
	bool dragging = false;
	// -1 drags the ball freely; 0, 1 and 2 turn about the yaw, pitch or roll ring only.
	int ring = -1;
	XrVector3f axis{};
};
// What the rotation orb shows inside the ball.
enum class OrbModel
{
	Headset,
	Controller,
	Puck
};
// Rotation orb: the device model inside a ball with yaw, pitch and roll rings, plus the angles relative to
// reference. Double-click resets to reference; clicking the nose points the device at the viewer.
SpatialEditResult RotationOrb(const char* id, ArcballState& state, XrQuaternionf& orientation, XrQuaternionf view,
							  XrQuaternionf reference, bool enabled, OrbModel model, ImU32 color, float width,
							  float size = 160);
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
	// Marker letters; trackers use their role letter.
	std::array<char, 7> labels{'H', 'L', 'R', 'T', 'T', 'T', 'T'};
	BodySample body;
	std::optional<XrPosef> gaze;
	std::optional<FrameView> eye;
	// Joints a dragged tracker may snap to.
	std::span<const BodyJoint> attachable;
};
struct RigViewState
{
	// Yaw is relative to the way the player faces; 0 looks over the player's shoulder (Back).
	float yaw = 0, pitch = -.2f, distance = 2.5f;
	XrVector3f pan{};
	// The device the camera orbits (a hand or tracker), or -1 for the player.
	int focus = -1;
	bool eye = false, expanded = false, orbiting = false, panning = false;
	GizmoState gizmo;
	int joint = -1;
	// Last drawn layout: the widget rectangle and each device's projected point.
	ImVec2 minimum{}, size{};
	std::array<ImVec2, 7> points{};
};
// header: the fraction of the view height covered by the view buttons.
FrameView RigCamera(const RigViewState& state, const SpatialScene& scene, float aspect, float header = 0);
SpatialEditResult RigView(const char* id, RigViewState& state, SpatialScene& scene, int& selected,
						  const std::array<bool, 7>& editable, ImVec2 size, const SpatialScene* ghost = nullptr);
ImU32 DeviceColor(int device, float alpha = 1);
} // namespace simxr::app
