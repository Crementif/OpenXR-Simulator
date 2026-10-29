#include "control/DebugProtocol.h"
#include "Pch.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"
#include "graphics/SnapshotBudget.h"
#include "runtime/Session.h"

namespace simxr::control
{
static std::uint32_t ReadU32(const Message& message, std::uint16_t name, std::optional<std::uint32_t> fallback = {})
{
	const auto* field = message.Find(name);
	if (!field && fallback)
		return *fallback;
	const auto value = field ? field->AsU32() : std::nullopt;
	if (!value)
		throw ControlRequestError{ErrorCode::Invalid, "Missing or invalid uint32 field"};
	return *value;
}

static Field Nested(std::uint16_t name, std::vector<Field> fields)
{
	return Field::Bytes(name, EncodeFields(fields));
}

static Field EncodePrediction(std::uint16_t name, const FramePrediction& prediction)
{
	return Nested(name,
				  {Field::U64(tag::FrameId, prediction.id.value), Field::U64(tag::Tick, prediction.displayTick.value),
				   Field::I64(tag::Time, prediction.displayTime.value.count()),
				   Field::I64(tag::Period, prediction.displayPeriod.count())});
}

static Field EncodeCaptureIds(std::uint16_t name, const std::vector<CaptureId>& ids)
{
	std::vector<Field> fields;
	for (const auto id : ids)
		fields.push_back(Field::U64(static_cast<std::uint16_t>(fields.size() + 1), id.value));
	return Nested(name, std::move(fields));
}

Field EncodeCaptureTarget(CaptureTarget target)
{
	TargetKind kind;
	switch (target.kind)
	{
	case CaptureKind::ComposedEyes:
		kind = TargetKind::ComposedEyes;
		break;
	case CaptureKind::LayerView:
		kind = TargetKind::LayerView;
		break;
	case CaptureKind::LayerDepth:
		kind = TargetKind::LayerDepth;
		break;
	case CaptureKind::LayerDepthLinear:
		kind = TargetKind::LayerDepthLinear;
		break;
	case CaptureKind::ProjectionLayers:
		kind = TargetKind::ProjectionLayers;
		break;
	case CaptureKind::QuadLayers:
		kind = TargetKind::QuadLayers;
		break;
	case CaptureKind::VirtualDisplay:
		kind = TargetKind::VirtualDisplay;
		break;
	default:
		throw ControlRequestError{ErrorCode::Internal, "Unknown capture kind"};
	}
	return Nested(tag::Target, {Field::U32(tag::CaptureKind, static_cast<std::uint32_t>(kind)),
								Field::U32(tag::Layer, target.layer), Field::U32(tag::View, target.view)});
}

CaptureTarget DecodeCaptureTarget(const Message& request)
{
	const auto* field = request.Find(tag::Target);
	if (!field)
		return {};
	auto nested = DecodeFields(field->value);
	if (nested.error != DecodeError::None)
		throw ControlRequestError{ErrorCode::Invalid, "Invalid target fields"};
	Message target;
	target.fields = std::move(nested.fields);
	constexpr std::array known{tag::CaptureKind, tag::Layer, tag::View};
	if (target.ValidateFields(known))
		throw ControlRequestError{ErrorCode::UnsupportedField, "Unknown required target field"};
	const auto kind = ReadU32(target, tag::CaptureKind);
	const auto layer = ReadU32(target, tag::Layer, 0), view = ReadU32(target, tag::View, 0);
	switch (static_cast<TargetKind>(kind))
	{
	case TargetKind::ComposedEyes:
		if (layer || view)
			break;
		return {};
	case TargetKind::LayerView:
		return {CaptureKind::LayerView, layer, view};
	case TargetKind::LayerDepth:
		return {CaptureKind::LayerDepth, layer, view};
	case TargetKind::LayerDepthLinear:
		return {CaptureKind::LayerDepthLinear, layer, view};
	case TargetKind::ProjectionLayers:
		if (!layer && !view)
			return {CaptureKind::ProjectionLayers};
		break;
	case TargetKind::QuadLayers:
		if (!layer && !view)
			return {CaptureKind::QuadLayers};
		break;
	case TargetKind::VirtualDisplay:
		if (layer || view)
			break;
		return {CaptureKind::VirtualDisplay};
	}
	throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported capture target"};
}

static DebugCommand DecodeCommandKind(std::uint32_t kind)
{
	switch (static_cast<CommandKind>(kind))
	{
	case CommandKind::Pause:
		return DebugCommand::Pause;
	case CommandKind::Step:
		return DebugCommand::Step;
	case CommandKind::StrictAdmission:
		return DebugCommand::StrictAdmission;
	case CommandKind::DisplayStep:
		return DebugCommand::DisplayStep;
	case CommandKind::Resume:
		return DebugCommand::Resume;
	case CommandKind::Cancel:
		return DebugCommand::Cancel;
	case CommandKind::Release:
		return DebugCommand::Release;
	case CommandKind::StartImageRecording:
		return DebugCommand::StartImageRecording;
	case CommandKind::ConfigureSnapshots:
		return DebugCommand::ConfigureSnapshots;
	case CommandKind::PinSnapshot:
		return DebugCommand::PinSnapshot;
	case CommandKind::UnpinSnapshot:
		return DebugCommand::UnpinSnapshot;
	case CommandKind::StopImageRecording:
		return DebugCommand::StopImageRecording;
	}
	throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported debug command"};
}

static const char* Name(DebugPhase value)
{
	switch (value)
	{
	case DebugPhase::Queued:
		return "queued";
	case DebugPhase::Pending:
		return "pending";
	case DebugPhase::Complete:
		return "complete";
	case DebugPhase::Failed:
		return "failed";
	case DebugPhase::Cancelled:
		return "cancelled";
	case DebugPhase::Interrupted:
		return "interrupted";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown operation phase"};
}

static const char* Name(DebugProgress value)
{
	switch (value)
	{
	case DebugProgress::Queued:
		return "queued";
	case DebugProgress::Pausing:
		return "pausing";
	case DebugProgress::Paused:
		return "paused";
	case DebugProgress::WaitingSubmission:
		return "waiting_submission";
	case DebugProgress::Issued:
		return "issued";
	case DebugProgress::Begun:
		return "begun";
	case DebugProgress::GpuPending:
		return "gpu_pending";
	case DebugProgress::SnapshotReady:
		return "snapshot_ready";
	case DebugProgress::Clear:
		return "clear";
	case DebugProgress::Empty:
		return "empty";
	case DebugProgress::Discarded:
		return "discarded";
	case DebugProgress::PipelineStall:
		return "pipeline_stall";
	case DebugProgress::Displayed:
		return "displayed";
	case DebugProgress::Resumed:
		return "resumed";
	case DebugProgress::Released:
		return "released";
	case DebugProgress::Cancelled:
		return "cancelled";
	case DebugProgress::RetentionFailure:
		return "retention_failure";
	case DebugProgress::Unsupported:
		return "unsupported";
	case DebugProgress::Busy:
		return "busy";
	case DebugProgress::SessionEnded:
		return "session_ended";
	case DebugProgress::LeaseExpired:
		return "lease_expired";
	case DebugProgress::RecordingStarted:
		return "recording_started";
	case DebugProgress::RecordingStopped:
		return "recording_stopped";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown operation progress"};
}

const char* SnapshotPhaseName(SnapshotPhase value)
{
	switch (value)
	{
	case SnapshotPhase::Reserved:
		return "reserved";
	case SnapshotPhase::Issued:
		return "issued";
	case SnapshotPhase::Begun:
		return "begun";
	case SnapshotPhase::GpuPending:
		return "gpu_pending";
	case SnapshotPhase::Ready:
		return "ready";
	case SnapshotPhase::Clear:
		return "clear";
	case SnapshotPhase::Empty:
		return "empty";
	case SnapshotPhase::Discarded:
		return "discarded";
	case SnapshotPhase::Cancelled:
		return "cancelled";
	case SnapshotPhase::Failed:
		return "failed";
	case SnapshotPhase::Unavailable:
		return "unavailable";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown snapshot phase"};
}

static const char* Name(FrameStage value)
{
	switch (value)
	{
	case FrameStage::Issued:
		return "issued";
	case FrameStage::Begun:
		return "begun";
	case FrameStage::Submitted:
		return "submitted";
	case FrameStage::Discarded:
		return "discarded";
	case FrameStage::Cancelled:
		return "cancelled";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown frame stage"};
}

static const char* Name(FrameOutputDisposition value)
{
	switch (value)
	{
	case FrameOutputDisposition::Unavailable:
		return "unavailable";
	case FrameOutputDisposition::Submitted:
		return "submitted";
	case FrameOutputDisposition::Clear:
		return "clear";
	case FrameOutputDisposition::Dropped:
		return "dropped";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown frame output disposition"};
}

static const char* Name(FrameDropReason value)
{
	switch (value)
	{
	case FrameDropReason::None:
		return "none";
	case FrameDropReason::NativeCapacity:
		return "native_capacity";
	case FrameDropReason::PresentationCapacity:
		return "presentation_capacity";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown frame drop reason"};
}

static const char* Name(PresentationKind value)
{
	switch (value)
	{
	case PresentationKind::NewFrame:
		return "new_frame";
	case PresentationKind::Repeat:
		return "repeat";
	case PresentationKind::Clear:
		return "clear";
	case PresentationKind::Empty:
		return "empty";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown presentation kind"};
}

static const char* Name(SnapshotAdmission value)
{
	switch (value)
	{
	case SnapshotAdmission::Accepted:
		return "accepted";
	case SnapshotAdmission::Capacity:
		return "capacity";
	case SnapshotAdmission::Closed:
		return "closed";
	case SnapshotAdmission::Unsupported:
		return "unsupported";
	case SnapshotAdmission::Invalid:
		return "invalid";
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown snapshot admission"};
}

// The applied device input and action activity of one frame, so the companion
// can draw input over time and cut takes from any range. Fields 1-5 are head and
// hands, 6 body, 7 gaze and 8-11 the tracker slots; body overrides stay sparse.
static Field EncodeAppliedInput(const FrameSubmissionState& submission, bool trackerCollections, bool bodyObservations)
{
	const auto& applied = *submission.commands;
	const auto& configuration = applied.configuration;
	const auto& tracking = configuration.tracking;
	if (tracking.bodyObservation && !bodyObservations)
		throw ControlRequestError{ErrorCode::UnsupportedField, "Body observations require protocol minor 11"};
	if (!trackerCollections && tracking.trackers.size() != 4)
		throw ControlRequestError{ErrorCode::UnsupportedField,
								  "Tracker inventory requires collection transport support"};
	std::vector<Field> fields{EncodeSimulationCommand(SetHeadPose{tracking.head}, 1),
							  EncodeSimulationCommand(SetControllerPose{0, tracking.controllers[0]}, 2),
							  EncodeSimulationCommand(SetControllerPose{1, tracking.controllers[1]}, 3),
							  EncodeSimulationCommand(SetControllerControls{0, configuration.input.controllers[0]}, 4),
							  EncodeSimulationCommand(SetControllerControls{1, configuration.input.controllers[1]}, 5),
							  EncodeSimulationCommand(SetBodyControls{tracking.body, tracking.bodyObservation}, 6),
							  EncodeSimulationCommand(SetGaze{tracking.gaze}, 7)};
	if (trackerCollections)
		fields.push_back(EncodeSimulationCommand(SetTrackers{tracking.trackers}, tag::TrackerCollection));
	else
		for (std::uint32_t slot = 0; slot < tracking.trackers.size(); ++slot)
			fields.push_back(EncodeSimulationCommand(SetTracker{slot, tracking.trackers[slot]},
													 static_cast<std::uint16_t>(8 + slot)));
	std::vector<Field> context{
		EncodeSimulationCommand(SetViewSettings{configuration.views}, 1),
		EncodeSimulationCommand(SetBodyProportions{configuration.proportions}, 2),
		EncodeSimulationCommand(SetControllerProfile{0, configuration.input.preferredProfiles[0]}, 3),
		EncodeSimulationCommand(SetControllerProfile{1, configuration.input.preferredProfiles[1]}, 4),
		EncodeSimulationCommand(SetReferenceSpaces{configuration.referenceSpaces}, 5)};
	if (tracking.eyeViews)
		context.push_back(EncodeSimulationCommand(SetEyeViews{tracking.eyeViews}, 6));
	fields.push_back(Nested(tag::InputContext, context));
	fields.push_back(Field::U64(tag::Seed, applied.seed));
	std::uint32_t scenarios = 0, owned = 0, active = 0;
	for (std::size_t index = 0; index < applied.scenarios.size(); ++index)
		if (applied.scenarios[index])
			scenarios |= 1u << index;
	for (std::size_t index = 0; index < applied.owners.size(); ++index)
		if (applied.owners[index] && applied.owners[index]->kind != CommandSourceKind::Manual)
			owned |= 1u << index;
	for (const auto& action : submission.actions.actions)
		for (const auto& [path, value] : action.values)
			if (value.active && (value.boolean || value.scalar != 0 || value.vector.x != 0 || value.vector.y != 0))
				++active;
	fields.push_back(Field::U32(tag::Scenario, scenarios));
	fields.push_back(Field::U32(tag::Ownership, owned));
	fields.push_back(Field::U32(tag::ActiveActions, active));
	return Nested(tag::AppliedInput, std::move(fields));
}

static Field EncodeFrameMetadata(const FrameRecord& frame, bool trackerCollections = false,
								 bool bodyObservations = false)
{
	std::vector<Field> fields{
		Field::U64(tag::Generation, frame.key.generation.value), Field::U64(tag::FrameId, frame.key.frame.value),
		EncodePrediction(tag::Prediction, frame.prediction), Field::Text(tag::Stage, Name(frame.stage))};
	if (frame.submission)
	{
		const auto& submission = *frame.submission;
		fields.push_back(
			Nested(tag::Submission, {Field::I64(tag::Time, submission.displayTime.value.count()),
									 Field::U32(tag::LayerCount, submission.layerCount),
									 Field::Text(tag::OutputDisposition, Name(submission.outputDisposition)),
									 Field::Text(tag::DropReason, Name(submission.dropReason))}));
		if (const auto cpu = frame.CpuFrameTime())
			fields.push_back(Field::I64(tag::CpuFrameTime, cpu->count()));
		if (submission.gpuTiming)
		{
			const auto gpu = submission.gpuTiming->compositorNanoseconds.load();
			if (gpu >= 0)
				fields.push_back(Field::I64(tag::CompositorGpuTime, gpu));
		}
		if (submission.commands && submission.commands->configurationAvailable)
			fields.push_back(EncodeAppliedInput(submission, trackerCollections, bodyObservations));
	}
	return Nested(tag::Frame, std::move(fields));
}

static ErrorCode AdmissionError(DebugAdmission value)
{
	switch (value)
	{
	case DebugAdmission::Accepted:
	case DebugAdmission::Duplicate:
		return ErrorCode::Ok;
	case DebugAdmission::Conflict:
	case DebugAdmission::Retired:
		return ErrorCode::Conflict;
	case DebugAdmission::LeaseRequired:
		return ErrorCode::LeaseRequired;
	case DebugAdmission::Capacity:
		return ErrorCode::Capacity;
	case DebugAdmission::Invalid:
		return ErrorCode::Invalid;
	case DebugAdmission::Closed:
		return ErrorCode::Closed;
	}
	return ErrorCode::Internal;
}

Field EncodeFrameKey(std::uint16_t name, FrameKey key)
{
	return Nested(name, {Field::U64(tag::Generation, key.generation.value), Field::U64(tag::FrameId, key.frame.value)});
}

Field EncodeDisplayTick(std::uint16_t name, const PresentationTick& tick)
{
	std::vector<Field> fields{Field::U64(tag::Tick, tick.tick.value), Field::I64(tag::Time, tick.time.value.count()),
							  Field::Text(tag::Kind, Name(tick.kind)),
							  Field::U64(tag::SkippedTicks, tick.skippedTicks)};
	fields.push_back(
		Field::Text(tag::PresentationPolicy, tick.policy == PresentationPolicy::Rotational ? "rotational" : "repeat"));
	fields.push_back(Field::Bool(tag::Reprojected, tick.reprojected));
	fields.push_back(Field::I64(tag::ReprojectionResult, tick.reprojectionResult));
	fields.push_back(Field::I64(tag::ReprojectionSubmissionTime, tick.reprojectionSubmissionTime.count()));
	fields.push_back(Field::I64(tag::ReprojectionCompletionLatency, tick.reprojectionCompletionLatency.count()));
	if (tick.displayHead)
	{
		const auto& head = *tick.displayHead;
		const auto& p = head.pose.position;
		const auto& q = head.pose.orientation;
		fields.push_back(Field::Bytes(
			tag::DisplayHead,
			InspectionValue::Object(
				{{"position", InspectionValue::Array({InspectionValue::Float(p.x), InspectionValue::Float(p.y),
													  InspectionValue::Float(p.z)})},
				 {"orientation", InspectionValue::Array({InspectionValue::Float(q.x), InspectionValue::Float(q.y),
														 InspectionValue::Float(q.z), InspectionValue::Float(q.w)})},
				 {"flags", InspectionValue::Unsigned(head.flags)},
				 {"sample_time", InspectionValue::Signed(head.sampleTime.value.count())}})
				.Encode()));
	}
	if (tick.imageTick)
		fields.push_back(Field::U64(tag::ImageTick, tick.imageTick->value));
	if (tick.source)
		fields.push_back(EncodeFrameKey(tag::Source, *tick.source));
	return Nested(name, std::move(fields));
}

DebugRequest DecodeDebugRequest(const Message& request)
{
	DebugRequest command;
	command.id = {RequireRequestU64(request, tag::CommandId)};
	command.command = DecodeCommandKind(ReadU32(request, tag::CommandKind));
	command.snapshotBudgetMiB = ReadU32(request, tag::SnapshotBudgetMiB, SnapshotBudget::DefaultMiB);
	command.lookahead = ReadU32(request, tag::Lookahead, 1);
	command.target = DecodeCaptureTarget(request);
	if (const auto* targets = request.Find(tag::AdditionalTargets))
	{
		const auto decoded = DecodeFields(targets->value);
		if (decoded.error != DecodeError::None || decoded.fields.empty() || decoded.fields.size() > 7)
			throw ControlRequestError{ErrorCode::Invalid, "Expected one to seven additional capture targets"};
		for (std::size_t index = 0; index < decoded.fields.size(); ++index)
		{
			auto field = decoded.fields[index];
			if (field.tag != index + 1)
				throw ControlRequestError{ErrorCode::Invalid, "Invalid additional target index"};
			field.tag = tag::Target;
			Message target;
			target.fields.push_back(std::move(field));
			command.additionalTargets.push_back(DecodeCaptureTarget(target));
		}
	}
	command.recordingCapacity = ReadU32(request, tag::Capacity, 2);
	if (request.Find(tag::Length))
		command.recordingFrames = RequireRequestU64(request, tag::Length);
	if (const auto field = request.Find(tag::AdvanceDisplay))
	{
		const auto advance = field->AsBool();
		if (!advance)
			throw ControlRequestError{ErrorCode::Invalid, "Invalid display advancement flag"};
		command.advanceDisplay = *advance;
	}
	command.generation = {RequireRequestU64(request, tag::Generation)};
	if (!command.generation.value)
		throw ControlRequestError{ErrorCode::Closed, "Session has not started"};
	if (request.Find(tag::OperationId))
		command.operation = {RequireRequestU64(request, tag::OperationId)};
	if (request.Find(tag::CaptureId))
		command.capture = {RequireRequestU64(request, tag::CaptureId)};
	return command;
}

std::optional<std::vector<std::string>> DecodeCompatibilityDeviations(const Message& status)
{
	const auto* field = status.Find(tag::CompatibilityDeviations);
	if (!field || field->value.size() > 8192)
		return {};
	const auto decoded = DecodeFields(field->value);
	if (decoded.error != DecodeError::None || decoded.fields.size() > 32)
		return {};
	std::vector<std::string> names;
	for (const auto& entry : decoded.fields)
	{
		const auto name = entry.AsText();
		if (entry.tag != names.size() + 1 || !name || name->empty() || name->size() > 128)
			return {};
		names.emplace_back(*name);
	}
	return names;
}

void EncodeDebuggerStatus(Message& response, const DebugControllerSnapshot& status, const SessionSnapshot& state,
						  const PresentationSnapshot& display, std::uint64_t capabilities)
{
	response.fields.push_back(Field::U64(tag::Capabilities, capabilities));
	// No API compatibility deviations are currently supported. Missing is unknown, not none.
	response.fields.push_back(Field::Bytes(tag::CompatibilityDeviations, {}));
	response.fields.push_back(Field::U32(tag::SnapshotBudgetMiB, SnapshotBudget::MiB()));
	response.fields.push_back(Field::U64(tag::SnapshotImageBytes, SnapshotBudget::ImageBytes()));
	response.fields.push_back(Field::I64(tag::State, state.state));
	response.fields.push_back(Field::Bool(tag::Running, state.running));
	response.fields.push_back(Field::Bool(tag::Closed, state.closed));
	response.fields.push_back(Field::Bool(tag::Manual, status.manual));
	response.fields.push_back(Field::U32(tag::Lookahead, status.maxLookahead));
	response.fields.push_back(Field::U64(tag::LedgerCount, status.commands));
	const std::array recordingPhases{"idle", "recording", "stopped", "overrun"};
	std::vector<Field> recording{
		Field::Text(tag::Phase, recordingPhases.at(static_cast<std::size_t>(status.imageRecording.phase))),
		Field::U64(tag::Capacity, status.imageRecording.capacity),
		Field::U64(tag::Length, status.imageRecording.captured),
		Field::U64(tag::TotalFrames, status.imageRecording.requested),
		EncodeCaptureTarget(status.imageRecording.target),
		EncodeCaptureIds(tag::Captures, status.imageRecording.captures)};
	if (status.imageRecording.missed)
		recording.push_back(EncodeFrameKey(tag::Frame, *status.imageRecording.missed));
	response.fields.push_back(Nested(tag::ImageRecording, std::move(recording)));
	std::vector<Field> pipeline{
		Field::Bool(tag::Running, status.pipeline.running), Field::Bool(tag::WaitPending, status.pipeline.waitPending),
		Field::Bool(tag::Paused, status.pipeline.paused), Field::U32(tag::Permits, status.pipeline.permits),
		Field::U64(tag::NextAdmission, status.pipeline.nextAdmission.value)};
	if (status.pipeline.waited)
		pipeline.push_back(EncodePrediction(tag::Waited, *status.pipeline.waited));
	if (status.pipeline.begun)
		pipeline.push_back(EncodePrediction(tag::Begun, *status.pipeline.begun));
	response.fields.push_back(Nested(tag::Pipeline, std::move(pipeline)));
	response.fields.push_back(Nested(tag::Retention, {Field::U64(tag::Capacity, status.retention.capacity),
													  Field::U64(tag::HeldSlots, status.retention.heldSlots),
													  Field::U64(tag::Retiring, status.retention.retiring),
													  Field::U64(tag::Pinned, status.retention.pinned),
													  EncodeCaptureIds(tag::Captures, status.retention.captures)}));
	std::vector<Field> presented{Field::U64(tag::LastSubmitted, display.lastSubmitted.value),
								 Field::U64(tag::CoalescedClears, display.coalescedLiveClears)};
	if (display.latest)
		presented.push_back(EncodeDisplayTick(tag::Display, *display.latest));
	response.fields.push_back(Nested(tag::Presentation, std::move(presented)));
}

void EncodeDebugOperation(Message& response, const DebugOperation& operation)
{
	response.fields.push_back(Field::U64(tag::OperationId, operation.id.value));
	response.fields.push_back(Field::U64(tag::CommandId, operation.request.id.value));
	response.fields.push_back(Field::Text(tag::Phase, Name(operation.phase)));
	response.fields.push_back(Field::Text(tag::Progress, Name(operation.progress)));
	response.fields.push_back(Field::I64(tag::Result, operation.result));
	response.fields.push_back(Field::Text(tag::RetentionResult, Name(operation.retention)));
	if (operation.frame)
		response.fields.push_back(EncodeFrameKey(tag::Frame, *operation.frame));
	if (operation.capture)
		response.fields.push_back(Field::U64(tag::CaptureId, operation.capture->value));
	if (operation.display)
		response.fields.push_back(EncodeDisplayTick(tag::Display, *operation.display));
	response.fields.push_back(EncodeCaptureIds(tag::Captures, operation.captures));
	response.fields.push_back(EncodeCaptureIds(tag::Diagnostics, operation.diagnostics));
	std::vector<Field> outstanding;
	for (const auto key : operation.outstanding)
		outstanding.push_back(EncodeFrameKey(static_cast<std::uint16_t>(outstanding.size() + 1), key));
	response.fields.push_back(Nested(tag::Outstanding, std::move(outstanding)));
}

void EncodeCaptureMetadata(Message& response, const SnapshotRecord& capture, bool trackerCollections)
{
	response.fields.push_back(Field::U64(tag::CaptureId, capture.id.value));
	response.fields.push_back(Field::Text(tag::Phase, SnapshotPhaseName(capture.phase)));
	response.fields.push_back(Field::Bool(tag::Pinned, capture.pinned));
	response.fields.push_back(Field::I64(tag::Result, capture.result));
	response.fields.push_back(EncodeCaptureTarget(capture.target));
	response.fields.push_back(
		Field::Bool(tag::ImageAvailable, capture.phase == SnapshotPhase::Ready && capture.output != nullptr));
	if (capture.metadata)
		response.fields.push_back(EncodeFrameMetadata(*capture.metadata, trackerCollections, response.minor >= 11));
	else if (capture.frame)
		response.fields.push_back(EncodeFrameKey(tag::Frame, *capture.frame));
	if (capture.display)
		response.fields.push_back(EncodeDisplayTick(tag::Display, *capture.display));
}

void EncodeDebugAdmission(Message& response, const DebugSubmission& admitted)
{
	response.fields.front() = Field::U32(tag::Error, static_cast<std::uint32_t>(AdmissionError(admitted.result)), true);
	if (admitted.operation.value)
		response.fields.push_back(Field::U64(tag::OperationId, admitted.operation.value));
	response.fields.push_back(Field::Bool(tag::Duplicate, admitted.result == DebugAdmission::Duplicate));
}

void EncodeFrameHistory(Message& response, const FrameHistorySnapshot& history, std::uint64_t after, std::size_t count,
						bool trackerCollections)
{
	std::vector<Field> frames;
	frames.reserve(std::min(count, history.frames.size()));
	std::size_t bytes = 0;
	constexpr std::size_t budget = MaxPayload - 4096;
	for (const auto& frame : history.frames)
	{
		if (frame->key.generation != history.generation || frame->key.frame.value <= after ||
			frame->stage == FrameStage::Issued || frame->stage == FrameStage::Begun)
			continue;
		auto encoded = EncodeFrameMetadata(*frame, trackerCollections, response.minor >= 11);
		// Each frame is one TLV; reserve room for the response metadata.
		const auto size = encoded.value.size() + 8;
		if (size > budget - bytes)
		{
			if (frames.empty())
				throw std::length_error("Frame metadata exceeds the wire page budget");
			break;
		}
		bytes += size;
		encoded.tag = static_cast<std::uint16_t>(frames.size() + 1);
		frames.push_back(std::move(encoded));
		if (frames.size() == count)
			break;
	}
	response.fields.push_back(Field::I64(tag::Time, history.origin.value.count()));
	response.fields.push_back(Field::U64(tag::Capacity, history.capacity));
	const auto oldest = std::find_if(history.frames.begin(), history.frames.end(),
									 [&](const auto& frame) { return frame->key.generation == history.generation; });
	response.fields.push_back(Field::U64(tag::Offset, oldest == history.frames.end() ? 0 : (*oldest)->key.frame.value));
	response.fields.push_back(Nested(tag::Frames, std::move(frames)));
}
} // namespace simxr::control
