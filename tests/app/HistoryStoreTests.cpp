#include "../TestSupport.h"
#include "app/FrameHistoryFeed.h"
#include "app/HistoryStore.h"
#include <chrono>
#include <iostream>
#include <limits>

namespace
{
using namespace simxr::app;
void RingAndBuckets()
{
	HistoryStore history(37);
	for (std::uint64_t i = 0; i < 120; ++i)
		history.Append({{4, i / 60 + 1, i % 60}, 7, 0, double(i), float(i % 11), i == 100 ? HistoryLate : 0U});
	Require(history.Size() == 37 && history.At(0).seconds == 83, "History ring did not evict oldest frames");
	for (std::size_t first = 0; first <= history.Size(); ++first)
		for (std::size_t end = first; end <= history.Size(); ++end)
		{
			const auto result = history.Query(first, end);
			Require(result.count == end - first, "Wrapped bucket count differs from frame range");
			if (first == end)
				continue;
			float minimum = 100, maximum = -1;
			std::uint32_t flags = 0;
			for (auto index = first; index < end; ++index)
			{
				minimum = std::min(minimum, history.At(index).frameMilliseconds);
				maximum = std::max(maximum, history.At(index).frameMilliseconds);
				flags |= history.At(index).flags;
			}
			Require(result.minimum == minimum && result.maximum == maximum && result.flags == flags,
					"Bucket lost a one-frame peak or event across ring wrap");
		}
	Require(history.Find({4, 2, 40}, 7) == 17, "Identity lookup lost wrapped frame");
	Require(!history.Find({4, 2, 1}, 7), "Identity lookup retained evicted frame");
	history.SetFlags(17, HistoryMarker, true);
	Require(history.Query(0, 37).flags & HistoryMarker, "Marker absent from density buckets");
	history.SetFlags(17, HistoryMarker, false);
	history.SetCoverage(17, 89, HistorySnapshot);
	Require(history.At(17).identity.frame == 40 && history.At(17).identity.run == 2 && history.At(17).epoch == 7,
			"Coverage changed frame identity");
	Require(history.Query(0, 37).flags == (HistoryLate | HistorySnapshot), "Coverage lost timing flags");
	history.SetCoverage(17, 0, 0);
	Require(history.Query(0, 37).flags == HistoryLate, "Released image coverage remained in ancestor buckets");
	Require(history.Nearest(99.8) == 17 && history.LowerBound(99.8) == 17, "Time hit-testing selected wrong frame");
	Require(history.Nearest(-1) == 0 && history.Nearest(1000) == 36, "Time hit-testing did not clamp edges");
	history.Clear();
	Require(!history.Nearest(0) && history.Query(0, 0).count == 0, "Clear retained old buckets");
	bool rejected = false;
	try
	{
		history.Append({{}, 0, 0, std::numeric_limits<double>::quiet_NaN()});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && history.Size() == 0, "Invalid time damaged history");
}
void RuntimeFeed()
{
	namespace wire = simxr::control;
	namespace tag = wire::tag;
	const auto page = [](std::uint64_t run, std::uint64_t id, std::int64_t time, std::uint64_t frameRun = 0)
	{
		wire::Message message;
		const std::vector<wire::Field> prediction{wire::Field::I64(tag::Time, time),
												  wire::Field::I64(tag::Period, 10000000)};
		const std::vector<wire::Field> frame{wire::Field::U64(tag::Generation, frameRun ? frameRun : run),
											 wire::Field::U64(tag::FrameId, id),
											 wire::Field::Bytes(tag::Prediction, wire::EncodeFields(prediction)),
											 wire::Field::Text(tag::Stage, "discarded")};
		const std::vector<wire::Field> frames{wire::Field::Bytes(tag::Frame, wire::EncodeFields(frame))};
		message.fields = {wire::Field::U64(tag::Session, 9), wire::Field::U64(tag::Generation, run),
						  wire::Field::Bytes(tag::Frames, wire::EncodeFields(frames)), wire::Field::I64(tag::Time, 0)};
		return message;
	};
	FrameHistoryFeed feed;
	feed.SetScope({1, 9, 2});
	feed.Accept(page(2, 10, 100000000));
	feed.Accept(page(2, 10, 100000000));
	feed.Accept(page(2, 12, 120000000));
	Require(feed.History().Size() == 2 && feed.After() == 12, "Feed repeated a paginated frame");
	// Twice the 10 ms period is a missed display time.
	Require(feed.History().At(1).flags == (HistoryDropped | HistoryGap | HistoryLate) &&
				feed.History().At(1).frameMilliseconds == 20 && feed.History().At(1).periodMilliseconds == 10,
			"Feed lost a dropped frame, gap, late frame or runtime interval");
	Require(std::abs(feed.History().At(1).runSeconds - .12) < .000001, "Run-relative time used attachment time");
	feed.SetScope({});
	feed.SetScope({1, 9, 2});
	feed.Accept(page(2, 12, 120000000));
	Require(feed.History().Size() == 2, "Scope revisit duplicated retained frames");
	feed.SetScope({2, 9, 3});
	feed.Accept(page(2, 13, 130000000));
	Require(feed.After() == 0, "Old run response advanced new run cursor");
	feed.Accept(page(3, 1, 1000));
	Require(feed.History().Size() == 3 && feed.History().At(2).epoch == 2 && feed.History().At(2).identity.run == 3 &&
				(feed.History().At(2).flags & HistoryRunStart) &&
				feed.History().At(2).seconds > feed.History().At(1).seconds,
			"Run clocks were joined without a divider");
	// A rejected page is skipped once and leaves a gap, never a retry loop.
	Require(!feed.Accept(page(3, 2, 2000, 4)).empty() && feed.After() == 2, "Invalid history page was not skipped");
	Require(feed.Accept(page(3, 3, 3000)).empty() && feed.History().Size() == 4 &&
				(feed.History().At(3).flags & HistoryGap),
			"Frame after a skipped page lacks its gap");
}
void ThirtyMinutes()
{
	HistoryStore history;
	for (std::uint64_t frame = 0; frame < 162000; ++frame)
		history.Append({{1, 1, frame},
						1,
						0,
						double(frame) / 90,
						frame == 81000 ? 42.0f : 11.1f,
						frame == 81000 ? HistoryDropped : 0U});
	const auto started = std::chrono::steady_clock::now();
	std::size_t records = 0, lateColumns = 0;
	for (int draw = 0; draw < 300; ++draw)
		for (std::size_t column = 0; column < 1280; ++column)
		{
			const auto bucket = history.Query(column * history.Size() / 1280, (column + 1) * history.Size() / 1280);
			records += bucket.count;
			lateColumns += (bucket.flags & HistoryDropped) != 0;
		}
	Require(records == 300 * history.Size() && lateColumns == 300, "Zoomed-out canvas lost records or drop marker");
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::cout << "162000 records, 1280 columns: " << seconds * 1000 / 300 << " ms per query pass\n";
}
} // namespace
int main()
{
	try
	{
		RingAndBuckets();
		RuntimeFeed();
		ThirtyMinutes();
		std::cout << "PASS history wrap, min/max/flags aggregation, identity and binary hit-testing\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
