#include "presentation/SnapshotStore.h"
#include "Pch.h"

#include "graphics/SnapshotBudget.h"

#include <cinttypes>

namespace simxr
{
static CaptureId NextCapture()
{
	static std::atomic<std::uint64_t> s_nextCapture{1};
	auto value = s_nextCapture.load();
	while (true)
	{
		if (value == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Capture identities exhausted");
		if (s_nextCapture.compare_exchange_weak(value, value + 1))
			return {value};
	}
}

static bool Pending(SnapshotPhase phase)
{
	return phase == SnapshotPhase::Issued || phase == SnapshotPhase::Begun || phase == SnapshotPhase::GpuPending;
}

static bool HasLayerView(const FrameRecord& metadata, CaptureTarget target)
{
	if (!target.IsLayerImage())
		return true;
	if (!metadata.submission || !metadata.submission->layers ||
		target.layer >= metadata.submission->layers->layers.size())
		return false;
	const auto& layer = metadata.submission->layers->layers[target.layer];
	if (const auto* projection = std::get_if<ProjectionLayerDescription>(&layer))
		return target.view < projection->views.size() &&
			   (!target.IsDepth() || projection->views[target.view].depth.has_value());
	return !target.IsDepth() && target.view == 0;
}

static bool Valid(CaptureTarget target)
{
	return target.IsLayerImage() || ((target.kind == CaptureKind::ComposedEyes ||
									  target.kind == CaptureKind::VirtualDisplay || target.IsFilteredComposition()) &&
									 !target.layer && !target.view);
}

SnapshotStore::SnapshotStore(std::size_t capacity, FrameHistory* history)
	: m_history(history), m_slots([capacity] { return capacity ? capacity : SnapshotBudget::Capacity(); })
{
	m_entries.reserve(m_slots.Capacity());
}

SnapshotStore::~SnapshotStore()
{
	std::vector<Entry> retired;
	{
		std::lock_guard lock(m_mutex);
		m_running = false;
		CancelPendingLocked();
		retired.swap(m_entries);
	}
	// Readers can reenter Read/Snapshot when the last output owner disappears.
	// Publish an empty store while all members are still alive, then release the
	// detached CPU observers. Native owners independently guarantee GPU safety.
}

void SnapshotStore::Start(SessionGeneration generation)
{
	std::lock_guard lock(m_mutex);
	if (!generation.value || generation <= m_generation)
		throw std::invalid_argument("Snapshot generation must increase");
	CancelPendingLocked();
	m_generation = generation;
	m_running = true;
}

void SnapshotStore::CancelPendingLocked() noexcept
{
	for (auto& entry : m_entries)
		if (entry.record.phase == SnapshotPhase::Reserved || entry.record.phase == SnapshotPhase::Issued ||
			entry.record.phase == SnapshotPhase::Begun)
		{
			entry.record.phase = SnapshotPhase::Cancelled;
			if (entry.cancelled)
				entry.record.metadata = entry.cancelled;
			entry.producer.reset();
			entry.composedProducer.reset();
			++entry.revision;
			TraceTerminal(entry);
		}
}

void SnapshotStore::TraceTerminal(Entry& entry) noexcept
{
	if (!m_history || entry.terminalTraced)
		return;
	const auto& record = entry.record;
	const char* phase = nullptr;
	bool ready = false;
	switch (record.phase)
	{
	case SnapshotPhase::Ready:
		phase = "ready";
		ready = true;
		break;
	case SnapshotPhase::Clear:
		phase = "clear";
		ready = true;
		break;
	case SnapshotPhase::Empty:
		phase = "empty";
		ready = true;
		break;
	case SnapshotPhase::Discarded:
		phase = "discarded";
		break;
	case SnapshotPhase::Cancelled:
		phase = "cancelled";
		break;
	case SnapshotPhase::Failed:
		phase = "failed";
		break;
	case SnapshotPhase::Unavailable:
		phase = "unavailable";
		break;
	default:
		return;
	}
	RuntimeTime time;
	if (record.display)
		time = record.display->time;
	else if (record.metadata)
		time = record.metadata->submission ? record.metadata->submission->displayTime
										   : record.metadata->prediction.displayTime;
	const auto target = record.target.kind == CaptureKind::ComposedEyes		  ? "composed"
						: record.target.kind == CaptureKind::LayerView		  ? "layer_view"
						: record.target.kind == CaptureKind::LayerDepth		  ? "layer_depth"
						: record.target.kind == CaptureKind::LayerDepthLinear ? "layer_depth_linear"
						: record.target.kind == CaptureKind::ProjectionLayers ? "projection_layers"
						: record.target.kind == CaptureKind::QuadLayers		  ? "quad_layers"
																			  : "display";
	char detail[128];
	std::snprintf(detail, sizeof(detail), "phase=%s; target=%s; layer=%" PRIu32 "; view=%" PRIu32, phase, target,
				  record.target.layer, record.target.view);
	m_history->Trace(ready ? FrameTraceKind::CaptureReady : FrameTraceKind::CaptureFailed,
					 record.frame.value_or(FrameKey{record.generation, {}}), time, record.result,
					 FrameDiagnosticCategory::Timing, record.id.value, detail);
	entry.terminalTraced = true;
}

void SnapshotStore::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	m_running = false;
	CancelPendingLocked();
}

CaptureReservation SnapshotStore::Reserve(CaptureTarget target, std::shared_ptr<FrameReservation> producer,
										  std::shared_ptr<FrameReservation> composedProducer)
{
	if (!Valid(target))
		return {SnapshotAdmission::Invalid, {}};
	{
		std::lock_guard lock(m_mutex);
		if (!m_running)
			return {SnapshotAdmission::Closed, {}};
	}
	auto slot = m_slots.Reserve();
	if (!slot)
		return {SnapshotAdmission::Capacity, {}};
	Entry entry;
	entry.record.id = NextCapture();
	entry.record.target = target;
	entry.record.m_retention = std::move(slot);
	entry.producer = std::move(producer);
	entry.composedProducer = std::move(composedProducer);
	const auto id = entry.record.id;
	{
		std::lock_guard lock(m_mutex);
		if (!m_running)
			return {SnapshotAdmission::Closed, {}};
		entry.record.generation = m_generation;
		m_entries.push_back(std::move(entry));
	}
	return {SnapshotAdmission::Accepted, id};
}

bool SnapshotStore::Bind(CaptureId id, std::shared_ptr<const FrameRecord> issued,
						 std::shared_ptr<const FrameRecord> cancelled) noexcept
{
	if (!issued || !cancelled || issued->stage != FrameStage::Issued || cancelled->stage != FrameStage::Cancelled ||
		issued->key != cancelled->key)
		return false;
	std::lock_guard lock(m_mutex);
	auto entry =
		std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& value) { return value.record.id == id; });
	if (!m_running || entry == m_entries.end() || entry->released || entry->record.phase != SnapshotPhase::Reserved ||
		entry->record.target.kind == CaptureKind::VirtualDisplay || issued->key.generation != m_generation ||
		entry->record.generation != m_generation)
		return false;
	entry->record.frame = issued->key;
	entry->record.metadata = std::move(issued);
	entry->record.phase = SnapshotPhase::Issued;
	entry->cancelled = std::move(cancelled);
	++entry->revision;
	return true;
}

CaptureReservation SnapshotStore::Pin(std::shared_ptr<const FrameRecord> metadata,
									  std::shared_ptr<const FrameRecord> cancelled,
									  std::shared_ptr<const FrameOutput> output,
									  std::shared_ptr<FrameReservation> producer, CaptureTarget target)
{
	if (!metadata || target.kind == CaptureKind::VirtualDisplay ||
		((metadata->stage == FrameStage::Issued || metadata->stage == FrameStage::Begun) &&
		 (!cancelled || cancelled->key != metadata->key || cancelled->stage != FrameStage::Cancelled)))
		return {SnapshotAdmission::Invalid, {}};
	if (metadata->stage == FrameStage::Submitted && !HasLayerView(*metadata, target))
		output.reset();
	const auto capture = Reserve(target, std::move(producer));
	if (capture.result != SnapshotAdmission::Accepted)
		return capture;
	bool valid = false;
	{
		std::lock_guard lock(m_mutex);
		auto entry = std::find_if(m_entries.begin(), m_entries.end(),
								  [&](const auto& value) { return value.record.id == capture.id; });
		if (m_running && metadata->key.generation == m_generation && entry != m_entries.end() && !entry->released)
		{
			valid = true;
			entry->record.frame = metadata->key;
			entry->record.metadata = metadata;
			entry->cancelled = std::move(cancelled);
			switch (metadata->stage)
			{
			case FrameStage::Issued:
				entry->record.phase = SnapshotPhase::Issued;
				break;
			case FrameStage::Begun:
				entry->record.phase = SnapshotPhase::Begun;
				break;
			case FrameStage::Discarded:
				entry->record.phase = SnapshotPhase::Discarded;
				break;
			case FrameStage::Cancelled:
				entry->record.phase = SnapshotPhase::Cancelled;
				break;
			case FrameStage::Submitted:
				entry->record.output = std::move(output);
				entry->record.phase = !metadata->submission ? SnapshotPhase::Unavailable
									  : (!metadata->submission->layerCount && target.kind == CaptureKind::ComposedEyes)
										  ? SnapshotPhase::Clear
									  : entry->record.output ? SnapshotPhase::GpuPending
															 : SnapshotPhase::Unavailable;
				break;
			}
			if (entry->record.phase == SnapshotPhase::Unavailable)
				entry->record.result = XR_ERROR_FEATURE_UNSUPPORTED;
			if (entry->record.phase != SnapshotPhase::Issued && entry->record.phase != SnapshotPhase::Begun)
			{
				entry->producer.reset();
				entry->composedProducer.reset();
			}
			++entry->revision;
			TraceTerminal(*entry);
		}
	}
	if (!valid)
	{
		Release(capture.id);
		return {SnapshotAdmission::Closed, {}};
	}
	return capture;
}

void SnapshotStore::Update(std::shared_ptr<const FrameRecord> metadata, std::span<const CaptureOutput> outputs) noexcept
{
	if (!metadata)
		return;
	std::lock_guard lock(m_mutex);
	for (auto& entry : m_entries)
	{
		if (entry.record.frame != metadata->key || entry.record.target.kind == CaptureKind::VirtualDisplay ||
			(entry.record.phase != SnapshotPhase::Issued && entry.record.phase != SnapshotPhase::Begun))
			continue;
		entry.record.metadata = metadata;
		switch (metadata->stage)
		{
		case FrameStage::Issued:
			entry.record.phase = SnapshotPhase::Issued;
			break;
		case FrameStage::Begun:
			entry.record.phase = SnapshotPhase::Begun;
			break;
		case FrameStage::Submitted:
		{
			const auto source = std::find_if(outputs.begin(), outputs.end(),
											 [&](const auto& value) { return value.target == entry.record.target; });
			entry.record.output =
				source == outputs.end() || !HasLayerView(*metadata, entry.record.target) ? nullptr : source->output;
			entry.record.phase =
				!metadata->submission ? SnapshotPhase::Unavailable
				: (!metadata->submission->layerCount && entry.record.target.kind == CaptureKind::ComposedEyes)
					? SnapshotPhase::Clear
				: entry.record.output ? SnapshotPhase::GpuPending
									  : SnapshotPhase::Unavailable;
			break;
		}
		case FrameStage::Discarded:
			entry.record.phase = SnapshotPhase::Discarded;
			break;
		case FrameStage::Cancelled:
			entry.record.phase = SnapshotPhase::Cancelled;
			break;
		}
		if (entry.record.phase == SnapshotPhase::Unavailable)
			entry.record.result = XR_ERROR_FEATURE_UNSUPPORTED;
		if (entry.record.phase != SnapshotPhase::Issued && entry.record.phase != SnapshotPhase::Begun)
		{
			entry.producer.reset();
			entry.composedProducer.reset();
		}
		++entry.revision;
		TraceTerminal(entry);
	}
}

bool SnapshotStore::PublishDisplay(CaptureId id, const PresentationTick& tick,
								   std::shared_ptr<const FrameRecord> metadata) noexcept
{
	std::lock_guard lock(m_mutex);
	auto entry =
		std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& value) { return value.record.id == id; });
	if (!m_running || entry == m_entries.end() || entry->released || entry->record.generation != m_generation ||
		entry->record.target.kind != CaptureKind::VirtualDisplay || entry->record.phase != SnapshotPhase::Reserved ||
		(tick.source && tick.source->generation != m_generation) ||
		(metadata && (!tick.source || metadata->key != *tick.source)))
		return false;
	entry->record.frame = tick.source;
	entry->record.display = tick;
	entry->record.display->reprojection.source.reset();
	entry->record.metadata = std::move(metadata);
	entry->record.output = tick.output;
	entry->record.phase = tick.kind == PresentationKind::Empty ? SnapshotPhase::Empty
						  : !entry->record.metadata			   ? SnapshotPhase::Unavailable
						  : tick.output						   ? SnapshotPhase::GpuPending
															   : SnapshotPhase::Clear;
	if (entry->record.phase == SnapshotPhase::Unavailable)
		entry->record.result = XR_ERROR_FEATURE_UNSUPPORTED;
	++entry->revision;
	TraceTerminal(*entry);
	return true;
}

std::shared_ptr<FrameReservation> SnapshotStore::ProducerReservation(CaptureId id) const
{
	std::lock_guard lock(m_mutex);
	const auto entry =
		std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& value) { return value.record.id == id; });
	return entry == m_entries.end() || entry->released ? nullptr : entry->producer;
}

std::shared_ptr<FrameReservation> SnapshotStore::ProducerReservation(FrameKey frame, CaptureTarget target) const
{
	std::lock_guard lock(m_mutex);
	for (const auto& entry : m_entries)
		if (!entry.released && entry.record.frame == frame)
		{
			if (entry.record.target == target && entry.producer)
				return entry.producer;
			if (target == CaptureTarget{} && entry.composedProducer)
				return entry.composedProducer;
		}
	return {};
}

bool SnapshotStore::SetProducer(FrameKey frame, std::shared_ptr<FrameReservation> producer,
								CaptureTarget target) noexcept
{
	if (!producer)
		return false;
	std::lock_guard lock(m_mutex);
	const auto matches = [&](const Entry& entry) {
		return !entry.released && entry.record.frame == frame &&
			   (entry.record.phase == SnapshotPhase::Issued || entry.record.phase == SnapshotPhase::Begun) &&
			   (entry.record.target == target ||
				(target == CaptureTarget{} && entry.record.target.IsAdditionalImage()));
	};
	for (const auto& entry : m_entries)
		if (matches(entry) && (entry.record.target == target ? entry.producer : entry.composedProducer))
			return false;
	bool attached = false;
	for (auto& entry : m_entries)
		if (matches(entry))
		{
			(entry.record.target == target ? entry.producer : entry.composedProducer) = producer;
			attached = true;
		}
	return attached;
}

void SnapshotStore::AbandonProducer(FrameKey frame, CaptureTarget target) noexcept
{
	std::lock_guard lock(m_mutex);
	for (auto& entry : m_entries)
		if (entry.record.frame == frame &&
			(entry.record.phase == SnapshotPhase::Issued || entry.record.phase == SnapshotPhase::Begun))
		{
			if (entry.record.target == target)
				entry.producer.reset();
			if (target == CaptureTarget{})
				entry.composedProducer.reset();
		}
}

std::vector<CaptureTarget> SnapshotStore::RequestedTargets(FrameKey frame) const
{
	std::vector<CaptureTarget> result;
	std::lock_guard lock(m_mutex);
	result.reserve(m_entries.size());
	for (const auto& entry : m_entries)
		if (!entry.released && entry.record.frame == frame &&
			(entry.record.phase == SnapshotPhase::Issued || entry.record.phase == SnapshotPhase::Begun) &&
			std::find(result.begin(), result.end(), entry.record.target) == result.end())
			result.push_back(entry.record.target);
	return result;
}

void SnapshotStore::Fail(FrameKey frame, XrResult result, std::shared_ptr<const FrameOutput> output) noexcept
{
	if (XR_SUCCEEDED(result))
		return;
	std::lock_guard lock(m_mutex);
	for (auto& entry : m_entries)
		if (entry.record.frame == frame &&
			(entry.record.phase == SnapshotPhase::Issued || entry.record.phase == SnapshotPhase::Begun))
		{
			entry.record.phase = SnapshotPhase::Failed;
			entry.record.result = result;
			entry.record.output = entry.record.target.kind == CaptureKind::ComposedEyes ? output : nullptr;
			entry.producer.reset();
			entry.composedProducer.reset();
			++entry.revision;
			TraceTerminal(entry);
		}
}

std::optional<SnapshotRecord> SnapshotStore::Read(CaptureId id) const
{
	std::lock_guard lock(m_mutex);
	const auto entry =
		std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& value) { return value.record.id == id; });
	if (entry == m_entries.end() || entry->released)
		return {};
	return entry->record;
}

std::optional<CaptureId> SnapshotStore::Find(FrameKey frame, CaptureTarget target) const
{
	std::lock_guard lock(m_mutex);
	const auto entry = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& value) {
		return !value.released && value.record.frame == frame && value.record.target == target;
	});
	return entry == m_entries.end() ? std::nullopt : std::optional{entry->record.id};
}

std::vector<CaptureId> SnapshotStore::FindAll(FrameKey frame) const
{
	std::vector<CaptureId> result;
	std::lock_guard lock(m_mutex);
	result.reserve(m_entries.size());
	for (const auto& entry : m_entries)
		if (!entry.released && entry.record.frame == frame)
			result.push_back(entry.record.id);
	return result;
}

SnapshotStoreState SnapshotStore::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	SnapshotStoreState state{m_running, m_generation, m_slots.Capacity(), m_slots.InUse()};
	for (const auto& entry : m_entries)
		if (entry.released)
			++state.retiring;
		else
		{
			state.captures.push_back(entry.record.id);
			if (entry.record.pinned)
				++state.pinned;
		}
	return state;
}

std::size_t SnapshotStore::Capacity() const
{
	return m_slots.Capacity();
}

std::optional<SnapshotStore::Entry> SnapshotStore::ReleaseLocked(std::vector<Entry>::iterator entry) noexcept
{
	entry->released = true;
	entry->producer.reset();
	entry->composedProducer.reset();
	if (Pending(entry->record.phase))
		return {};
	std::optional<Entry> released{std::move(*entry)};
	m_entries.erase(entry);
	return released;
}

bool SnapshotStore::Release(CaptureId id) noexcept
{
	std::optional<Entry> released;
	{
		std::lock_guard lock(m_mutex);
		auto entry =
			std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& value) { return value.record.id == id; });
		if (entry == m_entries.end() || entry->released)
			return false;
		released = ReleaseLocked(entry);
	}
	return true;
}

bool SnapshotStore::SetPinned(CaptureId id, bool pinned) noexcept
{
	std::lock_guard lock(m_mutex);
	for (auto& entry : m_entries)
		if (entry.record.id == id && !entry.released)
		{
			entry.record.pinned = pinned;
			return true;
		}
	return false;
}

bool SnapshotStore::EvictOldest() noexcept
{
	std::optional<Entry> released;
	{
		std::lock_guard lock(m_mutex);
		const auto eligible = [](const Entry& value) {
			const auto& record = value.record;
			const auto internalReaders = 1 + (record.display && record.display->output == record.output);
			return !value.released && !record.pinned && record.phase != SnapshotPhase::Reserved &&
				   !Pending(record.phase) && record.m_retention.use_count() == 1 &&
				   (!record.output || record.output.use_count() == internalReaders) &&
				   (!record.metadata || !record.metadata->sequenceCapture ||
					record.metadata->sequenceCapture->reservation.id != record.id);
		};
		const auto entry = std::find_if(m_entries.begin(), m_entries.end(), eligible);
		if (entry == m_entries.end())
			return false;
		released = ReleaseLocked(entry);
	}
	return true;
}

void SnapshotStore::SetCapacity(std::size_t capacity)
{
	std::lock_guard lock(m_mutex);
	m_entries.reserve(capacity);
	m_slots.SetCapacity(capacity);
}

void SnapshotStore::RetireSequenceCaptures(std::optional<CommandSource> owner) noexcept
{
	std::lock_guard lock(m_mutex);
	for (auto& entry : m_entries)
	{
		const auto& metadata = entry.record.metadata;
		if (!metadata || !metadata->sequenceCapture)
			continue;
		const auto& capture = *metadata->sequenceCapture;
		if ((owner && capture.request.owner != *owner) || capture.reservation.id != entry.record.id)
			continue;
		entry.released = true;
		entry.producer.reset();
		entry.composedProducer.reset();
	}
}

void SnapshotStore::RetireAll() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		for (auto& entry : m_entries)
			entry.released = true;
	}
	Poll();
}

void SnapshotStore::Poll() noexcept
{
	CaptureId cursor, limit;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& entry : m_entries)
			limit = std::max(limit, entry.record.id);
	}
	while (true)
	{
		CaptureId id;
		std::shared_ptr<const FrameOutput> output;
		std::uint64_t revision = 0;
		{
			std::lock_guard lock(m_mutex);
			const Entry* next = nullptr;
			for (const auto& entry : m_entries)
				if (entry.record.id > cursor && entry.record.id <= limit &&
					(!next || entry.record.id < next->record.id))
					next = &entry;
			if (!next)
				return;
			id = next->record.id;
			if (next->record.phase == SnapshotPhase::GpuPending)
				output = next->record.output;
			revision = next->revision;
		}
		const auto completion = output ? output->PollCompletion() : CompletionStatus::Complete;
		std::optional<Entry> released;
		{
			std::lock_guard lock(m_mutex);
			auto entry = std::find_if(m_entries.begin(), m_entries.end(),
									  [&](const auto& value) { return value.record.id == id; });
			if (entry != m_entries.end() && entry->revision == revision)
			{
				if (entry->record.phase == SnapshotPhase::GpuPending && completion != CompletionStatus::Pending)
				{
					entry->record.phase =
						completion == CompletionStatus::Complete ? SnapshotPhase::Ready : SnapshotPhase::Failed;
					if (completion == CompletionStatus::Failed)
						entry->record.result = XR_ERROR_RUNTIME_FAILURE;
				}
				TraceTerminal(*entry);
				if (entry->released && !Pending(entry->record.phase))
				{
					released = std::move(*entry);
					m_entries.erase(entry);
				}
			}
		}
		cursor = id;
	}
}
} // namespace simxr
