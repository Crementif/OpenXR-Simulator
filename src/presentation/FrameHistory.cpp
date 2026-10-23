#include "presentation/FrameHistory.h"
#include "Pch.h"

#include <atomic>
#include <limits>
#include <stdexcept>

namespace simxr
{
static SessionGeneration NextGeneration()
{
	static std::atomic<std::uint64_t> s_nextGeneration{1};
	auto value = s_nextGeneration.load();
	for (;;)
	{
		if (value == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Session generations exhausted");
		if (s_nextGeneration.compare_exchange_weak(value, value + 1))
			return {value};
	}
}

static bool Pending(FrameStage stage)
{
	return stage == FrameStage::Issued || stage == FrameStage::Begun;
}

FrameHistory::FrameHistory(std::size_t capacity, std::size_t diagnosticCapacity)
	: m_capacity(capacity), m_diagnostics(std::make_shared<FrameDiagnosticsJournal>(diagnosticCapacity))
{
	if (capacity < 2)
		throw std::invalid_argument("Frame history requires pipeline capacity");
}

FrameDiagnostic FrameHistory::Trace(FrameTraceKind kind, FrameKey frame, RuntimeTime time, XrResult result,
									FrameDiagnosticCategory category, std::uint64_t related,
									std::string_view detail) noexcept
{
	return m_diagnostics->Trace(kind, frame, time, result, category, related, detail);
}

FrameDiagnostics FrameHistory::Diagnostics(std::uint64_t after, std::size_t count, std::optional<FrameKey> frame) const
{
	return m_diagnostics->Read(after, count, frame);
}

SessionGeneration FrameHistory::StartGeneration(RuntimeTime origin)
{
	std::lock_guard lock(m_mutex);
	CancelPendingLocked();
	m_generation = NextGeneration();
	m_origin = origin;
	return m_generation;
}

FrameHistory::Records::iterator FrameHistory::Current(FrameId frame)
{
	const FrameKey key{m_generation, frame};
	auto found = std::find_if(m_records.begin(), m_records.end(),
							  [&](const auto& value)
							  {
								  return value.record->key == key;
							  });
	if (found == m_records.end())
		throw std::logic_error("Unknown frame history identity");
	return found;
}

void FrameHistory::Issue(
	const FramePrediction& prediction,
	const std::function<void(std::shared_ptr<const FrameRecord>, std::shared_ptr<const FrameRecord>)>& reserve,
	std::optional<SequenceCaptureRecord> sequenceCapture)
{
	if (!prediction.id.value)
		throw std::invalid_argument("Invalid frame identity");
	std::lock_guard lock(m_mutex);
	if (!m_generation.value)
		throw std::logic_error("Frame history is not running");
	const FrameKey key{m_generation, prediction.id};
	if (std::any_of(m_records.begin(), m_records.end(),
					[&](const auto& value)
					{
						return value.record->key == key;
					}))
		throw std::logic_error("Frame identity already issued");
	if (sequenceCapture && (!sequenceCapture->request.sequence || !sequenceCapture->request.owner.id ||
							sequenceCapture->request.frame != prediction.id ||
							((sequenceCapture->reservation.result == SnapshotAdmission::Accepted) !=
							 (sequenceCapture->reservation.id.value != 0))))
		throw std::invalid_argument("Invalid sequence capture receipt");
	auto record = std::make_shared<const FrameRecord>(
		FrameRecord{key, prediction, FrameStage::Issued, {}, std::move(sequenceCapture)});
	auto cancelled = std::make_shared<FrameRecord>(*record);
	cancelled->stage = FrameStage::Cancelled;
	if (m_records.size() == m_capacity)
	{
		auto retired = std::find_if(m_records.begin(), m_records.end(),
									[](const auto& value)
									{
										return !Pending(value.record->stage);
									});
		if (retired == m_records.end())
			throw std::length_error("Frame history pipeline capacity exhausted");
		const auto index = static_cast<std::size_t>(retired - m_records.begin());
		m_records.push_back({record, cancelled});
		try
		{
			if (reserve)
				reserve(record, cancelled);
		}
		catch (...)
		{
			m_records.pop_back();
			throw;
		}
		m_records.erase(m_records.begin() + static_cast<std::ptrdiff_t>(index));
		++m_evicted;
	}
	else
	{
		m_records.push_back({record, cancelled});
		try
		{
			if (reserve)
				reserve(record, cancelled);
		}
		catch (...)
		{
			m_records.pop_back();
			throw;
		}
	}
	Trace(FrameTraceKind::Issued, key, prediction.displayTime);
}

void FrameHistory::Begin(FrameId frame, std::optional<FrameId> discarded)
{
	std::lock_guard lock(m_mutex);
	auto current = Current(frame);
	if (current->record->stage != FrameStage::Issued)
		throw std::logic_error("Frame history begin order");
	auto begun = std::make_shared<FrameRecord>(*current->record);
	begun->stage = FrameStage::Begun;
	if (discarded)
	{
		auto previous = Current(*discarded);
		if (previous == current || previous->record->stage != FrameStage::Begun)
			throw std::logic_error("Frame history discard order");
		auto record = std::make_shared<FrameRecord>(*previous->record);
		record->stage = FrameStage::Discarded;
		previous->record = std::move(record);
		Trace(FrameTraceKind::Discarded, previous->record->key, previous->record->prediction.displayTime);
	}
	current->record = std::move(begun);
	Trace(FrameTraceKind::Begun, current->record->key, current->record->prediction.displayTime);
}

std::optional<Duration> FrameRecord::CpuFrameTime() const noexcept
{
	if (!submission || !submission->diagnostics)
		return {};
	std::optional<Duration> begin, end;
	for (const auto& event : submission->diagnostics->events)
	{
		if (event.frame != key || event.result != XR_SUCCESS || !event.sequence)
			continue;
		if (event.kind == FrameTraceKind::WaitReturned)
			begin = event.hostTime;
		if (event.kind == FrameTraceKind::Submitted)
			end = event.hostTime;
	}
	if (!begin || !end || *end < *begin)
		return {};
	if (begin->count() < 0 && end->count() > std::numeric_limits<Duration::rep>::max() + begin->count())
		return {};
	return *end - *begin;
}

XrResult FrameHistory::Submit(FrameId frame, FrameSubmissionState state, const std::function<XrResult()>& beforeCommit)
{
	std::lock_guard lock(m_mutex);
	auto current = Current(frame);
	if (current->record->stage != FrameStage::Begun || state.displayTime.value.count() <= 0)
		throw std::logic_error("Frame history submission order or time");
	auto record = std::make_shared<FrameRecord>(*current->record);
	record->stage = FrameStage::Submitted;
	record->submission = std::move(state);
	auto diagnostics = std::make_shared<FrameDiagnostics>();
	diagnostics->events.reserve(256);
	diagnostics->truncated = !m_diagnostics->ReadInto(*diagnostics, 0, 255, record->key);
	record->submission->diagnostics = diagnostics;
	if (beforeCommit)
	{
		const auto result = beforeCommit();
		if (XR_FAILED(result))
		{
			Trace(FrameTraceKind::ApiError, record->key, record->submission->displayTime, result,
				  FrameDiagnosticCategory::ApiError);
			return result;
		}
	}
	const auto submitted = Trace(FrameTraceKind::Submitted, record->key, record->submission->displayTime);
	const bool refreshed = m_diagnostics->ReadInto(*diagnostics, 0, 256, record->key);
	const bool recorded = submitted.sequence && std::any_of(diagnostics->events.begin(), diagnostics->events.end(),
															[&](const auto& event)
															{
																return event.sequence == submitted.sequence;
															});
	if (!refreshed || !recorded)
	{
		if (diagnostics->events.size() == 256)
			diagnostics->events.pop_back();
		diagnostics->events.push_back(submitted); // Reserved before native acceptance.
		if (submitted.sequence)
			diagnostics->next = std::max(diagnostics->next, submitted.sequence + 1);
		diagnostics->truncated = true;
	}
	if (record->submission->outputDisposition == FrameOutputDisposition::Dropped)
		Trace(FrameTraceKind::PreviewDropped, record->key, record->submission->displayTime, XR_SUCCESS,
			  FrameDiagnosticCategory::Performance, static_cast<std::uint64_t>(record->submission->dropReason));
	current->record = std::move(record);
	return XR_SUCCESS;
}

void FrameHistory::CancelPendingLocked() noexcept
{
	for (auto& entry : m_records)
		if (Pending(entry.record->stage))
		{
			Trace(FrameTraceKind::Cancelled, entry.record->key, entry.record->prediction.displayTime);
			entry.record = entry.cancelled;
		}
}

void FrameHistory::CancelPending() noexcept
{
	std::lock_guard lock(m_mutex);
	CancelPendingLocked();
}

std::shared_ptr<const FrameRecord> FrameHistory::Find(FrameKey key) const
{
	std::lock_guard lock(m_mutex);
	auto found = std::find_if(m_records.begin(), m_records.end(),
							  [&](const auto& value)
							  {
								  return value.record->key == key;
							  });
	return found == m_records.end() ? nullptr : found->record;
}

FrameHistorySnapshot FrameHistory::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	FrameHistorySnapshot result{m_generation, m_evicted, m_capacity, {}};
	result.origin = m_origin;
	result.frames.reserve(m_records.size());
	for (const auto& entry : m_records)
		result.frames.push_back(entry.record);
	return result;
}

std::shared_ptr<const FrameRecord> FrameHistory::CancellationRecord(FrameKey key) const
{
	std::lock_guard lock(m_mutex);
	const auto found = std::find_if(m_records.begin(), m_records.end(),
									[&](const auto& value)
									{
										return value.record->key == key;
									});
	return found == m_records.end() ? nullptr : found->cancelled;
}
} // namespace simxr
