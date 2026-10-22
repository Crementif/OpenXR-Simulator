#pragma once

#include "app/PlayerRig.h"
#include "app/ShellState.h"
#include "app/SimulationEdits.h"

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>

namespace simxr::app
{
// Owns authored values only. Observed runtime state never rewrites an edit in progress.
class DeviceControlPanel
{
  public:
	explicit DeviceControlPanel(PlayerRig& rig);
	// placement draws the spatial editors; hand pages put it below the input pad.
	std::optional<SimulationEdit> Draw(bool supported, const Selection* selection = nullptr,
									   const std::function<void()>& placement = {});
	void ClearHeldInputs();
	int SelectedTracker() const
	{
		return m_selectedTracker;
	}
	void SetRigPiloting(bool piloting)
	{
		m_rigPiloting = piloting;
	}
	ManualTransaction PlayerEdit(bool includeWorld = false) const;
	void SetManualSource(std::optional<CommandSource> source)
	{
		m_manualSource = source;
	}
	void ObserveState(const control::Message& state);
	void ResetObservations();
	void ObserveDelivery(bool pending, const std::string& error);
	std::optional<SimulationEdit> DrawAdvanced();

  private:
	void Apply(std::vector<SimulationCommand> commands);
	void ApplyOwned(ControlChannel channel, SimulationCommand command);
	void DrawOwnership();
	void LoadObservedSettings();
	void DrawOwner(ControlChannel channel);
	bool DrawScenarioNotice(ControlChannel channel);
	void ReloadRecoveredChannel(ControlChannel channel);
	void DrawPlayer();
	void DrawAnchor(bool tracker, std::size_t index);
	void DrawHead();
	void DrawExactPose(int device);
	void DrawHandRest();
	void DrawController(const std::function<void()>& placement);
	void DrawInputPad(std::uint32_t hand);
	void DrawBody();
	void DrawGaze();
	void DrawTrackers();
	void DrawViews();
	void DrawScenarios();

	PlayerRig& m_playerRig;
	std::optional<CommandSource> m_manualSource;
	bool m_rigPiloting = false;
	std::optional<SimulationEdit> m_edit;
	std::uint64_t m_applyFrame = 0;
	bool m_reschedule = true;
	bool m_pending = false, m_continuousEdit = false;
	std::set<std::string> m_pendingFields, m_rejectedFields;
	std::string m_deliveryError, m_fieldScope;
	double m_rejectedUntil = 0;
	bool Field(const char* key, bool changed, bool continuous = false);
	bool m_replaceOwner = false, m_exactAnchor = false;
	bool m_configurationLoaded = false;
	std::optional<ObservedSetup> m_observed;
	std::string m_observationError;
	std::uint64_t m_loadedFrame = 0;
	int m_setupSection = 0;
	int m_selectedChannel = 0;
	int m_controlHand = 0;
	int m_selectedScenario = 0;
	int m_bodyPreset = 0;
	int m_selectedTracker = 0;
	int m_trackerRole = 0;
	TrackingState m_authoredTracking;
	std::array<ControllerControls, 2> m_authoredControls;
	struct Latches
	{
		bool trigger = false, grip = false, primary = false, secondary = false, menu = false, stick = false;
	};
	std::array<Latches, 2> m_latches{};
	BodyProportions m_authoredProportions;
	ViewSettings m_authoredViews;
	std::array<std::array<char, 256>, 2> m_controllerProfiles{};
};
} // namespace simxr::app
