#pragma once

#include "control/ControlPacket.h"
#include "simulation/CommandTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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
	// Names the edit in History, such as a preset; empty describes it from the poses it changes.
	std::string label;
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
	std::optional<std::array<std::string, 2>> controllerProfiles;
	bool trackerAttachmentsAvailable = true;
	bool bodyOverridesAvailable = true;
	bool bodyRootAvailable = true;
	// The app enabled tracker interaction; absent from older runtimes.
	bool appTrackers = false;
	bool appTrackersKnown = false;
	std::optional<std::array<bool, 3>> appBodyExtensions;
	std::array<bool, MaxTrackerDevices> boundTrackers{};
};
std::optional<TrackingSnapshot> ReadInspectionTracking(const control::Message& state);
ObservedSetup ReadObservedSetup(const control::Message& state);
ManualTransaction PrepareHeadsetRelease(const ObservedSetup& observed, std::optional<CommandSource> manualSource,
										bool takeOver = false, bool trackers = false, bool body = false);
ControlChannel ScenarioInputChannel(FailureScenario scenario);

std::optional<CommandSource> SimulationEditSource(const control::Message& receipt);
std::string SimulationEditPhase(const control::Message& result);
std::optional<control::Message> ObservedEditResult(const control::Message& state, const control::Message& receipt);

PoseSample ObservedDevicePose(const control::Message& state, ControlChannel channel);
PoseSample PilotPose(PoseSample pose, XrVector3f movement, float yaw, float pitch, float seconds, float speed = 1);

control::ControlPacket EncodeSimulationEdit(const SimulationEdit& edit, std::uint64_t session, std::uint64_t generation,
											std::uint64_t transaction, const control::Message& observedState);
} // namespace simxr::app
