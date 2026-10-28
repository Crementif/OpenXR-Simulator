#include "../TestSupport.h"
#include "graphics/FrameOutput.h"
#include "presentation/FramePreviewHistory.h"
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <utility>

static thread_local bool s_failPreviewAllocation = false;
void* operator new(std::size_t size)
{
	if (std::exchange(s_failPreviewAllocation, false))
		throw std::bad_alloc{};
	if (auto* memory = std::malloc(size ? size : 1))
		return memory;
	throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* memory) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

namespace simxr::test
{
class PreviewOutput final : public FrameOutput
{
  public:
	explicit PreviewOutput(std::shared_ptr<FrameReservation> reservation) : m_reservation(std::move(reservation)) {}
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}

  private:
	std::shared_ptr<FrameReservation> m_reservation;
};
class CompressedOutput;
class CompressedPicture final : public ArchivedFrame
{
  public:
	explicit CompressedPicture(std::uint64_t value) : value(value) {}
	std::uint64_t Bytes() const noexcept override
	{
		return 4ull << 20;
	}
	std::shared_ptr<const FrameOutput> Decode(const FramePool& budget) const override;
	std::uint64_t value;
};
class CompressedOutput final : public FrameOutput
{
  public:
	CompressedOutput(std::shared_ptr<FrameReservation> reservation, std::uint64_t value)
		: reservation(std::move(reservation)), value(value)
	{
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return pending.load() ? CompletionStatus::Pending : CompletionStatus::Complete;
	}
	bool CanArchive() const noexcept override
	{
		return true;
	}
	std::shared_ptr<const ArchivedFrame> Archive() const override
	{
		return std::make_shared<CompressedPicture>(value);
	}
	std::shared_ptr<FrameReservation> reservation;
	std::atomic<bool> pending{false};
	std::uint64_t value;
};
std::shared_ptr<const FrameOutput> CompressedPicture::Decode(const FramePool& budget) const
{
	auto reservation = budget.Reserve();
	if (!reservation)
		return {};
	Require(budget.Consume(reservation), "Decoded reservation failed");
	return std::make_shared<CompressedOutput>(std::move(reservation), value);
}
template <typename Predicate> void Await(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate())
	{
		Require(std::chrono::steady_clock::now() < deadline, "History worker timed out");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
void TrackerAdmission()
{
	FramePreviewHistory history;
	TrackingSnapshot tracking;
	tracking.trackers.resize(MaxTrackerDevices);
	tracking.input.trackers.resize(MaxTrackerDevices);
	for (std::size_t index = 0; index < tracking.trackers.size(); ++index)
	{
		tracking.trackers[index].device = {{index + 1}, TrackerRole::Unassigned};
		tracking.input.trackers[index] = tracking.trackers[index].device;
	}
	for (std::uint64_t frame = 1; frame <= 2; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Tracker preview admission failed");
		auto output = std::make_shared<PreviewOutput>(reservation);
		s_failPreviewAllocation = true;
		history.Append({{1}, {frame}}, output, false, frame == 1 ? &tracking : nullptr);
		const bool didNotAllocate = std::exchange(s_failPreviewAllocation, false);
		Require(didNotAllocate, "Preview publication allocated after native admission");
	}
	const auto retained = history.Tracking({{1}, {1}});
	Require(retained && retained->trackers.size() == MaxTrackerDevices &&
				retained->input.trackers.size() == MaxTrackerDevices &&
				retained->trackers.back().device.identity.value == MaxTrackerDevices,
			"Preview retention truncated the tracker inventory");
	Require(!history.Tracking({{1}, {2}}) && !history.Find({{1}, {2}}).tracking,
			"Reserved storage fabricated an absent tracking snapshot");
	tracking.trackers.clear();
	Require(retained->trackers.size() == MaxTrackerDevices,
			"Retained tracker inventory aliases mutable publication input");
	history.Append({{1}, {3}}, {}, true, &tracking);
	Require(history.Tracking({{1}, {3}})->trackers.empty(), "Clear preview fabricated default trackers");
	tracking.trackers.resize(MaxTrackerDevices + 1);
	bool rejected = false;
	try
	{
		history.Append({{1}, {4}}, {}, true, &tracking);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && !history.Find({{1}, {4}}).submitted, "Over-capacity tracker inventory published a preview");
}

void MissingPreviewDoesNotBlockArchival()
{
	FramePreviewHistory history;
	history.Configure(64ull << 20);
	for (std::uint64_t frame = 1; frame <= 48; ++frame)
	{
		const FrameKey key{{1}, {frame}};
		auto reservation = history.Reserve(key);
		Require(static_cast<bool>(reservation), "Preview gap exhausted admission");
		if (frame % 2)
		{
			history.Append(key, {}, false);
			Require(!history.Find(key).submitted, "Missing pixels became a submitted preview");
		}
		else
		{
			Require(history.Budget().Consume(reservation), "Consume recovered preview admission");
			history.Append(key, std::make_shared<CompressedOutput>(std::move(reservation), frame), false);
		}
	}
	Eventually(
		[&]
		{
			return history.RamBytes() > 0 && history.Bytes() <= 8 * FramePreviewHistory::SlotBytes;
		},
		"Missing preview blocked archival of later frames");
	bool rejected = false;
	try
	{
		history.RetainRange({{1}, {1}}, {{1}, {2}});
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	Require(rejected, "Clip export silently crossed a missing preview");
}

void CompressedHistory()
{
	FramePreviewHistory history;
	history.Configure(64ull << 20);
	history.ConfigureRam(128ull << 20);
	std::shared_ptr<const FrameOutput> retained;
	for (std::uint64_t frame = 1; frame <= 32; ++frame)
	{
		std::shared_ptr<FrameReservation> reservation;
		Await(
			[&]
			{
				reservation = history.Reserve({{1}, {frame}});
				return bool(reservation);
			});
		Require(history.Budget().Consume(reservation), "Compressed admission failed");
		auto output = std::make_shared<CompressedOutput>(reservation, frame);
		TrackingSnapshot tracking{};
		tracking.head.pose.position.x = static_cast<float>(frame);
		history.Append({{1}, {frame}}, output, false, &tracking);
		if (frame == 1)
			retained = output;
	}
	Await(
		[&]
		{
			return history.RamBytes() >= (96ull << 20);
		});
	Require(history.Frames({1}).size() == 32, "Encoding skipped a retained frame");
	for (const auto frame : {1ull, 17ull, 2ull, 24ull, 32ull})
	{
		FramePreview preview;
		Await(
			[&]
			{
				preview = history.Find({{1}, {frame}});
				return bool(preview.output);
			});
		Require(std::static_pointer_cast<const CompressedOutput>(preview.output)->value == frame,
				"Decode substituted a frame");
		Require(preview.tracking && preview.tracking->head.pose.position.x == static_cast<float>(frame),
				"Archived pose did not survive with its image");
	}
	history.ConfigureRam(64ull << 20);
	Await(
		[&]
		{
			return history.RamBytes() <= history.RamLimit();
		});
	const auto frames = history.Frames({1});
	Require(frames.size() == 23 && !history.Find({{1}, {9}}).submitted && history.Find({{1}, {10}}).submitted,
			"RAM shrink did not evict the oldest compressed frames");
	Require(std::static_pointer_cast<const CompressedOutput>(retained)->value == 1, "Eviction invalidated reader");
	Require(history.Error().empty(), "History worker failed");
}
void LiveAdmissionDoesNotWaitForEncoding()
{
	FramePreviewHistory history;
	history.Configure(64ull << 20);
	std::shared_ptr<CompressedOutput> pending;
	for (std::uint64_t frame = 1; frame <= 16; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Fill live history budget");
		auto output = std::make_shared<CompressedOutput>(reservation, frame);
		if (frame == 1)
		{
			pending = output;
			pending->pending.store(true);
		}
		history.Append({{1}, {frame}}, std::move(output), false);
	}
	const auto before = std::chrono::steady_clock::now();
	auto unavailable = history.Reserve({{1}, {17}}, false);
	const auto elapsed = std::chrono::steady_clock::now() - before;
	Require(!unavailable && elapsed < std::chrono::milliseconds(250), "Live history waited for pending encoding");
	Require(history.Bytes() == history.Limit() && !history.Find({{1}, {17}}).submitted,
			"Live admission exceeded its budget or fabricated a frame");
	pending->pending.store(false);
	pending.reset();
	Eventually(
		[&]
		{
			return history.Bytes() < history.Limit();
		},
		"Encoding did not release history capacity");
	auto recovered = history.Reserve({{1}, {18}}, false);
	Require(recovered && history.Budget().Consume(recovered), "Live history did not recover after encoding");
	history.Append({{1}, {18}}, std::make_shared<CompressedOutput>(std::move(recovered), 18), false);
}

void ShrinkWaitsForEncoding()
{
	FramePreviewHistory history;
	history.Configure(128ull << 20);
	std::shared_ptr<CompressedOutput> pending;
	for (std::uint64_t frame = 1; frame <= 32; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Shrink fixture admission failed");
		auto output = std::make_shared<CompressedOutput>(reservation, frame);
		if (frame == 1)
		{
			pending = output;
			pending->pending.store(true);
		}
		history.Append({{1}, {frame}}, std::move(output), false);
	}
	history.Configure(64ull << 20);
	Require(history.Bytes() > history.Limit(), "Shrink fixture did not retain pending GPU work");
	std::jthread complete(
		[pending]
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			pending->pending.store(false);
		});
	auto reservation = history.Reserve({{1}, {33}});
	Require(reservation && history.Budget().Consume(reservation), "Budget shrink rejected the next frame");
	history.Append({{1}, {33}}, std::make_shared<CompressedOutput>(reservation, 33), false);
	Require(history.Frames({1}).size() == 33, "Budget shrink lost a frame instead of encoding it");
	Require(history.Bytes() <= history.Limit(), "Admission exceeded the smaller GPU budget");
}

void CloseWakesAdmission()
{
	FramePreviewHistory history;
	history.Configure(64ull << 20);
	for (std::uint64_t frame = 1; frame <= 16; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Close fixture admission failed");
		auto output = std::make_shared<CompressedOutput>(std::move(reservation), frame);
		output->pending.store(true);
		history.Append({{1}, {frame}}, std::move(output), false);
	}
	std::promise<void> started;
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  started.set_value();
								  return history.Reserve({{1}, {17}});
							  });
	started.get_future().wait();
	Require(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
			"Full history did not apply backpressure");
	history.CloseAdmission();
	Require(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !waiting.get(),
			"Closing history left frame admission blocked");
	Require(!history.Reserve({{1}, {18}}) && history.Frames({1}).size() == 16,
			"Closing admission removed history or accepted another frame");
}

void PendingEncodeDoesNotBlockSelection()
{
	FramePreviewHistory history;
	history.Configure(64ull << 20);
	std::shared_ptr<CompressedOutput> pending;
	for (std::uint64_t frame = 1; frame <= 10; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Pending history admission failed");
		auto output = std::make_shared<CompressedOutput>(reservation, frame);
		if (frame == 2)
		{
			pending = output;
			pending->pending.store(true);
		}
		history.Append({{1}, {frame}}, std::move(output), false);
	}
	Await(
		[&]
		{
			return history.RamBytes() > 0;
		});
	FramePreview selected;
	Await(
		[&]
		{
			selected = history.Find({{1}, {1}});
			return bool(selected.output);
		});
	Require(std::static_pointer_cast<const CompressedOutput>(selected.output)->value == 1,
			"Pending encode substituted the requested archive");
	pending->pending.store(false);
	auto abandoned = history.Reserve({{1}, {11}});
	abandoned.reset();
	Require(bool(history.Reserve({{1}, {11}})), "Abandoned admission blocked a retry");
}
void ContinuousHistory()
{
	FramePreviewHistory history;
	std::shared_ptr<const FrameOutput> retained;
	for (std::uint64_t frame = 1; frame <= 400; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Continuous preview admission failed");
		auto image = std::make_shared<PreviewOutput>(std::move(reservation));
		history.Append({{1}, {frame}}, image, false);
		if (frame == 1)
			retained = image;
		Require(history.Find({{1}, {frame}}).output == image, "Preview identity changed");
		Require(history.Bytes() <= FramePreviewHistory::BudgetBytes, "Retired reader escaped byte accounting");
	}
	Require(retained && !history.Find({{1}, {1}}).output, "Oldest eviction invalidated an independent reader");
	for (std::uint64_t frame = 300; frame <= 400; ++frame)
		Require(history.Find({{1}, {frame}}).output != nullptr, "A consecutive preview was skipped");
	Require(!history.Find({{2}, {400}}).submitted, "History substituted a different run");
	history.Append({{1}, {401}}, {}, true);
	Require(history.Find({{1}, {401}}).clear && !history.Find({{1}, {401}}).output, "Clear became stale pixels");
	auto before = history.Find({{1}, {400}}).output;
	for (int read = 0; read < 200; ++read)
		Require(history.Find({{1}, {400}}).output == before, "Idle history discarded a paused frame");
	before.reset();
	retained.reset();
	history.Configure(2048ull << 20);
	for (std::uint64_t frame = 402; frame <= 1000; ++frame)
	{
		auto reservation = history.Reserve({{1}, {frame}});
		Require(reservation && history.Budget().Consume(reservation), "Expanded history admission failed");
		history.Append({{1}, {frame}}, std::make_shared<PreviewOutput>(reservation), false);
	}
	Require(history.Frames({1}).size() == 513 && history.Find({{1}, {489}}).output,
			"Expanded history remained limited to the original ring");
	auto pinned = history.Find({{1}, {489}}).output;
	history.Configure(64ull << 20);
	Require(pinned && history.Limit() == (64ull << 20) && history.Bytes() <= history.Limit() &&
				!history.Find({{1}, {489}}).output && history.Find({{1}, {1000}}).output,
			"Shrinking lost the newest frame, invalidated a reader, or exceeded its budget");
	try
	{
		history.Configure(65ull << 20);
		throw std::runtime_error("Unaligned budget accepted");
	}
	catch (const std::invalid_argument&)
	{
	}
}
} // namespace simxr::test
int main()
{
	try
	{
		simxr::test::TrackerAdmission();
		simxr::test::ContinuousHistory();
		simxr::test::CompressedHistory();
		simxr::test::MissingPreviewDoesNotBlockArchival();
		simxr::test::ShrinkWaitsForEncoding();
		simxr::test::LiveAdmissionDoesNotWaitForEncoding();
		simxr::test::CloseWakesAdmission();
		simxr::test::PendingEncodeDoesNotBlockSelection();
		std::cout << "PASS consecutive previews, oldest eviction, independent readers, clear and run identity\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
