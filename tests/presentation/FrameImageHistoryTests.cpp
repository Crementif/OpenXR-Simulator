#include "../TestSupport.h"
#include "graphics/FrameOutput.h"
#include "presentation/FrameImageHistory.h"

#include <functional>
#include <iostream>

namespace
{
using namespace simxr;
class Output final : public FrameOutput
{
  public:
	std::function<void()> destroyed;
	~Output() override
	{
		if (destroyed)
			destroyed();
	}
	CompletionStatus PollCompletion() const noexcept override { return CompletionStatus::Complete; }
};
HistoricalImage Image(std::uint64_t frame, const std::shared_ptr<const FrameOutput>& output, bool clear = false)
{
	auto record = std::make_shared<FrameRecord>();
	record->key = {{1}, {frame}};
	record->stage = FrameStage::Submitted;
	record->submission.emplace();
	record->submission->layerCount = clear ? 0 : 1;
	return {record, output};
}
void BoundedOwnership()
{
	FrameImageHistory history;
	history.Start({1});
	std::size_t retiredCount = 0;
	std::shared_ptr<const FrameOutput> independent;
	for (std::uint64_t frame = 1; frame <= 12; ++frame)
	{
		auto output = std::make_shared<Output>();
		output->destroyed = [&]
		{
			Require(history.Snapshot().entries.size() <= 10, "Native retirement reentered corrupt history");
			++retiredCount;
		};
		if (frame == 1)
			independent = output;
		auto retired = history.Append(Image(frame, output));
	}
	auto snapshot = history.Snapshot(3);
	Require(snapshot.entries.size() == 3 && snapshot.entries.front().metadata->key.frame.value == 10 &&
		snapshot.entries.back().metadata->key.frame.value == 12 && snapshot.evicted == 2,
		"History lost its bound or chronological order");
	Require(!history.Find({{1}, {1}}).metadata && history.Find({{1}, {3}}).output,
		"Evicted history remained discoverable or current image disappeared");
	Require(retiredCount == 1, "Eviction destroyed an independently retained image");
	independent.reset();
	Require(retiredCount == 2, "Independent image ownership leaked");
	snapshot = {};
	{
		auto retired = history.Start({2});
		Require(history.Snapshot().entries.empty() && !history.Find({{1}, {3}}).metadata,
			"Restart redirected old identities to the new run");
		Require(retiredCount == 2, "Restart destroyed native owners before caller locks could release");
	}
	Require(retiredCount == 12, "Restart failed to release detached native owners");
}
void MissingImagesAndDestruction()
{
	auto history = std::make_unique<FrameImageHistory>();
	history->Start({1});
	history->Append(Image(1, {}));
	history->Append(Image(2, {}, true));
	Require(history->Snapshot().unavailable == 1, "Clear was counted as a missing image");
	bool reentered = false;
	auto output = std::make_shared<Output>();
	output->destroyed = [raw = history.get(), &reentered]
	{
		Require(raw->Snapshot().entries.empty(), "History destructor exposed partly destroyed native readers");
		reentered = true;
	};
	history->Append(Image(3, output));
	output.reset();
	history.reset();
	Require(reentered, "History destruction leaked its final image");
}
} // namespace
int main()
{
	try
	{
		BoundedOwnership();
		MissingImagesAndDestruction();
		std::cout << "Bounded image history, restart, independent readers and retirement reentry passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
