#pragma once

#include "simulation/CommandTypes.h"

#include <array>
#include <chrono>
#include <optional>

namespace simxr
{
using LiveInputClock = std::chrono::steady_clock;
struct LiveInputSample
{
	std::uint64_t connection = 0, sequence = 0, clockRevision = 0;
	RuntimeTime observedTime, targetTime;
	std::chrono::milliseconds maxAge{50};
	bool controlsActive = true;
	PoseSample head;
	std::optional<StereoEyeViews> eyeViews;
	std::optional<std::vector<TrackerDevice>> trackers;
	std::optional<BodyObservation> body;
	std::array<ControllerPose, 2> hands;
	std::array<ControllerControls, 2> controls;
};
enum class LiveInputPhase
{
	Disconnected,
	Active,
	Stale,
	Overflow
};
struct LiveInputStatus
{
	std::optional<CommandSource> owner;
	std::uint64_t connection = 0, sequence = 0, coalesced = 0, clockRevision = 0;
	LiveInputPhase phase = LiveInputPhase::Disconnected;
};
// Scheduler-owned value: copied with the prepared boundary so failed publication
// cannot consume an edge.
class LiveInputSlot
{
  public:
	using Channels = std::array<bool, ControlChannelCount>;
	static bool HasChannels(const Channels& owned);
	CommandResult Write(CommandSource source, const LiveInputSample& sample, const Channels& owned,
						LiveInputClock::time_point now);
	std::optional<CommandOutcome> Prepare(FrameId frame, SimulationTime time, const Channels& owned,
										  LiveInputClock::time_point now,
										  std::optional<std::uint64_t> clockRevision = {});
	LiveInputStatus Status() const;
	void AcknowledgeControls();
	bool Expired(LiveInputClock::time_point now) const;
	void Reset();

  private:
	struct Transitions
	{
		struct Transition
		{
			ControllerControls controls;
			RuntimeTime observedTime;
		};
		std::array<Transition, 16> values{};
		std::size_t first = 0, count = 0;
		ControllerControls last, published;
		RuntimeTime publishedTime;
		bool awaitingSync = false;
	};
	struct TrackerTransition
	{
		std::vector<TrackerDevice> devices;
		RuntimeTime observedTime;
	};
	std::array<TrackerTransition, 16> m_trackerTransitions;
	std::size_t m_trackerFirst = 0, m_trackerCount = 0;
	TrackerTransition m_publishedTrackers;
	bool m_trackersAwaitingSync = false;
	LiveInputSample m_latest;
	LiveInputStatus m_status;
	std::array<Transitions, 2> m_transitions;
	LiveInputClock::time_point m_received{};
	bool m_dirty = false, m_suspendedApplied = false, m_controlsSuspended = false;
};
} // namespace simxr
