#pragma once

#include "presentation/FramePreviewHistory.h"

#include "presentation/FrameHistory.h"
#include "presentation/FrameScheduler.h"
#include "presentation/SnapshotStore.h"
#include "presentation/VirtualDisplay.h"
#include "runtime/Instance.h"
#include "runtime/SessionCaptureCoordinator.h"
#include "runtime/SessionInteraction.h"
#include "runtime/SessionPublication.h"
#include "runtime/extensions/BodyTracking.h"
#include "simulation/ActionRegistry.h"
#include "simulation/HapticState.h"
#include "simulation/InputState.h"
#include "simulation/SpaceSystem.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace simxr
{
enum class FrameSubmissionPolicy
{
	Exact,
	Live
};

struct SessionSnapshot
{
	XrSessionState state = XR_SESSION_STATE_IDLE;
	bool running = false;
	bool closed = false;
	FramePrediction lastIssued;
	FrameId lastSubmitted;
	RuntimeTime time;
	SessionGeneration generation;
};

class Session
{
public:
	Session(XrSession handle, Instance& instance, std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
	Session(XrSession handle, std::shared_ptr<Instance> instance,
			std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
	~Session();
	XrResult Ready(RuntimeTime time);
	XrResult Begin(const XrSessionBeginInfo& info, const FrameSchedulerConfig& config,
				   PresentationPolicy policy = PresentationPolicy::Repeat,
				   std::shared_ptr<const PoseSequence> startupSequence = {});
	XrResult RequestExit(RuntimeTime time);
	XrResult End(RuntimeTime time);
	XrResult SetState(XrSessionState state, RuntimeTime time);
	void SetFocused(bool focused, RuntimeTime time);
	void Close();
	// Native publication runs after admission with session/publication locks released.
	// Failure retires only the admitted generation, including partially published state.
	XrResult WaitFrame(FramePrediction& prediction,
					   const std::function<XrResult(const FramePrediction&)>& publishNative = {});
	XrResult BeginFrame();
	XrResult EndFrame(FramePrediction& prediction, const XrFrameEndInfo* info = nullptr,
					  std::shared_ptr<const FrameLayers> prepared = {}, std::shared_ptr<const FrameOutput> output = {},
					  const std::function<XrResult()>& submit = {}, std::span<const CaptureOutput> captures = {},
					  FrameSubmissionPolicy policy = FrameSubmissionPolicy::Exact, ReprojectionInput reprojection = {},
					  std::shared_ptr<const FrameGpuTiming> gpuTiming = {});
	XrResult Attach(const XrSessionActionSetsAttachInfo& info);
	XrResult SyncActions(const XrActionsSyncInfo& info,
						 std::optional<std::array<ControllerControls, 2>> nativeControls = std::nullopt);
	XrResult ApplyHaptic(const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback);
	XrResult StopHaptic(const XrHapticActionInfo* info);
	SessionSnapshot Snapshot() const;
	std::shared_ptr<Clock> ClockSource() const;
	FrameScheduler& Scheduler();
	FrameHistory& History();
	XrResult EnableImageHistory();
	bool SupportsImageHistory() const;
	FrameImageHistory& ImageHistory();

	FramePreviewHistory& PreviewHistory() noexcept
	{
		return m_previewHistory;
	}

	VirtualDisplay& Display();
	SnapshotStore& Captures();
	XrResult ConfigureCapturePool(FramePool pool, bool layerViews = false, CapturePromoter promoter = {},
								  bool depthViews = false, bool filteredComposition = false);
	SnapshotAdmission StartImageRecording(CaptureTarget target, std::uint32_t capacity, std::uint64_t frames = 0,
										  std::span<CaptureId> captures = {});
	void StopImageRecording() noexcept;
	ImageRecordingSnapshot ImageRecording() const;
	CaptureId RefillImageRecording(CaptureId released);
	CaptureReservation ReserveCapture(CaptureTarget target = {});
	CaptureBoundaryResult ReserveCaptureBundle(std::span<const CaptureTarget> targets);
	XrResult PrepareCaptureProducer(FrameKey frame, std::shared_ptr<FrameReservation>& producer,
									CaptureTarget target = {});
	std::size_t CancelCaptureReservations();
	CaptureBoundaryResult CaptureBoundary(std::optional<FrameKey> after = {});
	bool SupportsCapture(CaptureTarget target = {}) const;
	HapticState& Haptics();
	std::optional<HapticSnapshot> ObserveHaptics(SessionGeneration generation);
	InputState& Input();
	ActionSyncState& Actions();
	TrackingSystem& Tracking();
	SpaceSystem& Spaces();
	BodyTracking& Body();
	const EnabledExtensions& Extensions() const;
	void PublishTracking(RuntimeTime time, TrackingState state, const ViewSettings& settings = {}, FrameId frame = {});
	XrResult PublishInput(InputSnapshot input);
	CommandScheduler& Commands();
	CommandResult SubmitLiveInput(SessionGeneration generation, CommandSource source, const LiveInputSample& sample);
	bool SubmitDisplayPrediction(SessionGeneration generation, CommandSource source, std::uint64_t attachment,
								 std::uint64_t sequence, std::uint64_t clockRevision, RuntimeTime target,
								 Duration period);
	// Serializes CPU-only control mutation against Begin/End/Close. The callback
	// may access Commands/ReleaseCommandSource, but must not change lifecycle,
	// perform I/O, wait for GPU work or reenter a generation gate.
	bool WithCommandGeneration(SessionGeneration generation, const std::function<void()>& work,
							   bool requireRunning = true);
	void ReleaseCommandSource(CommandSource source, bool retire = false);
	void StopPoseSequence(CommandSource source);
	XrResult ConfigureCommands(std::uint64_t seed, std::optional<CommandConfiguration> initial = {},
							   bool overrideInitial = true);
	CommandConfiguration CommandConfigurationSnapshot() const;
	RecordingResult StartCommandRecording(bool drain = false, CommandSource owner = ApplicationAutomationSource);
	XrResult LocateViews(const XrViewLocateInfo& info, XrViewState* state, std::uint32_t capacity, std::uint32_t* count,
						 XrView* views);
	XrViewConfigurationType ActiveViewConfiguration() const;
	std::uint32_t ActiveViewCount() const;
	LocatedViews ViewsAt(RuntimeTime time, bool foveated = false) const;

private:
	void Close(std::optional<SessionGeneration> expectedGeneration);
	friend class DebugController;
	bool WithDebugGeneration(SessionGeneration generation, const std::function<void()>& work,
							 bool requireRunning = true);
	void Transition(XrSessionState state, RuntimeTime time);
	void WatchLiveInput(std::stop_token stop);
	XrSession m_handle;
	std::shared_ptr<Instance> m_instanceOwner;
	Instance& m_instance;
	std::mutex m_lifecycleMutex;
	mutable std::mutex m_mutex;
	std::mutex m_publicationMutex;
	SessionSnapshot m_state;
	std::shared_ptr<Clock> m_clock;
	FrameScheduler m_scheduler;
	FrameHistory m_history;
	FramePreviewHistory m_previewHistory;
	VirtualDisplay m_display;
	SessionCaptureCoordinator m_captures;
	SessionInteraction m_interaction;
	SessionPublication m_publication;
	SpaceSystem m_spaces;
	BodyTracking m_body;
	std::uint64_t m_revision = 0;
	bool m_liveInputFailed = false;
	std::optional<CommandSource> m_externalDisplaySource;
	std::jthread m_liveInputWorker;
};
} // namespace simxr
