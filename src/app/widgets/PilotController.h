#pragma once

#include "app/services/PilotBindings.h"
#include "simulation/InputState.h"
#include <array>
#include <cstdint>
#include <imgui.h>
#include <string>

namespace simxr::app
{
class ManualInputController;
class PlayerRig;
class Window;
struct ObservedSetup;
enum class PilotTarget
{
	Player,
	Head,
	Left,
	Right
};

class PilotController
{
public:
	explicit PilotController(PlayerRig& rig);
	void Configure(const PilotBindings& keys, const GamepadBindings& gamepad, bool remoteMouseDrag = false);
	void Update(Window& window, ManualInputController& input, bool available, bool hovered,
				bool automaticGamepad = true);
	void Stop(Window* window, ManualInputController& input, bool discard, bool forceButtons = false);
	// The HUD along the image's bottom edge: target and lock at the left, the
	// hand's inputs in the middle, the hint at the right. Returns whether it is hovered.
	bool DrawHud(ImVec2 imageMin, ImVec2 imageMax, ManualInputController& input);

	// Folded, the HUD is one slim strip; the shell persists this.
	bool& HudFolded()
	{
		return m_hudFolded;
	}

	bool Active() const
	{
		return m_captured || m_starting;
	}

	bool Locked() const
	{
		return m_locked;
	}

	PilotTarget Target() const
	{
		return m_target;
	}

	const std::string& Error() const
	{
		return m_error;
	}

private:
	void DrawHelp() const;
	std::string KeyName(PilotAction action) const;
	bool Down(PilotAction action) const;
	bool Pressed(PilotAction action) const;
	void Submit(ManualInputController& input, const ObservedSetup& observed, bool poseChanged,
				const std::array<ControllerControls, 2>& controls, bool final = false);
	PlayerRig& m_rig;
	PilotBindings m_keys = DefaultPilotBindings();
	GamepadBindings m_gamepad;
	std::array<ImGuiKey, static_cast<std::size_t>(PilotAction::Count)> m_resolved{};
	PilotTarget m_target = PilotTarget::Player;
	unsigned m_hand = 1, m_handEdits = 0;
	bool m_poseEngaged = false;
	bool m_remoteMouseDrag = false, m_captureModeChanged = false;
	bool m_captured = false, m_starting = false, m_locked = false, m_gamepadActive = false, m_hudFolded = false;
	std::uint64_t m_startObservation = 0;
	std::uint32_t m_gamepadIdentity = 0;
	std::array<ControllerControls, 2> m_controls{};
	std::array<bool, 2> m_engaged{};
	std::array<bool, 4> m_dpad{};
	std::string m_error;
	double m_lastInput = 0;
};
} // namespace simxr::app
