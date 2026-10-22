#pragma once

#include "control/ControlPacket.h"
#include "simulation/CommandTypes.h"

namespace simxr::app
{
struct ManualTransaction
{
	// An absent selector uses the next frame after the observed command publication.
	std::optional<FrameId> frame;
	bool continuous = false;
	bool releaseEdge = false;
	CommandLatePolicy latePolicy = CommandLatePolicy::RescheduleNext;
	std::vector<SimulationCommand> commands;
};
struct ReleaseManualInputs
{
};
using SimulationEdit = std::variant<ManualTransaction, ReleaseManualInputs>;

struct ObservedSetup
{
	CommandConfiguration values;
	std::array<std::optional<CommandSource>, ControlChannelCount> owners;
	ScenarioState scenarios{};
	std::uint64_t frame = 0;
	bool trackerAttachmentsAvailable = true;
	bool bodyOverridesAvailable = true;
	bool bodyRootAvailable = true;
	// The app enabled tracker interaction; absent from older runtimes.
	bool appTrackers = false;
	std::array<bool, 4> boundTrackers{};
};
// Ctrl+Z history of device poses: one entry per edit group (a whole drag or one click), never per frame.
class UndoStack
{
  public:
	// Call for each submitted edit with the state observed before it. Input-only edits are not recorded.
	void Record(const ObservedSetup& before, const ManualTransaction& edit);
	// Refresh only from an observation with no pending local edit.
	void Observe(const ObservedSetup& observed);
	bool Empty() const
	{
		return m_entries.empty();
	}
	void Clear();
	// Restores the newest entry on the channels this source may drive.
	std::optional<ManualTransaction> Undo(const ObservedSetup& current, std::optional<CommandSource> source);

  private:
	struct Entry
	{
		TrackingState tracking;
		BodyProportions proportions;
	};
	std::vector<Entry> m_entries;
	std::optional<Entry> m_authored;
	bool m_open = false;
};
std::optional<TrackingSnapshot> ReadInspectionTracking(const control::Message& state);
ObservedSetup ReadObservedSetup(const control::Message& state);
ControlChannel ScenarioInputChannel(FailureScenario scenario);

std::optional<CommandSource> SimulationEditSource(const control::Message& receipt);
std::string SimulationEditPhase(const control::Message& result);
std::optional<control::Message> ObservedEditResult(const control::Message& state, const control::Message& receipt);

PoseSample ObservedDevicePose(const control::Message& state, ControlChannel channel);
PoseSample PilotPose(PoseSample pose, XrVector3f movement, float yaw, float pitch, float seconds, float speed = 1);

control::ControlPacket EncodeSimulationEdit(const SimulationEdit& edit, std::uint64_t session, std::uint64_t generation,
											std::uint64_t transaction, const control::Message& observedState);
} // namespace simxr::app
