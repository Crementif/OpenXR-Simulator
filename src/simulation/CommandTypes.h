#pragma once

#include "presentation/FrameTypes.h"
#include "simulation/InputState.h"
#include "simulation/ReferenceSpaceState.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace simxr
{
enum class CommandSourceKind
{
	Manual,
	Mcp,
	Rig,
	Sequence
};
struct CommandSource
{
	CommandSourceKind kind = CommandSourceKind::Manual;
	std::uint64_t id = 0;
	bool operator==(const CommandSource&) const = default;
};
enum class ControlChannel
{
	Head,
	LeftPose,
	RightPose,
	LeftControls,
	RightControls,
	Body,
	Gaze,
	Trackers,
	Views,
	Proportions,
	Profiles,
	Session,
	Count
};
inline constexpr auto ControlChannelCount = static_cast<std::size_t>(ControlChannel::Count);
enum class FailureScenario
{
	HeadUntracked,
	HeadUnavailable,
	BodyUntracked,
	BodyUnavailable,
	GazeUntracked,
	GazeUnavailable,
	LeftDisconnected,
	RightDisconnected,
	TrackersDisconnected,
	Unfocused,
	Hidden,
	Recenter,
	SessionLoss,
	Count
};
using ScenarioState = std::array<bool, static_cast<std::size_t>(FailureScenario::Count)>;

struct TakeControl
{
	ControlChannel channel;
	bool replace = false;
};
struct ReleaseControl
{
	ControlChannel channel;
};
struct SetHeadPose
{
	PoseSample value;
};
struct SetControllerPose
{
	std::uint32_t hand = 0;
	ControllerPose value;
};
struct SetControllerControls
{
	std::uint32_t hand = 0;
	ControllerControls value;
};
struct SetBodyControls
{
	BodyControls value;
};
struct SetGaze
{
	GazeInput value;
};
struct SetTracker
{
	std::uint32_t index = 0;
	TrackerDevice value;
};
struct SetViewSettings
{
	ViewSettings value;
};
struct SetBodyProportions
{
	BodyProportions value;
};
struct SetControllerProfile
{
	std::uint32_t hand = 0;
	std::string path;
};
struct SetScenario
{
	FailureScenario scenario;
	bool enabled = true;
};
struct SetReferenceSpaces
{
	ReferenceSpaceState value;
};
using SimulationCommand = std::variant<TakeControl, ReleaseControl, SetHeadPose, SetControllerPose,
									   SetControllerControls, SetBodyControls, SetGaze, SetTracker, SetViewSettings,
									   SetBodyProportions, SetControllerProfile, SetScenario, SetReferenceSpaces>;
enum class CommandLatePolicy
{
	Reject,
	RescheduleNext
};
struct CommandTarget
{
	// Exactly one selector. Time targets apply at the first boundary >= time.
	std::optional<FrameId> frame;
	std::optional<SimulationTime> time;
};
struct CommandTransaction
{
	std::uint64_t id = 0;
	CommandSource source;
	CommandTarget target;
	CommandLatePolicy latePolicy = CommandLatePolicy::Reject;
	std::vector<SimulationCommand> commands;
};
enum class CommandResult
{
	Queued,
	Applied,
	Late,
	Rescheduled,
	Conflict,
	Cancelled,
	Invalid,
	Capacity,
	Closed,
	NotFound,
	Released
};
struct CommandOutcome
{
	CommandTransaction transaction;
	CommandResult result = CommandResult::Invalid;
	FrameId appliedFrame;
	SimulationTime appliedTime;
	bool recordingOverrun = false;
	std::optional<CommandTarget> effectiveTarget;
	// Exact immutable retry identity, separate from normalized acknowledged values.
	std::shared_ptr<const CommandTransaction> requested;
	// Release events have no client transaction ID. Cleanup between boundaries
	// follows appliedFrame; cleanup during a boundary precedes its snapshot.
	bool afterFrame = false;
};
struct CommandConfiguration
{
	TrackingState tracking;
	InputSnapshot input;
	ViewSettings views;
	BodyProportions proportions;
	ReferenceSpaceState referenceSpaces;
};
struct SequenceCaptureIntent
{
	std::uint64_t sequence = 0;
	FrameId frame;
	CommandSource owner;
};
struct AppliedCommandSnapshot
{
	std::uint64_t seed = 0, revision = 0;
	FrameId frame;
	SimulationTime time;
	RuntimeTime runtimeTime;
	CommandConfiguration configuration;
	bool configurationAvailable = true;
	std::array<std::optional<CommandSource>, ControlChannelCount> owners;
	ScenarioState scenarios{};
	std::vector<CommandOutcome> outcomes;
	std::optional<SequenceCaptureIntent> sequenceCapture;
};
struct CommandSchedulerSnapshot
{
	bool closed = false;
	std::size_t pending = 0;
	std::vector<CommandOutcome> outcomes;
	std::shared_ptr<const AppliedCommandSnapshot> applied;
	std::uint64_t seed = 0;
};
// A recording starts at one atomic command boundary. Configuration deliberately
// precedes scenario effects so disabling a recorded failure can recover its pose
// flags/activity. Trigger flags describe history, not events to replay at origin.
struct CommandRecordingOrigin
{
	std::uint64_t seed = 0;
	CommandConfiguration configuration;
	ScenarioState scenarios{};
	FrameId frame;
	SimulationTime time;
};
struct CommandRecording
{
	std::uint32_t version = 2;
	bool active = false, overflow = false;
	std::size_t total = 0;
	CommandRecordingOrigin origin;
	std::vector<CommandOutcome> transactions;
};
} // namespace simxr
