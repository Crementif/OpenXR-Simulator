#include "control/DebugProtocol.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "runtime/Session.h"

#include <array>
#include <utility>

namespace simxr::control
{
namespace
{
std::uint64_t RequireU64(const Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	const auto value = field ? field->AsU64() : std::nullopt;
	if (!value)
		throw ControlRequestError{ErrorCode::Invalid, "Missing or invalid uint64 field"};
	return *value;
}
std::uint32_t ReadU32(const Message& message, std::uint16_t name, std::optional<std::uint32_t> fallback = {})
{
	const auto* field = message.Find(name);
	if (!field && fallback)
		return *fallback;
	const auto value = field ? field->AsU32() : std::nullopt;
	if (!value)
		throw ControlRequestError{ErrorCode::Invalid, "Missing or invalid uint32 field"};
	return *value;
}
Field Nested(std::uint16_t name, std::vector<Field> fields)
{
	return Field::Bytes(name, EncodeFields(fields));
}
Field EncodePrediction(std::uint16_t name, const FramePrediction& prediction)
{
	return Nested(name,
				  {Field::U64(tag::FrameId, prediction.id.value), Field::U64(tag::Tick, prediction.displayTick.value),
				   Field::I64(tag::Time, prediction.displayTime.value.count()),
				   Field::I64(tag::Period, prediction.displayPeriod.count())});
}
Field EncodeCaptureIds(std::uint16_t name, const std::vector<CaptureId>& ids)
{
	std::vector<Field> fields;
	for (const auto id : ids)
		fields.push_back(Field::U64(static_cast<std::uint16_t>(fields.size() + 1), id.value));
	return Nested(name, std::move(fields));
}
} // namespace

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
	case TargetKind::VirtualDisplay:
		if (layer || view)
			break;
		return {CaptureKind::VirtualDisplay};
	}
	throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported capture target"};
}
namespace
{
DebugCommand DecodeCommandKind(std::uint32_t kind)
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
	}
	throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported debug command"};
}
const char* Name(DebugPhase value)
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
const char* Name(DebugProgress value)
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
	}
	throw ControlRequestError{ErrorCode::Internal, "Unknown operation progress"};
}
const char* Name(SnapshotPhase value)
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
const char* Name(FrameStage value)
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
const char* Name(FrameOutputDisposition value)
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
const char* Name(FrameDropReason value)
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
const char* Name(PresentationKind value)
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
const char* Name(SnapshotAdmission value)
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
Field EncodeFrameMetadata(const FrameRecord& frame)
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
	}
	return Nested(tag::Frame, std::move(fields));
}
ErrorCode AdmissionError(DebugAdmission value)
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
} // namespace
Field EncodeFrameKey(std::uint16_t name, FrameKey key)
{
	return Nested(name, {Field::U64(tag::Generation, key.generation.value), Field::U64(tag::FrameId, key.frame.value)});
}

Field EncodeDisplayTick(std::uint16_t name, const PresentationTick& tick)
{
	std::vector<Field> fields{Field::U64(tag::Tick, tick.tick.value), Field::I64(tag::Time, tick.time.value.count()),
							  Field::Text(tag::Kind, Name(tick.kind)),
							  Field::U64(tag::SkippedTicks, tick.skippedTicks)};
	if (tick.source)
		fields.push_back(EncodeFrameKey(tag::Source, *tick.source));
	return Nested(name, std::move(fields));
}

DebugRequest DecodeDebugRequest(const Message& request)
{
	DebugRequest command;
	command.id = {RequireU64(request, tag::CommandId)};
	command.command = DecodeCommandKind(ReadU32(request, tag::CommandKind));
	command.lookahead = ReadU32(request, tag::Lookahead, 1);
	command.target = DecodeCaptureTarget(request);
	command.generation = {RequireU64(request, tag::Generation)};
	if (!command.generation.value)
		throw ControlRequestError{ErrorCode::Closed, "Session has not started"};
	if (request.Find(tag::OperationId))
		command.operation = {RequireU64(request, tag::OperationId)};
	if (request.Find(tag::CaptureId))
		command.capture = {RequireU64(request, tag::CaptureId)};
	return command;
}
void EncodeDebuggerStatus(Message& response, const DebugControllerSnapshot& status, const SessionSnapshot& state,
						  const PresentationSnapshot& display, std::uint64_t capabilities)
{
	response.fields.push_back(Field::U64(tag::Capabilities, capabilities));
	response.fields.push_back(Field::I64(tag::State, state.state));
	response.fields.push_back(Field::Bool(tag::Running, state.running));
	response.fields.push_back(Field::Bool(tag::Closed, state.closed));
	response.fields.push_back(Field::Bool(tag::Manual, status.manual));
	response.fields.push_back(Field::U32(tag::Lookahead, status.maxLookahead));
	response.fields.push_back(Field::U64(tag::LedgerCount, status.commands));
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
void EncodeCaptureMetadata(Message& response, const SnapshotRecord& capture)
{
	response.fields.push_back(Field::U64(tag::CaptureId, capture.id.value));
	response.fields.push_back(Field::Text(tag::Phase, Name(capture.phase)));
	response.fields.push_back(Field::I64(tag::Result, capture.result));
	response.fields.push_back(EncodeCaptureTarget(capture.target));
	response.fields.push_back(
		Field::Bool(tag::ImageAvailable, capture.phase == SnapshotPhase::Ready && capture.output != nullptr));
	if (capture.metadata)
		response.fields.push_back(EncodeFrameMetadata(*capture.metadata));
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
} // namespace simxr::control
