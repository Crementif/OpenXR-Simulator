#include "../TestSupport.h"
#include "graphics/FrameOutput.h"
#include "presentation/FramePreviewHistory.h"
#include <iostream>

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
		return CompletionStatus::Complete;
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
		history.Append({{1}, {frame}}, output, false);
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
void ContinuousHistory()
{
	FramePreviewHistory history;
	std::shared_ptr<const FrameOutput> retained;
	for (std::uint64_t frame = 1; frame <= 400; ++frame)
	{
		auto reservation = history.Reserve();
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
		auto reservation = history.Reserve();
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
		simxr::test::ContinuousHistory();
		simxr::test::CompressedHistory();
		std::cout << "PASS consecutive previews, oldest eviction, independent readers, clear and run identity\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
