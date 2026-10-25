#include "platform/windows/WindowsRuntimeInput.h"
#include "Pch.h"

#include "math/Math.h"
#include "control/legacy/WindowsLegacyChannel.h"
#include "control/legacy/WindowsRuntimeSettings.h"
#include "platform/windows/WindowsClock.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <windows.h>

namespace simxr::windows
{
namespace settings = simxr::legacy::settings;

static double UptimeSeconds()
{
	return static_cast<double>(GetTickCount64()) * .001;
}

static XrFovf EyeFov(const settings::EyeFov& eye)
{
	constexpr float radians = std::numbers::pi_v<float> / 180;
	return {eye.angleLeft * radians, eye.angleRight * radians, eye.angleUp * radians, eye.angleDown * radians};
}

static simxr::legacy::ControllerKeyboardSample PollControllerKeyboard(float frameSeconds)
{
	const auto down = [](int key)
	{
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	};
	simxr::legacy::ControllerKeyboardSample input;
	input.active = true;
	input.frameSeconds = frameSeconds;
	input.animationKeyDown = down('M');
	input.resetRight = down(VK_NUMPAD5);
	input.translationDirection = {static_cast<float>(down(VK_NUMPAD6) - down(VK_NUMPAD4)),
								  static_cast<float>(down(VK_ADD) - down(VK_SUBTRACT)),
								  static_cast<float>(down(VK_NUMPAD2) - down(VK_NUMPAD8))};
	input.rotationDirection = {static_cast<float>(down(VK_NUMPAD9) - down(VK_NUMPAD7)),
							   static_cast<float>(down(VK_NUMPAD1) - down(VK_NUMPAD3))};
	auto& left = input.hands[0];
	left.trigger = down(VK_LCONTROL);
	left.grip = down(VK_LMENU);
	left.menu = down('G');
	left.primary = down('V');
	left.secondary = down('B');
	if (down('I'))
		left.stick.y = 1;
	if (down('K'))
		left.stick.y = -1;
	if (down('J'))
		left.stick.x = -1;
	if (down('L'))
		left.stick.x = 1;
	auto& right = input.hands[1];
	right.trigger = down(VK_SPACE);
	right.grip = down('F');
	right.menu = down(VK_TAB);
	right.primary = down('R');
	right.secondary = down('T');
	right.thumbClick = down(VK_RETURN);
	if (down(VK_UP))
		right.stick.y = 1;
	if (down(VK_DOWN))
		right.stick.y = -1;
	if (down(VK_LEFT))
		right.stick.x = -1;
	if (down(VK_RIGHT))
		right.stick.x = 1;
	return input;
}

static simxr::legacy::ControllerOverrideEdit<float> DecodeHoldAxis(float value, float presentMinimum,
																   float activeMinimum)
{
	using Edit = simxr::legacy::ControllerOverrideEdit<float>;
	if (!(value >= presentMinimum))
		return {};
	if (value < activeMinimum)
		return Edit::Release();
	return Edit::Set(value);
}

static simxr::legacy::ControllerOverrideEdit<bool> DecodeHoldButton(float value)
{
	using Edit = simxr::legacy::ControllerOverrideEdit<bool>;
	if (!(value >= -0.5f))
		return {};
	if (value < 0)
		return Edit::Release();
	return Edit::Set(value >= 0.5f);
}

static simxr::legacy::ControllerHoldEdit DecodeControllerHold(const mcp::ControllerHoldCommand& command)
{
	using namespace simxr::legacy;
	ControllerHoldEdit edit;
	if (command.hand >= 0)
		edit.hand = command.hand == 0 ? ControllerHand::Left : ControllerHand::Right;
	edit.clear = command.clear;
	edit.trigger = DecodeHoldAxis(command.trigger, -0.5f, 0);
	edit.grip = DecodeHoldAxis(command.grip, -0.5f, 0);
	edit.primary = DecodeHoldButton(command.primary);
	edit.secondary = DecodeHoldButton(command.secondary);
	edit.menu = DecodeHoldButton(command.menu);
	edit.thumbClick = DecodeHoldButton(command.thumbClick);
	edit.stickX = DecodeHoldAxis(command.stickX, -1.5f, -1);
	edit.stickY = DecodeHoldAxis(command.stickY, -1.5f, -1);
	if (command.yaw != mcp::kHoldAngleUnset)
		edit.yaw = ControllerOverrideEdit<float>::Set(command.yaw);
	if (command.pitch != mcp::kHoldAngleUnset)
		edit.pitch = ControllerOverrideEdit<float>::Set(command.pitch);
	if (command.roll != mcp::kHoldAngleUnset)
		edit.roll = ControllerOverrideEdit<float>::Set(command.roll);
	return edit;
}

mcp::HeadPoseCommand WindowsRuntimeInput::Head() const
{
	std::lock_guard lock(m_mutex);
	return m_head;
}

void WindowsRuntimeInput::SetHead(const mcp::HeadPoseCommand& pose)
{
	std::lock_guard lock(m_mutex);
	const auto roll = pose.hasRoll ? pose.roll : m_head.roll;
	m_head = pose;
	m_head.roll = roll;
}

RuntimeInputSnapshot WindowsRuntimeInput::SnapshotLocked(double uptimeSeconds) const
{
	RuntimeInputSnapshot output;
	output.head = m_head;
	auto yaw = m_head.yaw, pitch = m_head.pitch, roll = m_head.roll;
	if (m_sweep.enabled)
	{
		const double phase = 2 * std::numbers::pi * m_sweep.freqHz * (uptimeSeconds - m_sweepStart);
		constexpr double radians = std::numbers::pi / 180;
		yaw = static_cast<float>(m_sweep.yawAmpDeg * radians * std::sin(phase));
		pitch = static_cast<float>(m_sweep.pitchAmpDeg * radians * std::sin(phase + 1));
		roll = static_cast<float>(m_sweep.rollAmpDeg * radians * std::sin(phase + 2));
	}
	const XrVector3f position{m_head.x, m_head.y, m_head.z};
	output.tracking.head.pose = {EulerAngles(yaw, pitch, roll), position};
	const auto controllers = m_controllers.Snapshot({position, m_head.yaw, m_head.pitch});
	for (std::size_t hand = 0; hand < controllers.hands.size(); ++hand)
	{
		output.tracking.controllers[hand].pose.pose = controllers.hands[hand].worldPose;
		auto& controls = output.controls[hand];
		const auto& controller = controllers.hands[hand].controls;
		controls.connected = true;
		controls.trigger = controller.trigger;
		controls.grip = controls.gripForce = controller.grip;
		controls.stick = controls.trackpad = controller.stick;
		controls.primary = controls.primaryTouch = controller.primary;
		controls.secondary = controls.secondaryTouch = controller.secondary;
		controls.menu = controller.menu;
		controls.thumbClick = controls.trackpadClick = controller.thumbClick;
		controls.triggerTouch = controller.triggerTouch;
		controls.thumbTouch = controls.trackpadTouch = controller.thumbClick;
	}
	const auto configuration = settings::ReadState();
	settings::GetRenderResolution(output.views.outerWidth, output.views.outerHeight);
	output.views.ipd = m_ipd.value_or(std::clamp(configuration.ipdMeters, 0.0f, .2f));
	if (m_fov)
		output.views.outerFov = *m_fov;
	else if (configuration.useAsymmetricFov &&
			 configuration.headsetProfile != settings::HeadsetProfile::GenericSymmetric)
	{
		const auto& profile = settings::GetHeadsetSpec(configuration.headsetProfile);
		for (std::size_t eye = 0; eye < output.views.outerFov.size(); ++eye)
			output.views.outerFov[eye] = EyeFov(profile.eye[eye]);
	}
	else
	{
		const auto degrees =
			configuration.fovDegrees > 0 && configuration.fovDegrees <= 180 ? configuration.fovDegrees : 90;
		const float halfAngle = degrees * std::numbers::pi_v<float> / 360;
		output.views.outerFov.fill({-halfAngle, halfAngle, halfAngle, -halfAngle});
	}
	return output;
}

RuntimeInputSnapshot WindowsRuntimeInput::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return SnapshotLocked(UptimeSeconds());
}

RuntimeInputSnapshot WindowsRuntimeInput::Poll(std::chrono::nanoseconds period, mcp::WindowsLegacyChannel& files)
{
	XrTime counterTime;
	if (XR_FAILED(ReadPerformanceTime(counterTime)))
		throw std::runtime_error("Cannot read Windows input clock");
	const double counterSeconds = static_cast<double>(counterTime) * 1e-9;
	const double uptimeSeconds = UptimeSeconds();
	std::lock_guard lock(m_mutex);
	const double periodSec = std::chrono::duration<double>(period).count();

	// The application owns the foreground window; the companion does not poll here.
	m_head.pitch = std::clamp(m_head.pitch, -1.5f, 1.5f);

	const settings::InputPolling inputPolling = settings::ReadState().inputPolling;
	bool inputActive = (inputPolling == settings::InputPolling::Background);
	const HWND foregroundWindow = inputActive ? nullptr : GetForegroundWindow();
	if (foregroundWindow && inputPolling == settings::InputPolling::AppForeground)
	{
		DWORD foregroundProcess = 0;
		GetWindowThreadProcessId(foregroundWindow, &foregroundProcess);
		inputActive = foregroundProcess == GetCurrentProcessId();
	}
	simxr::legacy::ControllerMotionFrame controllerFrame;
	controllerFrame.glideSeconds = counterSeconds;
	if (inputActive)
	{
		const auto down = [](int key)
		{
			return (GetAsyncKeyState(key) & 0x8000) != 0;
		};
		const float moveSpeed = settings::GetMoveSpeed(down(VK_SHIFT));
		const float deltaTime = static_cast<float>(periodSec);
		const auto orientation = EulerAngles(m_head.yaw, m_head.pitch, 0);
		const auto forward = Rotate(orientation, {0, 0, -1});
		const auto right = Rotate(orientation, {1, 0, 0});
		const auto forwardStep = static_cast<float>(down('W') - down('S')) * moveSpeed * deltaTime;
		const auto rightStep = static_cast<float>(down('D') - down('A')) * moveSpeed * deltaTime;
		const auto upStep = static_cast<float>(down('E') - down('Q')) * moveSpeed * deltaTime;
		m_head.x += forward.x * forwardStep + right.x * rightStep;
		m_head.y += forward.y * forwardStep + right.y * rightStep + upStep;
		m_head.z += forward.z * forwardStep + right.z * rightStep;

		controllerFrame.keyboard = PollControllerKeyboard(deltaTime);
	}

	files.TakeWrittenScreenshot();

	// MCP commands work regardless of window focus (file-based IPC).
	// The joined worker already decoded and staged copied values.
	{
		const auto commands = files.TakeCommands();
		const auto& cmd = commands.head;
		if (cmd.valid)
		{
			m_head.x = cmd.x;
			m_head.y = cmd.y;
			m_head.z = cmd.z;
			m_head.yaw = cmd.yaw;
			m_head.pitch = cmd.pitch;
			if (cmd.hasRoll)
				m_head.roll = cmd.roll;
			files.Acknowledge("head_pose", true);
		}

		// Per-eye FOV override (asymmetric headset profile testing).
		const auto& fov = commands.fov;
		if (fov.valid)
		{
			if (fov.clear)
			{
				m_fov.reset();
			}
			else
			{
				m_fov.emplace();
				for (std::size_t i = 0; i < m_fov->size(); ++i)
				{
					(*m_fov)[i].angleLeft = fov.angleLeft[i];
					(*m_fov)[i].angleRight = fov.angleRight[i];
					(*m_fov)[i].angleUp = fov.angleUp[i];
					(*m_fov)[i].angleDown = fov.angleDown[i];
				}
			}
			files.Acknowledge("fov", true);
		}

		// Settable IPD (test app's response to IPD ranges; 0mm = no parallax).
		const auto& ipdCmd = commands.ipd;
		if (ipdCmd.valid)
		{
			if (ipdCmd.clear)
			{
				m_ipd.reset();
			}
			else
			{
				m_ipd = ipdCmd.ipdMeters;
			}
			files.Acknowledge("ipd", true);
		}

		// Headset profile preset: applies both FOV and IPD at once.
		const auto& prof = commands.profile;
		if (prof.valid)
		{
			const int idx = settings::FindHeadsetSpec(prof.name);
			// "generic" carries no headset geometry, so it means the same as clearing.
			const bool clear =
				idx == 0 || std::strcmp(prof.name, "default") == 0 || std::strcmp(prof.name, "clear") == 0;
			bool applied = true;
			if (clear)
			{
				m_fov.reset();
				m_ipd.reset();
			}
			else if (idx > 0)
			{
				const auto& spec = settings::kHeadsetSpecs[idx];
				m_fov.emplace();
				for (std::size_t eye = 0; eye < m_fov->size(); ++eye)
					(*m_fov)[eye] = EyeFov(spec.eye[eye]);
				m_ipd = spec.ipdMm * 0.001f;
			}
			else
			{
				applied = false;
			}
			files.Acknowledge("headset_profile", applied);
		}

		// Display layout belongs to the companion; the removed runtime mirror cannot apply this command.
		if (commands.anaglyph.valid)
			files.Acknowledge("anaglyph", false);

		// Pose sweep: oscillate yaw/pitch/roll on a sine wave each frame.
		const auto& sweep = commands.sweep;
		if (sweep.valid)
		{
			m_sweep = sweep;
			m_sweepStart = uptimeSeconds;
			files.Acknowledge("pose_sweep", true);
		}

		const auto pose = commands.controllerPose;
		if (pose.valid)
		{
			simxr::legacy::ControllerPoseEdit edit;
			edit.hand = pose.hand == 0 ? simxr::legacy::ControllerHand::Left : simxr::legacy::ControllerHand::Right;
			edit.position = {pose.posX, pose.posY, pose.posZ};
			edit.yaw = pose.yaw;
			edit.pitch = pose.pitch;
			edit.duration = std::chrono::duration<double, std::milli>{pose.durationMs};
			edit.receivedSeconds = counterSeconds;
			if (pose.triggerSet)
				edit.trigger = pose.trigger;
			if (pose.buttonA >= 0)
				edit.primary = pose.buttonA != 0;
			controllerFrame.pose = edit;
			files.Acknowledge("controller_pose", true);
		}

		const auto hold = commands.controllerHold;
		if (hold.valid)
		{
			// Legacy selectors outside [0,1] target no hand; negative means both.
			if (hold.hand < 2)
				controllerFrame.hold = DecodeControllerHold(hold);
			files.Acknowledge("controller_hold", true);
		}

		const auto wheel = commands.wheel;
		if (wheel.valid)
		{
			constexpr float radians = std::numbers::pi_v<float> / 180;
			simxr::legacy::ControllerWheelSweep edit;
			edit.enabled = wheel.enabled;
			edit.hub = {wheel.hubX, wheel.hubY, wheel.hubZ};
			edit.radius = wheel.radius;
			edit.tilt = wheel.tiltDeg * radians;
			edit.amplitude = wheel.amplitudeDeg * radians;
			edit.frequencyHz = wheel.freqHz;
			edit.grip = wheel.grip;
			edit.spread = wheel.spreadDeg * radians;
			edit.receivedSeconds = uptimeSeconds;
			controllerFrame.wheel = edit;
			files.Acknowledge("wheel_sweep", true);
		}
	}

	// Evaluation is atomic for action readers: keyboard -> file pose -> wheel
	// -> held overrides. Shared pose history owns observable space velocities.
	controllerFrame.head = {XrVector3f{m_head.x, m_head.y, m_head.z}, m_head.yaw, m_head.pitch};
	controllerFrame.wheelSeconds = uptimeSeconds;
	const auto controllers = m_controllers.Update(controllerFrame);
	if (controllers.animationToggle)
		files.Log(*controllers.animationToggle ? "[SimXR] Auto motion ENABLED" : "[SimXR] Auto motion DISABLED");
	return SnapshotLocked(uptimeSeconds);
}
} // namespace simxr::windows
