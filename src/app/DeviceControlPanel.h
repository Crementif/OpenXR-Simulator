#pragma once

#include "app/SimulationEdits.h"

#include <array>
#include <optional>

namespace simxr::app
{
// Owns authored values only. Observed runtime state never rewrites an edit in progress.
class DeviceControlPanel
{
  public:
	DeviceControlPanel();
	std::optional<SimulationEdit> Draw(bool supported);
	void ClearHeldInputs();
	ControlChannel PilotChannel() const
	{
		return m_pilotChannel;
	}
	void SetPose(ControlChannel channel, PoseSample pose);
	void ObserveState(const control::Message& state);
	void ResetObservations();

  private:
	void Apply(std::vector<SimulationCommand> commands);
	void ApplyOwned(ControlChannel channel, SimulationCommand command);
	void DrawOwnership();
	void DrawDetailFooter();
	void LoadObservedSettings();
	void DrawOwner(ControlChannel channel);
	bool DrawScenarioNotice(ControlChannel channel);
	void ReloadRecoveredChannel(ControlChannel channel);
	void DrawHead();
	void DrawHandRest();
	void DrawController();
	void DrawBody();
	void DrawGaze();
	void DrawTrackers();
	void DrawViews();
	void DrawScenarios();

	std::optional<SimulationEdit> m_edit;
	std::uint64_t m_applyFrame = 0;
	bool m_reschedule = true;
	bool m_autoApply = true;
	bool m_replaceOwner = false;
	bool m_configurationLoaded = false;
	bool m_headAvailable = false;
	std::optional<ObservedSetup> m_observed;
	std::string m_observationError;
	std::uint64_t m_loadedFrame = 0;
	int m_setupSection = 0;
	int m_selectedChannel = 0;
	int m_controlHand = 0;
	ControlChannel m_pilotChannel = ControlChannel::Head;
	int m_selectedScenario = 0;
	int m_bodyPreset = 0;
	int m_selectedTracker = 0;
	int m_trackerRole = 0;
	TrackingState m_authoredTracking;
	PoseSample m_observedHead = TrackingState{}.head;
	std::array<ControllerControls, 2> m_authoredControls;
	std::array<float, 3> m_headAngles{};
	std::array<std::array<float, 3>, 2> m_controllerAngles{};
	BodyProportions m_authoredProportions;
	ViewSettings m_authoredViews;
	std::array<std::array<char, 256>, 2> m_controllerProfiles{};
};
} // namespace simxr::app
