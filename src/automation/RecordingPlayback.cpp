#include "automation/RecordingPlayback.h"
#include "Pch.h"
#include "automation/RecordingArchive.h"
#include "automation/RigProgress.h"
#include "automation/RigRuntime.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace simxr::automation
{
static std::uint64_t AddFrame(std::uint64_t frame, std::uint64_t offset)
{
	if (offset > std::numeric_limits<std::uint64_t>::max() - frame)
		throw std::overflow_error("Recording exceeds the replay frame range");
	return frame + offset;
}
static std::optional<std::uint64_t> RelativeFrame(const RecordedTransaction& transaction, FrameId origin)
{
	if (transaction.commands.empty() || transaction.commands.size() > 64 ||
		(transaction.afterFrame && transaction.phase != "released"))
		throw std::runtime_error("Malformed recorded transaction");
	if (transaction.phase == "released" &&
		(transaction.id || !std::all_of(transaction.commands.begin(), transaction.commands.end(),
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
		return {};
	}
	if (transaction.frame < origin || (transaction.frame == origin && !transaction.afterFrame))
		throw std::runtime_error("Applied recording transaction precedes its origin");
	return AddFrame(transaction.frame.value - origin.value, transaction.afterFrame ? 1 : 0);
}
static RecordingArchiveState Validate(const std::filesystem::path& directory, RigProgress& progress)
{
	RecordingArchiveReader reader(directory);
	(void)ReplayInitialCommands(reader.State().origin);
	std::uint64_t previous = 0;
	std::size_t group = 0;
	while (const auto page = reader.Next())
	{
		progress.Check(true);
		for (const auto& transaction : page->recording.transactions)
		{
			const auto relative = RelativeFrame(transaction, reader.State().origin.originFrame);
			if (!relative)
				continue;
			if (*relative < previous)
				throw std::runtime_error("Recording applied frames are out of order");
			group = *relative == previous ? group + 1 : 1;
			if (group > 256)
				throw std::runtime_error("Recording exceeds the replay transaction window");
			previous = *relative;
			for (const auto& command : transaction.commands)
				(void)ReplayCommand(command, transaction.runtimeTime);
		}
	}
	if (!reader.State().complete)
		throw std::runtime_error("Replay requires a complete recording archive");
	return reader.State();
}
static void ReleaseStep(RigRuntime& runtime, const ScriptStep& step)
{
	if (step.capture)
		runtime.ReleaseCapture(*step.capture);
}
static void Advance(RigRuntime& runtime, RigProgress& progress, std::uint64_t target)
{
	while (runtime.CurrentFrame().value < target)
	{
		progress.Check(true);
		ReleaseStep(runtime, runtime.StepApplication({}));
		const auto applied = runtime.CurrentFrame();
		for (;;)
		{
			const auto displayed = runtime.StepDisplay();
			ReleaseStep(runtime, displayed);
			if (displayed.frame && *displayed.frame >= applied)
				break;
			progress.Check();
		}
	}
}
void ReplayRecordingArchive(const std::filesystem::path& directory, std::uint64_t seed, RigRuntime& runtime,
							RigProgress& progress)
{
	const auto validated = Validate(directory, progress);
	if (validated.origin.seed != seed)
		throw std::runtime_error("Recording seed differs");
	RecordingArchiveReader reader(directory);
	if (reader.State().identity != validated.identity || reader.State().origin.seed != seed)
		throw std::runtime_error("Recording changed before replay");
	runtime.PauseApplication();
	const auto base = AddFrame(runtime.CurrentFrame().value, 1);
	const auto initial =
		runtime.Schedule({FrameId{base}, {}}, CommandLatePolicy::Reject, ReplayInitialCommands(reader.State().origin));
	Advance(runtime, progress, base);
	runtime.AwaitTransaction(initial);
	std::vector<std::uint64_t> pending;
	pending.reserve(256);
	std::uint64_t target = base;
	const auto finishFrame = [&]
	{
		if (pending.empty())
			return;
		Advance(runtime, progress, target);
		for (const auto transaction : pending)
			runtime.AwaitTransaction(transaction);
		pending.clear();
	};
	while (auto page = reader.Next())
	{
		progress.Check(true);
		for (auto& transaction : page->recording.transactions)
		{
			const auto relative = RelativeFrame(transaction, reader.State().origin.originFrame);
			if (!relative)
				continue;
			const auto next = AddFrame(base, *relative);
			if (next < target || (next == target && pending.size() == 256))
				throw std::runtime_error("Recording changed its replay order or transaction window");
			if (next != target)
			{
				finishFrame();
				target = next;
			}
			for (auto& command : transaction.commands)
				command = ReplayCommand(std::move(command), transaction.runtimeTime);
			pending.push_back(
				runtime.Schedule({FrameId{target}, {}}, CommandLatePolicy::Reject, std::move(transaction.commands)));
		}
	}
	if (!reader.State().complete || reader.State().next != validated.next)
		throw std::runtime_error("Recording changed or became incomplete during replay");
	finishFrame();
	runtime.ReleaseInputs();
	runtime.ResumeApplication();
	progress.Checkpoint({"recording replay", {}, true});
}
} // namespace simxr::automation
