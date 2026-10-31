#include "../TestSupport.h"
#include "graphics/ApplicationGpuTimer.h"
#include <vector>

namespace simxr::tests
{
class Timer final : public ApplicationGpuTimer
{
public:
	bool ready = false, fail = false;
	std::vector<std::pair<std::size_t, bool>> markers;

private:
	bool Write(std::size_t slot, bool end) override
	{
		markers.emplace_back(slot, end);
		return !fail;
	}

	bool Read(std::size_t, std::int64_t& nanoseconds) override
	{
		nanoseconds = 17;
		return ready;
	}
};

static void Ring()
{
	Timer timer;
	Require(timer.End().Load() == -1 && timer.markers.empty(), "Unbegun frame fabricated a timing");
	std::vector<GpuDuration> results;
	for (int frame = 0; frame < 16; ++frame)
	{
		timer.Begin();
		results.push_back(timer.End());
		Require(results.back().Load() == -1, "Pending result exposed a duration");
	}
	timer.Begin();
	Require(timer.End().Load() == -1 && timer.markers.size() == 32, "Full timer ring reused pending queries");
	timer.ready = true;
	timer.Collect();
	for (const auto& result : results)
		Require(result.Load() == 17, "Completed query did not reach its original frame");
	timer.ready = false;
	timer.Begin();
	timer.Begin();
	const auto restarted = timer.End();
	Require(timer.markers[32] == std::pair<std::size_t, bool>{0, false} &&
				timer.markers[33] == std::pair<std::size_t, bool>{0, true} &&
				timer.markers[34] == std::pair<std::size_t, bool>{1, false},
			"Discarded frame reused its in-flight begin marker");
	Require(results.front().Load() == 17 && restarted.Load() == -1, "Reused slot rewrote historical timing");
	timer.ready = true;
	timer.Collect();
	Require(restarted.Load() == 17, "Restarted frame never completed");
	Timer failed;
	failed.fail = true;
	failed.Begin();
	failed.fail = false;
	failed.Begin();
	Require(failed.markers[1].first == 1, "Failed native submission was reused without proof of completion");
	FrameGpuTiming first;
	const auto second = std::make_shared<FrameGpuTiming>(first);
	first.compositorNanoseconds.Store(31);
	Require(second->compositorNanoseconds.Load() == 31, "Copied history lost late compositor timing");
}
}

int main()
{
	simxr::tests::Ring();
}
