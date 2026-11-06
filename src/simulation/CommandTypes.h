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
	Sequence,
	Device
};

struct CommandSource
{
	CommandSourceKind kind = CommandSourceKind::Manual;
	std::uint64_t id = 0;
	bool operator==(const CommandSource&) const = default;
};

inline constexpr CommandSource ApplicationAutomationSource{CommandSourceKind::Rig, UINT64_MAX};

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
	std::optional<BodyObservation> observation;
	// Replay maps this recorded runtime boundary to the actual application boundary.
	std::optional<RuntimeTime> observationOrigin;
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

struct SetTrackers
{
	std::vector<TrackerDevice> value;
};

struct SetViewSettings
{
	ViewSettings value;
};

struct SetEyeViews
{
	std::optional<StereoEyeViews> value;
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

using SimulationCommand =
	std::variant<TakeControl, ReleaseControl, SetHeadPose, SetControllerPose, SetControllerControls, SetBodyControls,
				 SetGaze, SetTracker, SetViewSettings, SetBodyProportions, SetControllerProfile, SetScenario,
				 SetReferenceSpaces, SetEyeViews, SetTrackers>;
enum class CommandLatePolicy
{
	Reject,
	RescheduleNext
};

struct CommandTarget
{
	// Exactly one selector. Time targets apply at the first boundary whose predicted display time is >= time.
	std::optional<FrameId> frame;
	std::optional<RuntimeTime> time;
};

struct CommandTransaction
{
	std::uint64_t id = 0;
	CommandSource source;
	CommandTarget target;
	CommandLatePolicy latePolicy = CommandLatePolicy::Reject;
	std::vector<SimulationCommand> commands;
	// Optional observed inputs that must still match at the application boundary.
	std::vector<SimulationCommand> expected;
};
enum class RecordingResult
{
	Ok,
	Busy,
	Conflict,
	Invalid,
	Closed
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

struct LiveInputCorrelation
{
	std::uint64_t connection = 0, sequence = 0;
	RuntimeTime observedTime, targetTime;
	std::array<RuntimeTime, 2> controlsObservedTime;
	std::uint64_t clockRevision = 0;
	std::optional<RuntimeTime> trackersObservedTime;
};
struct CommandRecordingOrigin;

struct CommandOutcome
{
	CommandTransaction transaction;
	CommandResult result = CommandResult::Invalid;
	FrameId appliedFrame;
	RuntimeTime appliedTime;
	bool recordingOverrun = false;
	std::optional<CommandTarget> effectiveTarget;
	// Exact immutable retry identity, separate from normalized acknowledged values.
	std::shared_ptr<const CommandTransaction> requested;
	// Release events have no client transaction ID. Cleanup between boundaries
	// follows appliedFrame; cleanup during a boundary precedes its snapshot.
	bool afterFrame = false;
	std::optional<LiveInputCorrelation> liveInput;
	std::optional<RuntimeTime> appliedRuntimeTime;
	// Shared by publication history and the recorder; captures the underlying state after a release.
	std::shared_ptr<const CommandRecordingOrigin> releaseState;
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
	// Predicted display time of the boundary, the XrTime the application observes.
	RuntimeTime time;
	CommandConfiguration configuration;
	bool configurationAvailable = true;
	std::array<std::optional<CommandSource>, ControlChannelCount> owners;
	ScenarioState scenarios{};
	std::vector<CommandOutcome> outcomes;
	std::optional<SequenceCaptureIntent> sequenceCapture;
};

// Submissions that never entered the pending queue. Late ones also enter outcomes.
struct CommandRejections
{
	struct Last
	{
		std::uint64_t id = 0;
		CommandSource source;
		CommandResult result = CommandResult::Invalid;
		CommandTarget target;
	};

	std::uint64_t late = 0, conflict = 0, invalid = 0, capacity = 0, closed = 0;
	std::optional<Last> last;
};

struct CommandSchedulerSnapshot
{
	bool closed = false;
	std::size_t pending = 0;
	std::vector<CommandOutcome> outcomes;
	std::shared_ptr<const AppliedCommandSnapshot> applied;
	std::uint64_t seed = 0;
	std::size_t pendingCapacity = 0;
	// Earliest targets among uncancelled pending transactions.
	std::optional<FrameId> nextPendingFrame;
	std::optional<RuntimeTime> nextPendingTime;
	CommandRejections rejections;
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
	RuntimeTime time;
	std::optional<RuntimeTime> runtimeTime;
};

struct CommandRecording
{
	std::uint32_t version = 3;
	bool active = false, overflow = false, drain = false;
	std::size_t total = 0, floor = 0;
	std::uint64_t identity = 0;
	CommandRecordingOrigin origin;
	std::vector<CommandOutcome> transactions;
};
} // namespace simxr
