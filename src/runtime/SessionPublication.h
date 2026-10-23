#pragma once

#include "presentation/FrameHistory.h"
#include "runtime/Instance.h"
#include "runtime/extensions/EyeGazeInteraction.h"
#include "simulation/CommandScheduler.h"
#include "simulation/SpaceSystem.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace simxr
{
// Owns the session's commanded/native state merge and historical publications.
// Instance and synchronized actions outlive this owner. Session serializes
// mutating calls; exposed simulation services retain their own reader locks.
class SessionPublication
{
  public:
	class PreparedStart
	{
	  public:
		PreparedStart(SessionPublication& owner, RuntimeTime epoch, ViewLayout layout);
		~PreparedStart();
		PreparedStart(const PreparedStart&) = delete;
		PreparedStart& operator=(const PreparedStart&) = delete;
		const ReferenceSpaceState& ReferenceSpaces() const;
		void Commit();

	  private:
		SessionPublication& m_owner;
		CommandConfiguration m_baseline;
		RuntimeTime m_epoch;
		ViewLayout m_layout;
		bool m_committed = false;
	};

	// Copies the baseline before pacing. Reserve applies one exact command
	// boundary while scheduler admission is locked; its callback reserves all
	// throwing history/capture storage before command state commits.
	class FrameBoundary
	{
	  public:
		FrameBoundary(SessionPublication& owner, bool focused, const ReferenceSpaceState& spaces);
		FrameBoundary(const FrameBoundary&) = delete;
		FrameBoundary& operator=(const FrameBoundary&) = delete;
		void Reserve(const FramePrediction& prediction,
					 const std::function<void(const AppliedCommandSnapshot&)>& reserveHistory);
		const AppliedCommandSnapshot& Applied() const;
		std::optional<XrSessionState> StateTransition(XrSessionState current);
		void Publish();

	  private:
		SessionPublication& m_owner;
		CommandConfiguration m_baseline;
		RuntimeTime m_epoch;
		std::shared_ptr<const AppliedCommandSnapshot> m_applied;
		bool m_published = false;
	};

	SessionPublication(Instance& instance, ActionSyncState& actions);
	void Configure(std::uint64_t seed, std::optional<CommandConfiguration> initial, bool overrideInitial);
	void Stop();
	bool ReleaseSource(CommandSource source, bool retire);
	void StopPoseSequence(CommandSource source);
	void PublishTracking(RuntimeTime time, TrackingState state, const ViewSettings& settings, bool focused);
	XrResult PublishInput(InputSnapshot input);
	CommandConfiguration ConfigurationSnapshot(const FramePrediction& lastIssued) const;
	void StartRecording(const ReferenceSpaceState& spaces);
	void CaptureFrame(FrameId frame, FrameSubmissionState& submission) const;
	void RecordViewQuery(ViewQuerySnapshot query);
	bool TakeProfileChange() noexcept;
	CommandScheduler& Commands();
	InputState& Input();
	TrackingSystem& Tracking();
	ViewConfiguration ConfigurationAt(RuntimeTime time) const;
	ViewLayout Layout() const noexcept;
	LocatedViews ViewsAt(RuntimeTime time, bool foveated) const;
	PoseSample ActionPose(XrAction action, XrPath subaction, RuntimeTime time) const;
	XrResult ValidatePoseAction(XrAction action, XrPath subaction) const;

  private:
	bool ReleaseChannels(const std::array<bool, ControlChannelCount>& channels);
	struct ViewRevision
	{
		RuntimeTime time;
		ViewConfiguration configuration;
	};
	void PublishEffective(RuntimeTime time, const CommandConfiguration& effective);
	Instance& m_instance;
	ActionSyncState& m_actions;
	CommandScheduler m_commands;
	InputState m_input;
	TrackingSystem m_tracking;
	EyeGazeInteraction m_gaze;
	std::uint64_t m_seed = 0;
	std::optional<CommandConfiguration> m_initial;
	bool m_overrideInitial = true;
	RuntimeTime m_epoch;
	CommandConfiguration m_baseline;
	std::uint64_t m_managedInputRevision = 0;
	bool m_sessionOverride = false;
	bool m_profileChanged = false;
	std::deque<ViewRevision> m_viewHistory;
	std::deque<ViewQuerySnapshot> m_viewQueries;
	ViewLayout m_layout = ViewLayout::Stereo;
};
} // namespace simxr
