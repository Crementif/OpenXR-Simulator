#include "automation/RecordingRange.h"
#include "Pch.h"
#include "automation/RecordingArchive.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "simulation/CommandState.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace simxr::automation
{
static constexpr CommandSource s_source{CommandSourceKind::Rig, 1};
static RuntimeTime Anchor(const std::optional<RuntimeTime>& time)
{
	return time.value_or(RuntimeTime{Duration{1}});
}
InputRecording ReadRecordingRange(const std::filesystem::path& directory, FrameId first, FrameId last,
								  std::stop_token stop)
{
	if (last < first)
		throw std::invalid_argument("Choose an ordered range of recorded frames");
	RecordingArchiveReader reader(directory);
	const auto origin = reader.State().origin;
	if (first < origin.originFrame)
		throw std::invalid_argument("Range starts before the recording origin");
	CommandState state;
	CommandConfiguration baseline;
	CommandTransaction initial{1, s_source, {}, CommandLatePolicy::Reject, ReplayInitialCommands(origin)};
	if (state.Apply(initial, baseline, Anchor(origin.originRuntimeTime)) != CommandResult::Applied)
		throw std::runtime_error("Cannot restore the recorded range origin");
	baseline = state.RecordingOrigin(baseline, Anchor(origin.originRuntimeTime)).configuration;
	bool foundFirst = first == origin.originFrame, foundLast = last == origin.originFrame;
	std::optional<RuntimeTime> firstTime = foundFirst ? origin.originRuntimeTime : std::nullopt;
	std::uint64_t previous = origin.originFrame.value;
	std::size_t bytes = 0;
	std::vector<RecordedTransaction> selected;
	while (auto page = reader.Next(stop))
	{
		if (stop.stop_requested())
			throw std::runtime_error("Recording range export cancelled");
		for (auto& transaction : page->recording.transactions)
		{
			if (transaction.commands.empty() || transaction.commands.size() > 64 ||
				(transaction.afterFrame && transaction.phase != "released"))
				throw std::runtime_error("Malformed recorded range transaction");
			if (transaction.phase == "released" &&
				(transaction.id || !std::ranges::all_of(transaction.commands,
														[](const auto& command)
														{
															return std::holds_alternative<ReleaseControl>(command);
														})))
				throw std::runtime_error("Malformed recorded release event");
			if (transaction.phase != "applied" && transaction.phase != "released")
			{
				constexpr std::array phases{"queued",  "late",	   "rescheduled", "conflict", "cancelled",
											"invalid", "capacity", "closed",	  "not_found"};
				if (std::find(phases.begin(), phases.end(), transaction.phase) == phases.end())
					throw std::runtime_error("Unknown recorded transaction phase");
				continue;
			}
			if (transaction.afterFrame && transaction.frame.value == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("Recorded release exceeds the frame range");
			const auto effective = transaction.frame.value + (transaction.afterFrame ? 1 : 0);
			if (effective < previous)
				throw std::runtime_error("Recorded input frames are out of order");
			previous = effective;
			if (transaction.phase == "applied")
			{
				foundLast |= transaction.frame == last;
				if (transaction.frame == first)
				{
					if (foundFirst && firstTime != transaction.runtimeTime)
						throw std::runtime_error("Range boundary has inconsistent runtime anchors");
					foundFirst = true;
					firstTime = transaction.runtimeTime;
				}
			}
			if (effective <= first.value)
			{
				if (transaction.releaseState)
				{
					InputRecording fallback;
					fallback.version = 4;
					fallback.initial = transaction.releaseState->initial;
					fallback.initialScenarios = transaction.releaseState->scenarios;
					fallback.originRuntimeTime = transaction.releaseState->runtimeTime;
					CommandState restored;
					CommandTransaction apply{
						transaction.id, s_source, {}, CommandLatePolicy::Reject, ReplayInitialCommands(fallback)};
					if (restored.Apply(apply, baseline, *fallback.originRuntimeTime) != CommandResult::Applied)
						throw std::runtime_error("Cannot restore the recorded release state");
					state.Swap(restored);
				}
				else
				{
					if (std::ranges::any_of(transaction.commands,
											[](const auto& command)
											{
												return std::holds_alternative<ReleaseControl>(command);
											}))
						throw std::runtime_error(
							"Cannot trim past a released channel without its recorded fallback state");
					CommandTransaction apply{
						transaction.id, s_source, {}, CommandLatePolicy::Reject, transaction.commands};
					if (state.Apply(apply, baseline, Anchor(transaction.runtimeTime)) != CommandResult::Applied)
						throw std::runtime_error("Cannot reconstruct the selected range's initial rig");
				}
			}
			else if (effective <= last.value)
			{
				if (selected.size() == 1024)
					throw std::length_error("Editable range exceeds 1024 outcomes; select a shorter range");
				for (const auto& command : transaction.commands)
				{
					const auto size = control::EncodeSimulationCommand(command).value.size();
					if (size > 64 * 1024 * 1024 - bytes)
						throw std::length_error("Editable range exceeds its 64 MiB input budget");
					bytes += size;
				}
				transaction.releaseState.reset();
				selected.push_back(std::move(transaction));
			}
		}
	}
	if (!reader.State().complete)
		throw std::runtime_error("Range export requires a complete recording archive");
	if (!foundFirst || !foundLast)
		throw std::invalid_argument("Choose endpoints with recorded applied input, or the recording origin");
	CommandRecording snapshot;
	snapshot.version = 4;
	snapshot.origin = state.RecordingOrigin(baseline, Anchor(firstTime));
	snapshot.origin.seed = origin.seed;
	snapshot.origin.frame = first;
	snapshot.origin.runtimeTime = firstTime;
	control::Message message;
	message.fields.push_back(
		control::Field::Bytes(control::tag::Document, control::SimulationRecording(snapshot).Encode()));
	auto result = DecodeRecordingPage(message).recording;
	result.transactions = std::move(selected);
	result.total = result.transactions.size();
	return result;
}
} // namespace simxr::automation
