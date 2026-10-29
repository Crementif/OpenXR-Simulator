#include "automation/RecordingPlayback.h"
#include "Pch.h"
#include "automation/RecordingArchive.h"
#include "automation/RigProgress.h"
#include "automation/RigRuntime.h"

namespace simxr::automation
{
static std::uint64_t AddFrame(std::uint64_t frame, std::uint64_t offset)
{
	if (offset > std::numeric_limits<std::uint64_t>::max() - frame)
		throw std::overflow_error("Recording exceeds the replay frame range");
	return frame + offset;
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
			const auto frame = ReplayFrame(transaction, reader.State().origin.originFrame, page->recording.version);
			if (!frame)
				continue;
			const auto relative = frame->value - reader.State().origin.originFrame.value;
			if (relative < previous)
				throw std::runtime_error("Recording applied frames are out of order");
			group = relative == previous ? group + 1 : 1;
			if (group > 256)
				throw std::runtime_error("Recording exceeds the replay transaction window");
			previous = relative;
			for (const auto& command : transaction.commands)
				(void)ReplayCommand(command, transaction.runtimeTime);
		}
	}
	if (!reader.State().complete)
		throw std::runtime_error("Replay requires a complete recording archive");
	return reader.State();
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
	runtime.AdvanceToFrame(FrameId{base});
	runtime.AwaitTransaction(initial);
	std::vector<std::uint64_t> pending;
	pending.reserve(256);
	std::uint64_t target = base;
	const auto finishFrame = [&] {
		if (pending.empty())
			return;
		runtime.AdvanceToFrame(FrameId{target});
		for (const auto transaction : pending)
			runtime.AwaitTransaction(transaction);
		pending.clear();
	};
	while (auto page = reader.Next())
	{
		progress.Check(true);
		for (auto& transaction : page->recording.transactions)
		{
			const auto frame = ReplayFrame(transaction, reader.State().origin.originFrame, page->recording.version);
			if (!frame)
				continue;
			const auto next = AddFrame(base, frame->value - reader.State().origin.originFrame.value);
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
