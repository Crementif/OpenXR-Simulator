#include "runtime/Session.h"
#include "runtime/SpaceQueries.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace simxr
{
namespace
{
Instance& RetainedInstance(const std::shared_ptr<Instance>& instance)
{
	if (!instance)
		throw std::invalid_argument("Missing instance owner");
	return *instance;
}
} // namespace

Session::Session(XrSession handle, Instance& instance, std::shared_ptr<Clock> clock)
	: m_handle(handle), m_instance(instance), m_clock(std::move(clock)), m_scheduler(m_clock), m_display(m_clock),
	  m_captures(m_history), m_interaction(handle, instance), m_publication(instance, m_interaction.Actions()),
	  m_spaces(
		  handle, m_publication.Tracking(),
		  [this](XrAction action, XrPath path, RuntimeTime time)
		  {
			  return m_publication.ActionPose(action, path, time);
		  },
		  [this](XrAction action, XrPath path)
		  {
			  return m_publication.ValidatePoseAction(action, path);
		  },
		  &instance.Events()),
	  m_body(handle, m_publication.Tracking(), m_spaces, instance.Extensions())
{
	if (handle == XR_NULL_HANDLE)
		throw std::invalid_argument("Session requires a handle");
}

Session::Session(XrSession handle, std::shared_ptr<Instance> instance, std::shared_ptr<Clock> clock)
	: Session(handle, RetainedInstance(instance), std::move(clock))
{
	m_instanceOwner = std::move(instance);
}

Session::~Session()
{
	Close();
}

void Session::Transition(XrSessionState state, RuntimeTime time)
{
	m_interaction.SetFocused(state == XR_SESSION_STATE_FOCUSED, time);
	m_state.state = state;
	m_state.time = time;
	m_instance.Events().Push(SessionStateEvent{m_handle, state, time});
}

XrResult Session::Ready(RuntimeTime time)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	if (m_state.running)
		return XR_ERROR_SESSION_RUNNING;
	Transition(XR_SESSION_STATE_IDLE, time);
	Transition(XR_SESSION_STATE_READY, time);
	return XR_SUCCESS;
}

XrResult Session::Begin(const XrSessionBeginInfo& info, const FrameSchedulerConfig& config)
{
	if (info.type != XR_TYPE_SESSION_BEGIN_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info.primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
		(info.primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ||
		 !m_instance.Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)))
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	std::lock_guard lifecycle(m_lifecycleMutex);
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	if (m_state.running)
		return XR_ERROR_SESSION_RUNNING;
	if (m_state.state != XR_SESSION_STATE_READY)
		return XR_ERROR_SESSION_NOT_READY;
	if (config.epoch.value.count() <= 0)
		return XR_ERROR_TIME_INVALID;
	const auto layout = info.primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO
							? ViewLayout::StereoWithInset
							: ViewLayout::Stereo;
	SessionPublication::PreparedStart publication(m_publication, config.epoch, layout);
	auto timing = config;
	if (!timing.hostEpoch)
		timing.hostEpoch = m_clock->Now();
	m_state.generation = m_history.StartGeneration();
	try
	{
		m_captures.Start(m_state.generation);
		m_scheduler.Start(timing);
		m_display.Start(m_state.generation, {timing.epoch, timing.period}, timing.mode, timing.hostEpoch);
		if (XR_FAILED(m_spaces.StartGeneration(timing.epoch, publication.ReferenceSpaces())))
			throw std::invalid_argument("Invalid session reference-space origin");
	}
	catch (...)
	{
		m_scheduler.Stop();
		m_history.CancelPending();
		m_captures.Stop();
		throw;
	}
	++m_revision;
	m_interaction.Reset();
	publication.Commit();
	m_state.running = true;
	m_state.lastIssued = {};
	m_state.lastSubmitted = {};
	Transition(XR_SESSION_STATE_SYNCHRONIZED, config.epoch);
	Transition(XR_SESSION_STATE_VISIBLE, config.epoch);
	Transition(XR_SESSION_STATE_FOCUSED, config.epoch);
	return XR_SUCCESS;
}

XrResult Session::RequestExit(RuntimeTime time)
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (m_state.state != XR_SESSION_STATE_STOPPING)
		Transition(XR_SESSION_STATE_STOPPING, time);
	return XR_SUCCESS;
}

XrResult Session::End(RuntimeTime time)
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	std::unique_lock lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (m_state.state != XR_SESSION_STATE_STOPPING)
		return XR_ERROR_SESSION_NOT_STOPPING;
	++m_revision;
	m_scheduler.Stop();
	m_history.CancelPending();
	m_captures.Stop();
	m_state.running = false;
	m_publication.Stop();
	Transition(XR_SESSION_STATE_IDLE, time);
	Transition(XR_SESSION_STATE_EXITING, time);
	lock.unlock();
	m_display.Stop();
	return XR_SUCCESS;
}

XrResult Session::SetState(XrSessionState state, RuntimeTime time)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	if (state != XR_SESSION_STATE_SYNCHRONIZED && state != XR_SESSION_STATE_VISIBLE &&
		state != XR_SESSION_STATE_FOCUSED && state != XR_SESSION_STATE_STOPPING &&
		state != XR_SESSION_STATE_LOSS_PENDING)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (m_state.state != state)
		Transition(state, time);
	return XR_SUCCESS;
}

void Session::Close()
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	std::unique_lock lock(m_mutex);
	if (m_state.closed)
		return;
	++m_revision;
	m_state.closed = true;
	m_state.running = false;
	m_publication.Stop();
	m_scheduler.Stop();
	m_history.CancelPending();
	m_captures.Stop();
	m_interaction.LoseFocus(m_state.time);
	m_body.Clear();
	m_spaces.Clear();
	m_instance.Events().RemoveSession(m_handle);
	lock.unlock();
	m_display.Stop();
	m_captures.Retire();
}

XrResult Session::WaitFrame(FramePrediction& prediction)
{
	std::uint64_t revision;
	FrameKey traceFrame;
	RuntimeTime traceTime;
	std::optional<SessionPublication::FrameBoundary> boundary;
	{
		std::lock_guard lock(m_mutex);
		if (!m_state.running)
			return XR_ERROR_SESSION_NOT_RUNNING;
		revision = m_revision;
		traceFrame.generation = m_state.generation;
		traceTime = m_state.time;
		boundary.emplace(m_publication, m_state.state == XR_SESSION_STATE_FOCUSED,
						 m_spaces.RecordingState(m_state.time));
	}
	m_history.Trace(FrameTraceKind::WaitStarted, traceFrame, traceTime);
	std::unique_lock publication(m_publicationMutex, std::defer_lock);
	const auto prepareBoundary = [this, &boundary, &publication](const FramePrediction& candidate)
	{
		publication.lock();
		if (!m_spaces.CanPublish(candidate.displayTime))
			throw std::invalid_argument("Reference-space history exceeds the next frame boundary");
		const auto reserveHistory = [this, &candidate]
		{
			const auto reserveCaptures =
				[this](std::shared_ptr<const FrameRecord> issued, std::shared_ptr<const FrameRecord> cancelled)
			{
				m_captures.BindIssued(std::move(issued), std::move(cancelled));
			};
			m_history.Issue(candidate, reserveCaptures);
		};
		boundary->Reserve(candidate, reserveHistory);
	};
	auto result = m_scheduler.WaitFrame(prediction, prepareBoundary);
	m_history.Trace(FrameTraceKind::WaitReturned,
					{traceFrame.generation, XR_SUCCEEDED(result) ? prediction.id : FrameId{}},
					XR_SUCCEEDED(result) ? prediction.displayTime : traceTime, result);
	if (XR_FAILED(result))
	{
		m_history.Trace(FrameTraceKind::ApiError, traceFrame, traceTime, result, FrameDiagnosticCategory::ApiError);
		return result;
	}
	SessionGeneration generation;
	{
		std::lock_guard lock(m_mutex);
		if (m_state.closed || revision != m_revision)
			return XR_ERROR_SESSION_NOT_RUNNING;
		m_state.lastIssued = prediction;
		m_state.time = prediction.displayTime;
		generation = m_state.generation;
		if (const auto transition = boundary->StateTransition(m_state.state))
			Transition(*transition, prediction.displayTime);
		boundary->Publish();
		const auto& applied = boundary->Applied();
		for (const auto& outcome : applied.outcomes)
			m_history.Trace(outcome.result == CommandResult::Applied ? FrameTraceKind::CommandsApplied
																	 : FrameTraceKind::CommandRejected,
							{generation, prediction.id}, prediction.displayTime, XR_SUCCESS,
							FrameDiagnosticCategory::Command, outcome.transaction.id);
		bool spaceChangeRequested = false;
		for (const auto& outcome : applied.outcomes)
			if (outcome.result == CommandResult::Applied)
				for (const auto& command : outcome.transaction.commands)
				{
					const auto* scenario = std::get_if<SetScenario>(&command);
					spaceChangeRequested |=
						std::holds_alternative<SetReferenceSpaces>(command) ||
						(scenario && scenario->scenario == FailureScenario::Recenter && scenario->enabled);
				}
		const auto restored =
			m_spaces.Restore(prediction.displayTime, applied.configuration.referenceSpaces, spaceChangeRequested);
		if (XR_FAILED(restored))
			throw std::logic_error("Accepted reference-space state exceeds its publication boundary");
	}
	m_display.AdvanceFromFrame(generation, {prediction.displayTime.value - prediction.displayPeriod});
	return result;
}

bool Session::WithDebugGeneration(SessionGeneration generation, const std::function<void()>& work, bool requireRunning)
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	{
		std::lock_guard lock(m_mutex);
		if (m_state.closed || m_state.generation != generation || (requireRunning && !m_state.running))
			return false;
	}
	work();
	return true;
}

XrResult Session::BeginFrame()
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	const auto before = m_scheduler.Snapshot();
	if (!before.waited)
	{
		m_history.Trace(FrameTraceKind::ApiError, {m_state.generation, {}}, m_state.time, XR_ERROR_CALL_ORDER_INVALID,
						FrameDiagnosticCategory::ApiError);
		return XR_ERROR_CALL_ORDER_INVALID;
	}
	m_history.Begin(before.waited->id, before.begun ? std::optional{before.begun->id} : std::nullopt);
	m_captures.ObserveFrame({m_state.generation, before.waited->id});
	if (before.begun)
		m_captures.ObserveFrame({m_state.generation, before.begun->id});
	return m_scheduler.BeginFrame();
}

XrResult Session::EndFrame(FramePrediction& prediction, const XrFrameEndInfo* info,
						   std::shared_ptr<const FrameLayers> prepared, std::shared_ptr<const FrameOutput> output,
						   const std::function<XrResult()>& submit, std::span<const CaptureOutput> captures,
						   FrameSubmissionPolicy policy)
{
	auto captureSubmission = m_captures.PrepareSubmission(output, captures);
	if (XR_FAILED(captureSubmission.Validation()))
		return captureSubmission.Validation();
	const std::function<XrResult()> noSubmit;
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!captures.empty() && !m_captures.Supports({CaptureKind::LayerView}))
		return XR_ERROR_FEATURE_UNSUPPORTED;
	const auto pending = m_scheduler.Snapshot().begun;
	if (!pending)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (info && (info->type != XR_TYPE_FRAME_END_INFO || info->displayTime <= 0))
		return XR_ERROR_VALIDATION_FAILURE;
	if ((prepared &&
		 (!info || prepared->displayTime.value.count() != info->displayTime ||
		  prepared->blendMode != info->environmentBlendMode || prepared->layers.size() != info->layerCount)) ||
		(!prepared && info && info->layerCount))
		return XR_ERROR_VALIDATION_FAILURE;
	const FrameKey key{m_state.generation, pending->id};
	const auto preparedCapture = captureSubmission.Prepare(key);
	if (XR_FAILED(preparedCapture))
		return preparedCapture;
	const bool live = policy == FrameSubmissionPolicy::Live && !captureSubmission.Requested();
	if (captureSubmission.Requested() && info && info->layerCount && !output)
		return XR_ERROR_LIMIT_REACHED;
	if ((output && (!info || !info->layerCount)) || (submit && !output && (!live || !info || !info->layerCount)))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto promoted = captureSubmission.Promote();
	if (XR_FAILED(promoted))
		return promoted;
	FrameSubmissionState submission;
	submission.displayTime = info ? RuntimeTime{Duration{info->displayTime}} : pending->displayTime;
	submission.layerCount = info ? info->layerCount : 0;
	submission.blendMode = info ? info->environmentBlendMode : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	if (prepared)
		submission.layers = std::move(prepared);
	else
		submission.layers = std::make_shared<const FrameLayers>(submission.displayTime, submission.blendMode,
																std::vector<LayerDescription>{});
	m_publication.CaptureFrame(pending->id, submission);
	submission.haptics = m_interaction.Haptics().Snapshot(submission.displayTime);
	if (!submission.layerCount)
		submission.outputDisposition = FrameOutputDisposition::Clear;
	else if (output)
		submission.outputDisposition = FrameOutputDisposition::Submitted;
	else
		submission.outputDisposition = FrameOutputDisposition::Unavailable;
	bool runNative = true;
	if (live && submission.layerCount && !output)
	{
		submission.outputDisposition = FrameOutputDisposition::Dropped;
		submission.dropReason = FrameDropReason::NativeCapacity;
		runNative = false;
	}
	XrResult result = XR_SUCCESS;
	struct CommitFailure
	{
		XrResult result;
	};
	auto commit = [&]
	{
		const auto accepted = m_history.Submit(pending->id, std::move(submission), runNative ? submit : noSubmit);
		if (XR_FAILED(accepted))
			throw CommitFailure{accepted};
		result = m_scheduler.EndFrame(prediction);
		if (XR_SUCCEEDED(result))
		{
			m_state.lastSubmitted = prediction.id;
			captureSubmission.Commit(m_history.Find({m_state.generation, prediction.id}), runNative);
		}
		return m_history.Find({m_state.generation, prediction.id});
	};
	if (submission.layerCount == 0 || output)
	{
		try
		{
			const bool liveClear = live && submission.layerCount == 0;
			const PresentationSubmission presented{{m_state.generation, pending->id}, submission.displayTime, output};
			const auto admission = liveClear ? m_display.SubmitLiveClearWithMetadata(presented, commit)
											 : m_display.SubmitWithMetadata(presented, commit);
			if (admission == PresentationAdmission::QueueFull)
			{
				if (!live)
					return XR_ERROR_LIMIT_REACHED;
				submission.outputDisposition = FrameOutputDisposition::Dropped;
				submission.dropReason = FrameDropReason::PresentationCapacity;
				runNative = false;
				commit();
				return result;
			}
			if (admission != PresentationAdmission::Accepted)
				return XR_ERROR_RUNTIME_FAILURE;
		}
		catch (const CommitFailure& failure)
		{
			return failure.result;
		}
	}
	else
		commit();
	return result;
}

XrResult Session::Attach(const XrSessionActionSetsAttachInfo& info)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	return m_interaction.Attach(info, m_publication.Input().Snapshot());
}

XrResult Session::SyncActions(const XrActionsSyncInfo& info,
							  std::optional<std::array<ControllerControls, 2>> nativeControls)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	auto input = m_publication.Input().Snapshot();
	if (nativeControls)
	{
		input.time = std::max(input.time, m_state.time);
		input.controllers = *nativeControls;
		const auto published = m_publication.PublishInput(std::move(input));
		if (XR_FAILED(published))
			return published;
		input = m_publication.Input().Snapshot();
	}
	const auto result = m_interaction.Sync(info, input, m_state.time, m_state.state == XR_SESSION_STATE_FOCUSED);
	if (XR_SUCCEEDED(result))
		m_interaction.PublishProfileChange(m_publication.TakeProfileChange(), m_state.running);
	return result;
}

XrResult Session::ApplyHaptic(const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	return m_interaction.ApplyHaptic(info, feedback, std::max(m_state.time, m_publication.Input().Snapshot().time),
									 m_state.state == XR_SESSION_STATE_FOCUSED);
}

XrResult Session::StopHaptic(const XrHapticActionInfo* info)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	return m_interaction.StopHaptic(info, std::max(m_state.time, m_publication.Input().Snapshot().time),
									m_state.state == XR_SESSION_STATE_FOCUSED);
}

SessionSnapshot Session::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_state;
}

std::shared_ptr<Clock> Session::ClockSource() const
{
	return m_clock;
}

FrameScheduler& Session::Scheduler()
{
	return m_scheduler;
}

FrameHistory& Session::History()
{
	return m_history;
}

VirtualDisplay& Session::Display()
{
	return m_display;
}

SnapshotStore& Session::Captures()
{
	return m_captures.Snapshots();
}

XrResult Session::ConfigureCapturePool(FramePool pool, bool layerViews, CapturePromoter promoter)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	if (m_state.running)
		return XR_ERROR_CALL_ORDER_INVALID;
	return m_captures.Configure(std::move(pool), layerViews, std::move(promoter));
}

CaptureReservation Session::ReserveCapture(CaptureTarget target)
{
	{
		std::lock_guard lock(m_mutex);
		if (!m_state.running || m_state.closed)
			return {SnapshotAdmission::Closed, {}};
		if (!m_captures.Supports(target))
			return {SnapshotAdmission::Unsupported, {}};
	}
	m_captures.Collect();
	std::lock_guard lock(m_mutex);
	if (!m_state.running || m_state.closed)
		return {SnapshotAdmission::Closed, {}};
	if (!m_scheduler.Snapshot().paused)
		return {SnapshotAdmission::Invalid, {}};
	return m_captures.Reserve(target);
}

XrResult Session::PrepareCaptureProducer(FrameKey frame, std::shared_ptr<FrameReservation>& producer,
										 CaptureTarget target)
{
	{
		std::lock_guard lock(m_mutex);
		if (!m_state.running || m_state.closed || frame.generation != m_state.generation)
			return XR_ERROR_SESSION_NOT_RUNNING;
		if (target != CaptureTarget{} && !m_captures.Supports(target))
			return XR_ERROR_FEATURE_UNSUPPORTED;
	}
	m_captures.Collect();
	std::lock_guard lock(m_mutex);
	if (!m_state.running || m_state.closed || frame.generation != m_state.generation)
		return XR_ERROR_SESSION_NOT_RUNNING;
	auto prepared = m_captures.PrepareProducer(frame, target);
	producer = std::move(prepared.second);
	return prepared.first;
}

std::size_t Session::CancelCaptureReservations()
{
	std::lock_guard lock(m_mutex);
	m_scheduler.CancelPermits();
	return m_captures.CancelQueued();
}

CaptureBoundaryResult Session::CaptureBoundary(std::optional<FrameKey> after)
{
	auto boundary = m_captures.PrepareBoundary();
	{
		std::lock_guard lock(m_mutex);
		if (m_state.closed || !m_state.running)
			return {};
	}
	m_captures.Collect();
	std::lock_guard lock(m_mutex);
	if (m_state.closed || !m_state.running)
		return {};
	const auto pipeline = m_scheduler.Snapshot();
	if (!pipeline.paused)
		return {SnapshotAdmission::Invalid, {}};
	return boundary.Capture(m_state.generation, pipeline, m_display.Retain(), after);
}

bool Session::SupportsCapture(CaptureTarget target) const
{
	std::lock_guard lock(m_mutex);
	return !m_state.closed && m_captures.Supports(target);
}

HapticState& Session::Haptics()
{
	return m_interaction.Haptics();
}

InputState& Session::Input()
{
	return m_publication.Input();
}

ActionSyncState& Session::Actions()
{
	return m_interaction.Actions();
}

TrackingSystem& Session::Tracking()
{
	return m_publication.Tracking();
}

SpaceSystem& Session::Spaces()
{
	return m_spaces;
}

BodyTracking& Session::Body()
{
	return m_body;
}

const EnabledExtensions& Session::Extensions() const
{
	return m_instance.Extensions();
}

CommandScheduler& Session::Commands()
{
	return m_publication.Commands();
}

bool Session::WithCommandGeneration(SessionGeneration generation, const std::function<void()>& work)
{
	return WithDebugGeneration(generation, work);
}

void Session::ReleaseCommandSource(CommandSource source, bool retire)
{
	std::lock_guard lock(m_mutex);
	if (m_publication.ReleaseSource(source, retire) && m_state.running &&
		(m_state.state == XR_SESSION_STATE_VISIBLE || m_state.state == XR_SESSION_STATE_SYNCHRONIZED))
		Transition(XR_SESSION_STATE_FOCUSED, m_state.time);
}

XrResult Session::ConfigureCommands(std::uint64_t seed, std::optional<CommandConfiguration> initial,
									bool overrideInitial)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	if (m_state.running)
		return XR_ERROR_SESSION_RUNNING;
	m_publication.Configure(seed, std::move(initial), overrideInitial);
	return XR_SUCCESS;
}

void Session::StartCommandRecording()
{
	std::scoped_lock lock(m_mutex, m_publicationMutex);
	if (m_state.closed || !m_state.running)
		throw std::logic_error("Cannot record a stopped session");
	m_publication.StartRecording(m_spaces.RecordingState(m_state.time));
}

CommandConfiguration Session::CommandConfigurationSnapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_publication.ConfigurationSnapshot(m_state.lastIssued);
}

XrResult Session::PublishInput(InputSnapshot input)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	return m_publication.PublishInput(std::move(input));
}

void Session::PublishTracking(RuntimeTime time, TrackingState state, const ViewSettings& settings, FrameId frame)
{
	const ViewConfiguration validatedViews(ViewLayout::Stereo, settings);
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		throw std::logic_error("Session is closed");
	if (frame.value && frame != m_state.lastIssued.id)
		throw std::invalid_argument("Tracking frame is not current");
	m_publication.PublishTracking(time, std::move(state), settings, m_state.state == XR_SESSION_STATE_FOCUSED);
}

XrResult Session::LocateViews(const XrViewLocateInfo& info, XrViewState* state, std::uint32_t capacity,
							  std::uint32_t* count, XrView* views)
{
	if (info.viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
		(info.viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ||
		 !m_instance.Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)))
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return XR_ERROR_SESSION_LOST;
	const auto active = m_publication.Layout() == ViewLayout::StereoWithInset
							? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO
							: XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (info.viewConfigurationType != active)
		return XR_ERROR_VALIDATION_FAILURE;
	const RuntimeTime time{Duration{info.displayTime}};
	auto sample = m_publication.Tracking().Sample(time).value_or(TrackingSnapshot{});
	return simxr::LocateViews(m_spaces, m_publication.ConfigurationAt(time), sample, info, state, capacity, count,
							  views);
}

XrViewConfigurationType Session::ActiveViewConfiguration() const
{
	std::lock_guard lock(m_mutex);
	return m_publication.Layout() == ViewLayout::StereoWithInset ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO
																 : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
}

std::uint32_t Session::ActiveViewCount() const
{
	std::lock_guard lock(m_mutex);
	return m_publication.Layout() == ViewLayout::StereoWithInset ? 4 : 2;
}

LocatedViews Session::ViewsAt(RuntimeTime time, bool foveated) const
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed)
		return {time};
	return m_publication.ViewsAt(time, foveated);
}
} // namespace simxr
