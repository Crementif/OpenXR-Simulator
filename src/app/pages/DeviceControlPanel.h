#pragma once

#include "app/services/ShellState.h"
#include "app/services/SimulationEdits.h"
#include "app/widgets/LiveField.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace simxr::app
{
class PlayerRig;

// Owns authored values only. Observed runtime state never rewrites an edit in progress.
class DeviceControlPanel
{
  public:
	explicit DeviceControlPanel(PlayerRig& rig);
	std::optional<SimulationEdit> Draw(bool supported, const Selection* selection = nullptr,
									   const std::function<void()>& placement = {});
	void ClearHeldInputs();
	bool Active() const;
	void CancelEditing();
	std::optional<SimulationEdit> EndInputFrame();
	int SelectedTracker() const
	{
		return m_selectedTracker;
	}
	void SetRigPiloting(bool piloting)
	{
		m_rigPiloting = piloting;
	}
	void SetManualSource(std::optional<CommandSource> source)
	{
		m_manualSource = source;
	}
	void ObserveState(const control::Message& state);
	void ResetObservations();
	void ObserveDelivery(bool pending, const std::string& error);
	std::optional<SimulationEdit> DrawAdvanced();
	std::optional<SimulationEdit> DrawSessionMenu();
	std::optional<SimulationEdit> DrawHeader(const Selection& selection, bool showState = true);

  private:
	void SelectDevice(const DeviceSel& device);
	bool CanControl(ControlChannel channel) const;
	bool CanReset(DevicePage page) const;
	ManualTransaction PlayerEdit(bool includeWorld = false) const;
	void Apply(std::vector<SimulationCommand> commands, std::string label = {});
	void ApplyOwned(ControlChannel channel, SimulationCommand command);
	void DrawOwnership();
	void LoadObservedSettings();
	void DrawOwner(ControlChannel channel, bool takeover = true);
	bool ScenarioActive(ControlChannel channel) const;
	void DrawTracking(DevicePage page);
	void ReloadRecoveredChannel(ControlChannel channel);
	void DrawPlayer();
	void DrawAnchor(std::size_t hand);
	void DrawHead();
	void DrawExactPose(int device);
	void DrawHandPresets(std::size_t hand);
	void DrawBodyPoses();
	void ResetPage(DevicePage page);
	void ApplyRig(std::vector<SimulationCommand> extra = {}, std::string label = {});
	void DrawController(std::uint32_t hand, const std::function<void()>& placement);
	void DrawInputPad(std::uint32_t hand);
	void DrawBody();
	void DrawGaze();
	void DrawTrackers();
	void DrawViews();

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
	bool Field(const char* key, bool changed, bool continuous = false, const FieldValue& value = {});
	bool InputField(const char* key, bool changed);
	bool m_replaceOwner = false, m_exactAnchor = false;
	std::array<bool, MaxTrackerDevices + 3> m_quaternionView{};
	bool m_configurationLoaded = false;
	std::optional<ObservedSetup> m_observed;
	std::string m_observationError;
	std::optional<DeviceSel> m_selection;
	unsigned m_activeField = 0;
	int m_fieldFrame = -1;
	int m_selectedChannel = 0;
	int m_selectedTracker = 0;
	TrackingState m_authoredTracking;
	std::array<ControllerControls, 2> m_authoredControls;
	struct PadInputs
	{
		bool trigger = false, grip = false, primary = false, secondary = false, menu = false, stick = false;
	};
	std::array<PadInputs, 2> m_latches{}, m_heldInputs{};
	std::array<int, 2> m_padFrames{-1, -1};
	std::array<unsigned, 2> m_padActiveIds{};
	BodyProportions m_authoredProportions;
	ViewSettings m_authoredViews;
	std::array<std::array<char, 256>, 2> m_controllerProfiles{};
};
} // namespace simxr::app
