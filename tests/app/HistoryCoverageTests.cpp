#include "../TestSupport.h"
#include "app/services/HistoryCoverage.h"
#include "app/services/HistoryStore.h"
#include <iostream>

namespace simxr::test
{
using namespace simxr::app;

static void OverlappingSources()
{
	HistoryStore history(8);
	history.Append({{1, 2, 3}, 4, 0, 1, 16, HistoryLate | HistoryMarker});
	HistoryCoverage coverage;
	const auto publish = [&](bool snapshot, bool preview, bool pin)
	{
		coverage.Begin();
		if (preview)
			coverage.Add({1, 2, 3}, 4, 0, HistoryPreview);
		if (snapshot)
		{
			coverage.Add({1, 2, 3}, 4, 12, HistorySnapshot);
			coverage.Add({1, 2, 3}, 4, 9, HistorySnapshot);
		}
		if (pin)
			coverage.Add({1, 2, 3}, 4, 0, HistoryPinned);
		coverage.Apply(history);
	};
	publish(true, true, true);
	Require(history.At(0).capture == 9 && history.Query(0, 1).flags == (HistoryLate | HistoryMarker | HistoryPreview |
																		HistorySnapshot | HistoryPinned),
			"Overlapping coverage lost a source or changed capture priority");
	publish(true, true, true);
	Require(history.At(0).capture == 9 && history.Query(0, 1).missed == 1,
			"Repeated coverage changed capture or timing buckets");
	publish(false, true, true);
	Require(history.At(0).capture == 0 &&
				history.Query(0, 1).flags == (HistoryLate | HistoryMarker | HistoryPreview | HistoryPinned),
			"Releasing a snapshot removed another coverage source");
	publish(false, false, true);
	Require(history.Query(0, 1).flags == (HistoryLate | HistoryMarker | HistoryPinned),
			"Preview removal cleared an independent pin");
	publish(false, false, false);
	Require(history.Query(0, 1).flags == (HistoryLate | HistoryMarker),
			"Coverage removal retained stale flags or removed unrelated flags");
}

static void UnsortedSources()
{
	HistoryStore history(8);
	for (std::uint64_t frame = 1; frame <= 5; ++frame)
		history.Append({{1, 1, frame}, 1, 0, double(frame), 16, HistoryAction});
	HistoryCoverage coverage;
	coverage.Begin();
	coverage.Add({1, 1, 3}, 1, 30, HistorySnapshot);
	coverage.Add({1, 1, 1}, 1, 0, HistoryPreview);
	coverage.Add({1, 1, 5}, 1, 0, HistoryPinned);
	coverage.Add({1, 1, 3}, 1, 0, HistoryPreview);
	coverage.Add({1, 1, 3}, 1, 29, HistorySnapshot);
	coverage.Add({1, 1, 4}, 1, 0, HistoryPreview);
	coverage.Add({1, 1, 1}, 1, 0, HistoryPinned);
	coverage.Apply(history);
	Require(history.At(0).flags == (HistoryAction | HistoryPreview | HistoryPinned) &&
				history.At(2).flags == (HistoryAction | HistoryPreview | HistorySnapshot) &&
				history.At(2).capture == 29 && history.At(4).flags == (HistoryAction | HistoryPinned),
			"Unsorted sources lost duplicate coverage or insertion priority");
	coverage.Begin();
	coverage.Add({1, 1, 5}, 1, 0, HistoryPreview);
	coverage.Add({1, 1, 2}, 1, 41, HistorySnapshot);
	coverage.Apply(history);
	Require(history.At(0).flags == HistoryAction && history.At(2).flags == HistoryAction &&
				history.At(3).flags == HistoryAction && history.At(4).flags == (HistoryAction | HistoryPreview) &&
				history.At(1).capture == 41,
			"Interleaved additions and removals left stale coverage");
}

static void ScopeAndRingChanges()
{
	HistoryStore history(2);
	HistoryCoverage coverage;
	coverage.Begin();
	coverage.Add({1, 1, 7}, 1, 0, HistoryPreview);
	coverage.Apply(history);
	history.Append({{1, 1, 7}, 1, 0, 1, 16});
	coverage.Begin();
	coverage.Add({1, 1, 7}, 1, 0, HistoryPreview);
	coverage.Apply(history);
	Require(history.At(0).flags == HistoryPreview, "Coverage did not reach a later history page");
	history.Append({{1, 1, 7}, 2, 0, 2, 16});
	coverage.Begin();
	coverage.Add({1, 1, 7}, 2, 42, HistorySnapshot);
	coverage.Apply(history);
	Require(history.At(0).flags == 0 && history.At(1).flags == HistorySnapshot && history.At(1).capture == 42,
			"Reconnect coverage crossed epochs");
	history.Append({{1, 1, 8}, 2, 0, 3, 16, HistoryDriven});
	history.Append({{1, 1, 9}, 2, 0, 4, 16, HistoryMarker});
	coverage.Begin();
	coverage.Apply(history);
	Require(history.At(0).flags == HistoryDriven && history.At(1).flags == HistoryMarker,
			"Evicted coverage changed a reused ring slot");
	history.Clear();
	history.Append({{2, 1, 1}, 3, 0, 1, 16});
	coverage.Begin();
	coverage.Add({2, 1, 1}, 3, 0, HistoryPreview);
	coverage.Apply(history);
	history.Append({{2, 1, 1}, 3, 0, 2, 16, HistoryRunStart});
	coverage.Begin();
	coverage.Add({2, 1, 1}, 3, 0, HistoryPreview);
	coverage.Apply(history);
	Require(history.At(1).flags == (HistoryRunStart | HistoryPreview),
			"Revisited identity did not update the newest segment");
	history.Clear();
	history.Append({{2, 1, 1}, 3, 0, 1, 16});
	coverage.Begin();
	coverage.Add({2, 1, 1}, 3, 0, HistoryPreview);
	coverage.Apply(history);
	Require(history.At(0).flags == HistoryPreview, "Clear reused a previous coverage application");
}
} // namespace simxr::test

int main()
{
	try
	{
		simxr::test::OverlappingSources();
		simxr::test::ScopeAndRingChanges();
		simxr::test::UnsortedSources();
		std::cout << "PASS overlapping coverage, release, reconnect, late pages and ring reuse\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
