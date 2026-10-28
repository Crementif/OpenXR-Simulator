#include "automation/RecordingEvidence.h"
#include "Pch.h"
#include "automation/RecordingArchive.h"

namespace simxr::automation
{
static std::optional<std::int64_t> Add(std::int64_t value, std::int64_t delta)
{
	if (value <= 0 || (delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta) ||
		(delta < 0 && value <= -delta))
		return {};
	return value + delta;
}
RecordingEvidence ReadRecordingEvidence(const std::filesystem::path& directory, std::uint64_t cursor,
										std::stop_token stop)
{
	RecordingArchiveReader reader(directory);
	RecordingEvidence result;
	if (!reader.State().game)
		return result;
	result.available = true;
	const auto origin = reader.State().game->origin;
	result.initial = origin.journal.state;
	result.first = origin.journal.total;
	result.next = std::max(cursor, result.first);
	std::vector<std::optional<std::pair<std::int64_t, std::int64_t>>> intervals;
	std::optional<FrameNativeClockMapping> lastClock = origin.clock;
	while (const auto event = reader.NextEvent())
	{
		if (stop.stop_requested())
			throw std::runtime_error("SDK evidence read cancelled");
		const auto* page = std::get_if<RecordedGamePage>(&*event);
		if (!page)
			continue;
		if (page->clock)
			lastClock = page->clock;
		for (const auto& observation : page->journal.observations)
		{
			if (observation.sequence < std::max(cursor, result.first))
				continue;
			if (result.entries.size() == 16)
			{
				result.more = true;
				continue;
			}
			result.entries.push_back({observation});
			result.next = observation.sequence + 1;
			std::optional<std::pair<std::int64_t, std::int64_t>> interval;
			if (page->clock && observation.time && page->clock->domain == observation.time->domain)
			{
				const auto delta = observation.time->nanoseconds - page->clock->nativeNanoseconds;
				const auto before = Add(page->clock->runtimeBefore.value.count(), delta);
				const auto after = Add(page->clock->runtimeAfter.value.count(), delta);
				const auto resolution =
					std::max(page->clock->resolutionNanoseconds, observation.time->resolutionNanoseconds);
				const auto low = before ? Add(*before, -resolution) : std::nullopt;
				const auto high = after ? Add(*after, resolution) : std::nullopt;
				if (low && high)
					interval = std::pair{*low, *high};
			}
			intervals.push_back(interval);
		}
	}
	result.complete = reader.State().complete;
	result.gap = reader.State().game->gap || reader.State().game->failed;
	result.correlated = !reader.State().game->uncorrelated;
	if (!result.complete || result.gap || !result.correlated || !origin.clock || !lastClock)
		return result;
	for (std::size_t index = 0; index < result.entries.size(); ++index)
	{
		const auto& time = result.entries[index].observation.time;
		if (!time || time->nanoseconds < origin.clock->nativeNanoseconds ||
			time->nanoseconds > lastClock->nativeNanoseconds)
			intervals[index].reset();
	}
	RecordingArchiveReader inputs(directory);
	bool ordered = true;
	std::optional<RuntimeTime> previous;
	std::optional<FrameId> previousFrame;
	const auto consider = [&](FrameId frame, std::optional<RuntimeTime> time)
	{
		if (!time || (previous &&
					  (*time < *previous || frame < *previousFrame || (frame == *previousFrame && *time != *previous))))
		{
			ordered = false;
			return;
		}
		previous = time;
		previousFrame = frame;
		for (std::size_t index = 0; index < result.entries.size(); ++index)
		{
			if (!intervals[index])
				continue;
			auto& entry = result.entries[index];
			if (time->value.count() <= intervals[index]->first)
				entry.first = frame;
			if (!entry.last && time->value.count() >= intervals[index]->second)
				entry.last = frame;
		}
	};
	if (inputs.State().origin.originRuntimeTime)
		consider(inputs.State().origin.originFrame, inputs.State().origin.originRuntimeTime);
	while (auto page = inputs.Next(stop))
		for (const auto& transaction : page->recording.transactions)
			if (transaction.phase == "applied")
				consider(transaction.frame, transaction.runtimeTime);
	if (!inputs.State().complete || inputs.State().identity != reader.State().identity ||
		inputs.State().next != reader.State().next || inputs.State().session != reader.State().session ||
		inputs.State().generation != reader.State().generation)
		throw std::runtime_error("Recording changed while SDK evidence was correlated");
	for (auto& entry : result.entries)
		if (!ordered || !entry.first || !entry.last || *entry.last < *entry.first)
		{
			entry.first.reset();
			entry.last.reset();
		}
	return result;
}
} // namespace simxr::automation
