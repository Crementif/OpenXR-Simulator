#include "platform/windows/WindowsRuntimeInput.h"
#include "Pch.h"

#include "math/Math.h"
#include "platform/windows/WindowsRuntimeFiles.h"
#include "control/legacy/WindowsRuntimeSettings.h"
#include "platform/windows/WindowsClock.h"

#include <windows.h>

namespace simxr::windows
{
namespace settings = simxr::legacy::settings;

WindowsRuntimeInput::WindowsRuntimeInput()
{
	if (const auto* width = std::getenv("SIMXR_WIDTH"))
		m_renderWidth = ParseLaunchRenderExtent(width);
	if (const auto* height = std::getenv("SIMXR_HEIGHT"))
		m_renderHeight = ParseLaunchRenderExtent(height);
}

RuntimeInputSnapshot WindowsRuntimeInput::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return SnapshotLocked();
}

static simxr::legacy::ControllerKeyboardSample PollControllerKeyboard(bool inputActive, float frameSeconds)
{
	simxr::legacy::ControllerKeyboardSample input;
	input.active = true;
	input.hands[1].thumbClick = false;
	// An inactive window still supplies releases for its keyboard-owned controls.
	if (!inputActive)
		return input;
	const auto down = [](int key) {
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	};
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

RuntimeInputSnapshot WindowsRuntimeInput::Poll(std::chrono::nanoseconds period, WindowsRuntimeFiles& files)
{
	XrTime counterTime;
	if (XR_FAILED(ReadPerformanceTime(counterTime)))
		throw std::runtime_error("Cannot read Windows input clock");
	const double counterSeconds = static_cast<double>(counterTime) * 1e-9;
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
		const auto down = [](int key) {
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
	}
	controllerFrame.keyboard = PollControllerKeyboard(inputActive, static_cast<float>(periodSec));

	controllerFrame.head = {XrVector3f{m_head.x, m_head.y, m_head.z}, m_head.yaw, m_head.pitch};
	const auto controllers = m_controllers.Update(controllerFrame);
	if (controllers.animationToggle)
		files.Log(*controllers.animationToggle ? "[SimXR] Auto motion ENABLED" : "[SimXR] Auto motion DISABLED");
	return SnapshotLocked();
}

RuntimeHeadPose WindowsRuntimeInput::Head() const
{
	std::lock_guard lock(m_mutex);
	return m_head;
}

static XrFovf EyeFov(const settings::EyeFov& eye)
{
	constexpr float radians = std::numbers::pi_v<float> / 180;
	return {eye.angleLeft * radians, eye.angleRight * radians, eye.angleUp * radians, eye.angleDown * radians};
}

RuntimeInputSnapshot WindowsRuntimeInput::SnapshotLocked() const
{
	RuntimeInputSnapshot output;
	output.head = m_head;
	const XrVector3f position{m_head.x, m_head.y, m_head.z};
	output.tracking.head.pose = {EulerAngles(m_head.yaw, m_head.pitch, m_head.roll), position};
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
	if (m_renderWidth)
		output.views.outerWidth = *m_renderWidth;
	if (m_renderHeight)
		output.views.outerHeight = *m_renderHeight;
	output.views.ipd = std::clamp(configuration.ipdMeters, 0.0f, .2f);
	if (configuration.useAsymmetricFov && configuration.headsetProfile != settings::HeadsetProfile::GenericSymmetric)
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
} // namespace simxr::windows
