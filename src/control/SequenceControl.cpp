#include "control/SequenceControl.h"
#include "Pch.h"
#include "control/ControlRequestError.h"
#include "control/DebugProtocol.h"
#include "control/ControlSchema.h"
#include "control/ControlSessions.h"
#include "control/SimulationControl.h"
#include "control/WireProtocol.h"
#include "simulation/PoseSequence.h"
#include "runtime/Session.h"

namespace simxr::control
{
static InspectionValue Status(const PoseSequenceStatus& status)
{
	return InspectionValue::Object({{"id", InspectionValue::Unsigned(status.id)},
									{"active", InspectionValue::Boolean(status.active)},
									{"loop", InspectionValue::Boolean(status.loop)},
									{"result", InspectionValue::Text(SimulationResultName(status.result))},
									{"origin", InspectionValue::Unsigned(status.origin.value)},
									{"last_frame", InspectionValue::Unsigned(status.lastFrame.value)},
									{"groups", InspectionValue::Unsigned(status.groups)}});
}

struct AdmissionDescription
{
	const char* name;
	XrResult result;
};

static AdmissionDescription DescribeAdmission(SnapshotAdmission value)
{
	switch (value)
	{
	case SnapshotAdmission::Accepted:
		return {"accepted", XR_SUCCESS};
	case SnapshotAdmission::Capacity:
		return {"capacity", XR_ERROR_LIMIT_REACHED};
	case SnapshotAdmission::Closed:
		return {"closed", XR_ERROR_SESSION_NOT_RUNNING};
	case SnapshotAdmission::Unsupported:
		return {"unsupported", XR_ERROR_FEATURE_UNSUPPORTED};
	case SnapshotAdmission::Invalid:
		return {"invalid", XR_ERROR_VALIDATION_FAILURE};
	}
	throw std::invalid_argument("Unknown sequence capture admission");
}

InspectionValue SequenceControl::Dispatch(const Message& request, const ControlSession& binding, CommandSource source,
										  Message& response, std::vector<SnapshotRecord>& held)
{
	switch (static_cast<OpCode>(request.opcode))
	{
	case OpCode::UploadSequence:
		return Upload(request, binding);
	case OpCode::StartSequence:
		return Start(request, binding, source, response);
	case OpCode::ReadSequence:
		return Read(request, binding, source, held);
	case OpCode::StopSequence:
	{
		const auto status = binding.session->Commands().PoseSequenceStatusFor(source);
		if (!status || status->id != RequireRequestU64(request, tag::SequenceId))
			throw ControlRequestError{ErrorCode::Conflict,
									  "Sequence stop does not identify the current owned execution"};
		binding.session->StopPoseSequence(source);
		return Status(*binding.session->Commands().PoseSequenceStatusFor(source));
	}
	default:
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported sequence operation"};
	}
}

InspectionValue SequenceControl::Upload(const Message& request, const ControlSession& binding)
{
	const auto id = RequireRequestU64(request, tag::ProgramId);
	const auto offset = RequireRequestU64(request, tag::Offset);
	const auto total = RequireRequestU64(request, tag::TotalBytes);
	const auto field = request.Find(tag::Bytes);
	if (!id || !field || total > MaxBytes || offset > total || field->value.size() > ChunkBytes ||
		field->value.size() > total - offset || (field->value.empty() && total))
		throw ControlRequestError{ErrorCode::Invalid, "Invalid bounded sequence upload"};
	const std::string_view bytes(reinterpret_cast<const char*>(field->value.data()), field->value.size());
	if (bytes.find('\0') != std::string_view::npos)
		throw ControlRequestError{ErrorCode::Invalid, "Sequence JSON lines cannot contain NUL bytes"};
	const SessionGeneration generation{RequireRequestU64(request, tag::Generation)};
	if (id != m_program)
	{
		if (id < m_program || offset)
			throw ControlRequestError{ErrorCode::Conflict,
									  "New sequence uploads require an increasing program ID and offset zero"};
		std::string prepared(bytes);
		prepared.reserve(static_cast<std::size_t>(total));
		m_bytes.swap(prepared);
		m_program = id;
		m_total = static_cast<std::size_t>(total);
		m_session = binding.id;
		m_generation = generation;
		m_started.reset();
	}
	else
	{
		if (m_session != binding.id || m_generation != generation || total != m_total || offset > m_bytes.size())
			throw ControlRequestError{ErrorCode::Conflict, "Sequence upload identity or offset changed"};
		if (offset < m_bytes.size())
		{
			if (bytes.size() > m_bytes.size() - offset ||
				m_bytes.compare(static_cast<std::size_t>(offset), bytes.size(), bytes))
				throw ControlRequestError{ErrorCode::Conflict, "Sequence upload retry changed its bytes"};
		}
		else if (!m_started)
			m_bytes.append(bytes);
		else if (!bytes.empty())
			throw ControlRequestError{ErrorCode::Conflict, "Started sequence program is immutable"};
	}
	return InspectionValue::Object({{"program_id", InspectionValue::Unsigned(m_program)},
									{"received", InspectionValue::Unsigned(m_bytes.size())},
									{"total", InspectionValue::Unsigned(m_total)},
									{"complete", InspectionValue::Boolean(m_bytes.size() == m_total)}});
}

InspectionValue SequenceControl::Start(const Message& request, const ControlSession& binding, CommandSource source,
									   Message& response)
{
	const auto id = RequireRequestU64(request, tag::ProgramId);
	const auto loopField = request.Find(tag::Loop);
	const auto loop = loopField ? loopField->AsBool() : std::optional{false};
	if (!loop)
		throw ControlRequestError{ErrorCode::Invalid, "Sequence loop must be boolean"};
	if (!id || id != m_program || m_session != binding.id ||
		m_generation.value != RequireRequestU64(request, tag::Generation) || m_bytes.size() != m_total)
		throw ControlRequestError{ErrorCode::Conflict, "Sequence program is incomplete or belongs to another run"};
	auto status = binding.session->Commands().PoseSequenceStatusFor(source);
	if (m_started)
	{
		if (m_loop != *loop || !status || status->id != *m_started)
			throw ControlRequestError{ErrorCode::Conflict,
									  "Sequence start retry changed or its execution was replaced"};
	}
	else
	{
		try
		{
			auto program = std::make_shared<const PoseSequence>(PoseSequence::Parse(m_bytes, *loop));
			status = binding.session->Commands().StartPoseSequence(source, std::move(program));
		}
		catch (const std::length_error&)
		{
			throw ControlRequestError{ErrorCode::Capacity, "Sequence exceeds the supported step count"};
		}
		catch (const std::overflow_error&)
		{
			throw ControlRequestError{ErrorCode::Invalid, "Sequence exceeds the frame identity range"};
		}
		if (!status)
			throw ControlRequestError{ErrorCode::Busy,
									  "Another source owns sequence playback or the session is closed"};
		m_started = status->id;
		m_loop = *loop;
	}
	response.fields.push_back(Field::U64(tag::SequenceId, status->id));
	return Status(*status);
}

InspectionValue SequenceControl::Read(const Message& request, const ControlSession& binding, CommandSource source,
									  std::vector<SnapshotRecord>& held)
{
	auto status = binding.session->Commands().PoseSequenceStatusFor(source);
	const auto id = request.Find(tag::SequenceId) ? RequireRequestU64(request, tag::SequenceId)
					: status					  ? status->id
												  : 0;
	const auto after = request.Find(tag::Offset) ? RequireRequestU64(request, tag::Offset) : 0;
	const auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 32;
	if (!id || !count || count > 64)
		throw ControlRequestError{ErrorCode::Invalid,
								  "Sequence status requires an execution and a page size in [1,64]"};
	const auto history = binding.session->History().Snapshot();
	if (history.generation.value != RequireRequestU64(request, tag::Generation))
		throw ControlRequestError{ErrorCode::StaleGeneration, "Sequence history belongs to another run"};
	status = binding.session->Commands().PoseSequenceStatusFor(source);
	std::vector<InspectionValue> captures;
	held.reserve(held.size() + static_cast<std::size_t>(count) + binding.session->Captures().Capacity());
	std::uint64_t cursor = after;
	bool more = false;
	for (const auto& frame : history.frames)
	{
		if (frame->key.frame.value <= after)
			continue;
		if (captures.size() == count)
		{
			more = true;
			break;
		}
		cursor = frame->key.frame.value;
		if (!frame->sequenceCapture || frame->sequenceCapture->request.sequence != id ||
			frame->sequenceCapture->request.owner != source)
			continue;
		const auto& receipt = *frame->sequenceCapture;
		const auto admission = DescribeAdmission(receipt.reservation.result);
		const char* phase = "not_admitted";
		XrResult result = admission.result;
		if (receipt.reservation.id.value)
		{
			auto record = binding.session->Captures().Read(receipt.reservation.id);
			phase = "released";
			if (record)
			{
				held.push_back(std::move(*record));
				const auto& capture = held.back();
				phase = SnapshotPhaseName(capture.phase);
				result = capture.result;
				if (capture.phase == SnapshotPhase::GpuPending && capture.output)
				{
					const auto completion = capture.output->PollCompletion();
					if (completion == CompletionStatus::Complete)
						phase = "ready";
					else if (completion == CompletionStatus::Failed)
					{
						phase = "failed";
						result = XR_ERROR_RUNTIME_FAILURE;
					}
				}
			}
		}
		captures.push_back(
			InspectionValue::Object({{"frame", InspectionValue::Unsigned(cursor)},
									 {"capture_id", InspectionValue::Unsigned(receipt.reservation.id.value)},
									 {"admission", InspectionValue::Text(admission.name)},
									 {"phase", InspectionValue::Text(phase)},
									 {"result", InspectionValue::Signed(result)}}));
	}
	std::vector<InspectionValue> retained;
	for (const auto captureId : binding.session->Captures().Snapshot().captures)
	{
		auto record = binding.session->Captures().Read(captureId);
		if (!record)
			continue;
		held.push_back(std::move(*record));
		const auto& capture = held.back();
		if (!capture.metadata || !capture.metadata->sequenceCapture)
			continue;
		const auto& receipt = *capture.metadata->sequenceCapture;
		if (receipt.request.owner == source && receipt.request.sequence == id && receipt.reservation.id == captureId)
			retained.push_back(
				InspectionValue::Object({{"capture_id", InspectionValue::Unsigned(captureId.value)},
										 {"frame", InspectionValue::Unsigned(capture.metadata->key.frame.value)}}));
	}
	const auto oldest = history.frames.empty() ? 0 : history.frames.front()->key.frame.value;
	bool gap = false;
	if (oldest)
	{
		if (after)
			gap = oldest > after && oldest - after > 1;
		else if (status && status->id == id)
			gap = status->origin.value && oldest > status->origin.value;
		else
			gap = history.evicted != 0;
	}
	return InspectionValue::Object({{"sequence_id", InspectionValue::Unsigned(id)},
									{"status", status && status->id == id ? Status(*status) : InspectionValue::Null()},
									{"current_sequence", InspectionValue::Unsigned(status ? status->id : 0)},
									{"next_frame", InspectionValue::Unsigned(cursor)},
									{"more", InspectionValue::Boolean(more)},
									{"oldest_frame", InspectionValue::Unsigned(oldest)},
									{"history_gap", InspectionValue::Boolean(gap)},
									{"evicted_frames", InspectionValue::Unsigned(history.evicted)},
									{"held_captures", InspectionValue::Array(std::move(retained))},
									{"captures", InspectionValue::Array(std::move(captures))}});
}

} // namespace simxr::control
