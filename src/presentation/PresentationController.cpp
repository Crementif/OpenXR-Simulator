#include "presentation/PresentationController.h"
#include "Pch.h"
#include "presentation/FrameHistory.h"

namespace simxr
{
PresentationController::PresentationController(std::size_t capacity, FrameHistory* history,
											   CompletionObserver completed)
	: m_capacity(capacity), m_history(history), m_completed(std::move(completed))
{
	if (!capacity)
		throw std::invalid_argument("Presentation queue requires capacity");
}

void PresentationController::Start(SessionGeneration generation, const DisplayTimeline& timeline)
{
	std::optional<PresentationSubmission> released;
	std::shared_ptr<const FrameOutput> releasedImage;
	std::optional<PresentationSubmission> releasedClear;
	{
		std::lock_guard lock(m_mutex);
		if (!generation.value || generation <= m_generation)
			throw std::invalid_argument("Presentation generation must increase");
		for (auto& entry : m_entries)
			entry.retiring = true;
		released = std::move(m_current);
		m_current.reset();
		releasedImage = std::move(m_displayOutput);
		releasedClear = std::move(m_liveClear);
		m_liveClear.reset();
		m_coalescedLiveClears = 0;
		m_generation = generation;
		m_timeline = timeline;
		m_lastSubmitted = {};
		m_lastSelected = {};
		m_latest.reset();
		m_running = true;
		++m_revision;
	}
	m_published.notify_all();
}

void PresentationController::Retime(SessionGeneration generation, const DisplayTimeline& timeline)
{
	std::lock_guard lock(m_mutex);
	if (m_running && generation == m_generation)
		m_timeline = timeline;
}

PresentationAdmission PresentationController::Submit(PresentationSubmission submission,
													 const std::function<void()>& commit)
{
	const auto metadata = submission.metadata;
	return SubmitWithMetadata(std::move(submission), [&] {
		if (commit)
			commit();
		return metadata;
	});
}

PresentationAdmission
PresentationController::SubmitWithMetadata(PresentationSubmission submission,
										   const std::function<std::shared_ptr<const FrameRecord>()>& commit)
{
	PresentationSubmission cancelled;
	std::shared_ptr<const FrameRecord> replacedMetadata;
	std::lock_guard lock(m_mutex);
	if (!m_running)
		return PresentationAdmission::Closed;
	if (submission.key.generation != m_generation)
		return PresentationAdmission::StaleGeneration;
	if ((submission.reprojection.source && !submission.output) || !submission.key.frame.value ||
		submission.key.frame <= m_lastSubmitted || submission.targetTime.value.count() <= 0)
		return PresentationAdmission::Invalid;
	if (m_entries.size() >= m_capacity)
		return PresentationAdmission::QueueFull;
	const auto frame = submission.key.frame;
	// Allocate before transferring readers: allocation failure must release them after unlock.
	static_assert(std::is_nothrow_move_assignable_v<PresentationSubmission>);
	m_entries.emplace_back();
	m_entries.back().submission = std::move(submission);
	try
	{
		if (commit)
		{
			auto metadata = commit();
			replacedMetadata = std::move(m_entries.back().submission.metadata);
			m_entries.back().submission.metadata = std::move(metadata);
		}
	}
	catch (...)
	{
		cancelled = std::move(m_entries.back().submission);
		m_entries.pop_back();
		throw;
	}
	m_lastSubmitted = frame;
	return PresentationAdmission::Accepted;
}

PresentationAdmission
PresentationController::SubmitLiveClearWithMetadata(PresentationSubmission submission,
													const std::function<std::shared_ptr<const FrameRecord>()>& commit)
{
	std::optional<PresentationSubmission> replaced;
	std::shared_ptr<const FrameRecord> replacedMetadata;
	std::lock_guard lock(m_mutex);
	if (!m_running)
		return PresentationAdmission::Closed;
	if (submission.key.generation != m_generation)
		return PresentationAdmission::StaleGeneration;
	if (submission.output || submission.reprojection.source || !submission.key.frame.value ||
		submission.key.frame <= m_lastSubmitted || submission.targetTime.value.count() <= 0)
		return PresentationAdmission::Invalid;
	if (commit)
	{
		auto metadata = commit();
		replacedMetadata = std::move(submission.metadata);
		submission.metadata = std::move(metadata);
	}
	if (m_liveClear)
	{
		replaced = std::move(m_liveClear);
		if (m_coalescedLiveClears != std::numeric_limits<std::uint64_t>::max())
			++m_coalescedLiveClears;
	}
	m_lastSubmitted = submission.key.frame;
	m_liveClear = std::move(submission);
	return PresentationAdmission::Accepted;
}

std::optional<PresentationTick> PresentationController::Advance(RuntimeTime now,
																std::optional<SessionGeneration> generation,
																const ImageTransform& transform)
{
	return AdvanceAt(now, generation, transform, false);
}

std::optional<PresentationTick> PresentationController::AdvanceExternal(RuntimeTime target,
																		SessionGeneration generation,
																		const ImageTransform& transform,
																		const std::function<bool()>& current)
{
	return AdvanceAt(target, generation, transform, true, current);
}

void PresentationController::Stop() noexcept
{
	std::optional<PresentationSubmission> released;
	std::shared_ptr<const FrameOutput> releasedImage;
	std::optional<PresentationSubmission> releasedClear;
	{
		std::lock_guard lock(m_mutex);
		m_running = false;
		++m_revision;
		for (auto& entry : m_entries)
			entry.retiring = true;
		released = std::move(m_current);
		m_current.reset();
		releasedImage = std::move(m_displayOutput);
		releasedClear = std::move(m_liveClear);
		m_liveClear.reset();
	}
	m_published.notify_all();
}

std::size_t PresentationController::PollRetired()
{
	std::vector<Observation> observed;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& entry : m_entries)
			if (entry.retiring)
				observed.push_back({entry.submission});
	}
	for (auto& value : observed)
		value.status = value.submission.output ? value.submission.output->PollCompletion() : CompletionStatus::Complete;
	std::lock_guard lock(m_mutex);
	std::erase_if(m_entries, [&](const auto& entry) {
		return entry.retiring && std::any_of(observed.begin(), observed.end(), [&](const auto& value) {
				   return value.submission.key == entry.submission.key && value.status != CompletionStatus::Pending;
			   });
	});
	return static_cast<std::size_t>(
		std::count_if(m_entries.begin(), m_entries.end(), [](const auto& entry) { return entry.retiring; }));
}

PresentationSnapshot PresentationController::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	PresentationSnapshot result{m_running, m_generation, m_capacity, m_lastSubmitted, m_latest, {}, {}};
	result.liveClear = m_liveClear ? std::optional{m_liveClear->key} : std::nullopt;
	result.coalescedLiveClears = m_coalescedLiveClears;
	if (result.latest && m_current)
	{
		result.latest->output = m_displayOutput;
		result.latest->reprojection = m_current->reprojection;
	}
	for (const auto& entry : m_entries)
		(entry.retiring ? result.retiring : result.queued).push_back(entry.submission.key);
	return result;
}

std::vector<PresentationSubmission> PresentationController::Retain() const
{
	std::vector<PresentationSubmission> retained;
	{
		std::lock_guard lock(m_mutex);
		retained.reserve(m_entries.size() + (m_current ? 1 : 0) + (m_liveClear ? 1 : 0));
		if (m_current)
			retained.push_back(*m_current);
		if (m_liveClear && (!m_current || m_current->key != m_liveClear->key))
			retained.push_back(*m_liveClear);
		for (const auto& entry : m_entries)
			if (!entry.retiring && (!m_current || m_current->key != entry.submission.key))
				retained.push_back(entry.submission);
	}
	std::sort(retained.begin(), retained.end(),
			  [](const auto& left, const auto& right) { return left.key < right.key; });
	return retained;
}

std::optional<PresentationTick> PresentationController::AdvanceAt(RuntimeTime now,
																  std::optional<SessionGeneration> generation,
																  const ImageTransform& transform, bool exact,
																  const std::function<bool()>& current)
{
	std::vector<Observation> observed;
	DisplayTick tick;
	RuntimeTime time;
	std::uint64_t revision;
	{
		std::lock_guard lock(m_mutex);
		if (!m_running || (generation && *generation != m_generation) || now < m_timeline->Epoch())
			return {};
		tick = m_timeline->TickAtOrBefore(now);
		time = exact ? now : m_timeline->TimeAt(tick);
		if (m_latest)
		{
			if (time <= m_latest->time)
				return {};
			if (m_latest->tick.value == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("Presentation identity exhausted");
			tick.value = std::max(tick.value, m_latest->tick.value + 1);
		}
		revision = m_revision;
		observed.reserve(m_entries.size() + (m_liveClear ? 1 : 0));
		for (const auto& entry : m_entries)
			observed.push_back({entry.submission});
		if (m_liveClear)
			observed.push_back({*m_liveClear});
	}
	for (auto& value : observed)
		value.status = value.submission.output ? value.submission.output->PollCompletion() : CompletionStatus::Complete;

	std::optional<PresentationSubmission> released;
	std::optional<PresentationSubmission> releasedClear;
	std::optional<PresentationSubmission> chosen;
	std::shared_ptr<const FrameOutput> releasedImage;
	PresentationTick next{tick, time};
	bool newSource = false;
	{
		std::lock_guard lock(m_mutex);
		if (!m_running || revision != m_revision || (m_latest && tick <= m_latest->tick))
			return {};
		const Observation* selected = nullptr;
		for (const auto& value : observed)
		{
			auto entry = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& candidate) {
				return candidate.submission.key == value.submission.key;
			});
			const bool liveClear = m_liveClear && m_liveClear->key == value.submission.key;
			if ((!liveClear && (entry == m_entries.end() || entry->retiring)) ||
				value.status != CompletionStatus::Complete || value.submission.targetTime > time ||
				value.submission.key.frame <= m_lastSelected)
				continue;
			if (!selected || value.submission.key.frame > selected->submission.key.frame)
				selected = &value;
		}
		newSource = selected != nullptr;
		chosen = selected ? std::optional{selected->submission} : m_current;
		next.skippedTicks = m_latest ? tick.value - m_latest->tick.value - 1 : tick.value;
		next.kind = selected ? (selected->submission.output ? PresentationKind::NewFrame : PresentationKind::Clear)
					: chosen ? PresentationKind::Repeat
							 : PresentationKind::Empty;
		if (chosen)
		{
			next.source = chosen->key;
			next.metadata = chosen->metadata;
			next.output = chosen->output;
			next.reprojection = chosen->reprojection;
		}
	}
	if (transform)
	{
		const auto image = transform(next);
		if (static_cast<bool>(image.output) != static_cast<bool>(next.output) ||
			(image.output && image.output->PollCompletion() != CompletionStatus::Complete))
			throw std::logic_error("Display transform must preserve clear state and return completed images");
		next.policy = PresentationPolicy::Rotational;
		next.output = image.output;
		next.displayHead = image.head;
		next.reprojected = image.reprojected;
		next.reprojectionResult = image.result;
		next.reprojectionSubmissionTime = image.submissionTime;
		next.reprojectionCompletionLatency = image.completionLatency;
	}
	// GPU observation/transform may have outlived a physical opportunity or
	// attachment. Validate outside the presentation lock before publishing it.
	if (current && !current())
		return {};
	{
		std::lock_guard lock(m_mutex);
		if (!m_running || revision != m_revision || (m_latest && tick <= m_latest->tick))
			return {};
		if (newSource)
		{
			released = std::move(m_current);
			m_current = chosen;
			m_lastSelected = chosen->key.frame;
		}
		if (next.output)
			next.imageTick = newSource || next.output != m_displayOutput ? std::optional{tick} : m_latest->imageTick;
		releasedImage = std::move(m_displayOutput);
		m_displayOutput = next.output;
		for (auto entry = m_entries.begin(); entry != m_entries.end();)
		{
			auto value = std::find_if(observed.begin(), observed.end(), [&](const auto& candidate) {
				return candidate.submission.key == entry->submission.key;
			});
			if (value == observed.end())
			{
				++entry;
				continue;
			}
			if (!entry->completionReported && value->status == CompletionStatus::Complete && entry->submission.output &&
				entry->submission.metadata)
			{
				entry->completionReported = true;
				value->reportCompletion = true;
			}
			if (!entry->retiring && entry->submission.key.frame <= m_lastSelected)
				entry->retiring = true;
			if (value->status == CompletionStatus::Failed ||
				(entry->retiring && value->status == CompletionStatus::Complete))
				entry = m_entries.erase(entry);
			else
				++entry;
		}
		if (m_liveClear && m_liveClear->key.frame <= m_lastSelected)
		{
			releasedClear = std::move(m_liveClear);
			m_liveClear.reset();
		}
		m_latest = next;
		m_latest->output.reset();
		m_latest->reprojection.source.reset();
		++m_revision;
		if (m_history)
		{
			const auto kind = next.kind == PresentationKind::NewFrame ? FrameTraceKind::DisplaySelected
							  : next.kind == PresentationKind::Repeat ? FrameTraceKind::DisplayRepeated
							  : next.kind == PresentationKind::Clear  ? FrameTraceKind::DisplayCleared
																	  : FrameTraceKind::DisplayEmpty;
			char detail[96];
			std::snprintf(detail, sizeof(detail), "skipped_ticks=%llu; content=%s",
						  static_cast<unsigned long long>(next.skippedTicks),
						  m_current ? (m_current->output ? "image" : "clear") : "empty");
			m_history->Trace(kind, next.source.value_or(FrameKey{m_generation, {}}), next.time, XR_SUCCESS,
							 FrameDiagnosticCategory::Timing, next.tick.value, detail);
		}
	}
	m_published.notify_all();
	if (m_completed)
		for (const auto& value : observed)
			if (value.reportCompletion)
				m_completed(value.submission.key, value.submission.metadata->prediction, time);
	return next;
}

void PresentationController::WaitForImage(std::optional<DisplayTick> after,
										  std::chrono::steady_clock::time_point deadline) const
{
	std::unique_lock lock(m_mutex);
	m_published.wait_until(lock, deadline,
						   [&] { return !m_running || (m_latest ? m_latest->imageTick : std::nullopt) != after; });
}

} // namespace simxr
