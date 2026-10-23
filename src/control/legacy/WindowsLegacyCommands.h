#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace mcp
{
constexpr float kHoldAngleUnset = 100.0f;

struct HeadPoseCommand
{
	bool valid = false;
	float x = 0.0f;
	float y = 1.7f;
	float z = 0.0f;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float roll = 0.0f;
	bool hasRoll = false; // False preserves the current roll.
};

struct FovCommand
{
	bool valid = false;
	bool clear = false;				   // {"clear": true} reverts to the symmetric UI default
	std::array<float, 2> angleLeft{};  // radians, < 0
	std::array<float, 2> angleRight{}; // radians, > 0
	std::array<float, 2> angleUp{};	   // radians, > 0
	std::array<float, 2> angleDown{};  // radians, < 0
};

struct IpdCommand
{
	bool valid = false;
	bool clear = false;
	float ipdMeters = 0.064f;
};

struct HeadsetProfileCommand
{
	bool valid = false;
	char name[32] = {};
};

struct PoseSweepCommand
{
	bool valid = false;
	bool enabled = false;
	float yawAmpDeg = 30.0f;
	float pitchAmpDeg = 15.0f;
	float rollAmpDeg = 15.0f;
	float freqHz = 0.25f;
};

struct AnaglyphCommand
{
	bool valid = false;
	bool enabled = false;
};

struct ProjLogEntry
{
	std::uint64_t frame = 0;
	// Pose embedded in projection-layer view 0 (left eye).
	float poseQx = 0;
	float poseQy = 0;
	float poseQz = 0;
	float poseQw = 1;
	float posX = 0;
	float posY = 0;
	float posZ = 0;
	// FOV per eye (radians, OpenXR convention)
	std::array<float, 2> aL{};
	std::array<float, 2> aR{};
	std::array<float, 2> aU{};
	std::array<float, 2> aD{};
	// Image sub-rect per eye (left/right): offset_x, offset_y, extent_w, extent_h
	std::array<std::int32_t, 2> rectX{};
	std::array<std::int32_t, 2> rectY{};
	std::array<std::int32_t, 2> rectW{};
	std::array<std::int32_t, 2> rectH{};
};

struct BurstCommand
{
	bool valid = false;
	int frames = 16;
	HeadPoseCommand pose; // pose.valid == step the head pose at burst start
};

struct BurstFrameMeta
{
	std::uint64_t frame = 0;
	float headYaw = 0;
	float headPitch = 0;
	float headRoll = 0;
	float headX = 0;
	float headY = 0;
	float headZ = 0;
	ProjLogEntry proj; // app-submitted pose/FOV for this frame
	double tMs = 0;
};

struct ControllerPoseCommand
{
	bool valid = false;
	int hand = 1;	   // 0=left, 1=right
	float posX = 0.2f; // Position offset from head (head-local space)
	float posY = -0.3f;
	float posZ = -0.4f;
	float yaw = 0.0f;	  // Yaw offset relative to head
	float pitch = -0.3f;  // Pitch offset relative to head
	float trigger = 0.0f; // 0.0-1.0 trigger value
	bool triggerSet = false;
	int buttonA = -1; // -1=unchanged, 0=released, 1=pressed
	// 0 jumps to the pose. Anything else moves the hand there over that many milliseconds,
	// which is what gives an app a hand VELOCITY to see: a jump is one frame of it, too
	// short for a gesture that needs speed and distance at the same moment.
	float durationMs = 0.0f;
};

struct ControllerHoldCommand
{
	bool valid = false;
	bool clear = false;
	int hand = -1; // 0 = left, 1 = right, -1 = both
	float trigger = -1.0f;
	float grip = -1.0f;
	float primary = -1.0f;	 // A on the right hand, X on the left
	float secondary = -1.0f; // B on the right hand, Y on the left
	float menu = -1.0f;
	float thumbClick = -1.0f;
	float stickX = -2.0f; // the stick is bipolar, so ITS sentinel is < -1
	float stickY = -2.0f;
	// Wrist orientation, radians, relative to the head. Bipolar like the stick, so the
	// sentinel is a value no wrist can reach rather than a negative one.
	float yaw = kHoldAngleUnset;
	float pitch = kHoldAngleUnset;
	float roll = kHoldAngleUnset;
};

struct WheelSweepCommand
{
	bool valid = false;
	bool enabled = false;
	float hubX = 0.0f; // head-local metres; -Z is forward, +Y is up
	float hubY = -0.35f;
	float hubZ = -0.45f;
	float radius = 0.18f;  // rim radius, metres
	float tiltDeg = 20.0f; // rake: 0 is a vertical wheel, 90 a flat one
	float amplitudeDeg = 80.0f;
	float freqHz = 0.15f;
	float grip = 1.0f;		 // squeeze axis held at this value while enabled
	float spreadDeg = 90.0f; // hands at -/+ this bearing from the top: 90 is 9 and 3 o'clock
};

struct ScreenshotRequest
{
	std::string eye = "both";
	std::string layer = "projection";
};
struct WindowsInputCommands
{
	HeadPoseCommand head;
	FovCommand fov;
	IpdCommand ipd;
	HeadsetProfileCommand profile;
	AnaglyphCommand anaglyph;
	PoseSweepCommand sweep;
	ControllerPoseCommand controllerPose;
	ControllerHoldCommand controllerHold;
	WheelSweepCommand wheel;
};
struct WindowsOutputRequests
{
	bool screenshot = false;
	ScreenshotRequest capture;
	BurstCommand burst;
	bool projectionLog = false;
};
} // namespace mcp
