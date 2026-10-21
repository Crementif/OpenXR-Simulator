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
void WindowOwnership()
{
	FrameImageHistory history;
	history.Start({1});
	for (std::uint64_t frame = 1; frame <= 10; ++frame)
		history.Append(Image(frame, std::make_shared<Output>()));
	auto started = history.BeginWindow();
	auto window = std::move(started.window);
	Require(window && !history.BeginWindow().window, "Concurrent collecting window was admitted");
	for (std::uint64_t frame = 11; frame <= 40; ++frame)
		history.Append(Image(frame, frame == 15 ? nullptr : std::make_shared<Output>()));
	const auto snapshot = window->Snapshot();
	Require(snapshot.trigger.value == 10 && snapshot.preCount == 10 && snapshot.postCount == 12 &&
		!snapshot.interrupted && snapshot.entries.size() == 22, "Window changed trigger or collection bounds");
	for (std::size_t index = 0; index < snapshot.entries.size(); ++index)
		Require(snapshot.entries[index].metadata->key.frame.value == index + 1, "Window skipped or substituted a frame");
	Require(window->Find({15}).metadata && !window->Find({15}).output && !window->Find({23}).metadata,
		"Missing window pixels or following frames were misrepresented");
	auto next = history.BeginWindow().window;
	Require(next && next->Snapshot().trigger.value == 40, "Completed window prevented subsequent capture");
	history.Append(Image(41, std::make_shared<Output>()));
	history.Start({2});
	Require(next->Snapshot().interrupted && next->Snapshot().postCount == 1,
		"Generation restart did not interrupt the collecting window");
	Require(window->Find({1}).output && !window->Snapshot().interrupted,
		"Restart invalidated a completed independent window");
}
void IndependentWindowTargets()
{
	for (const auto view : {ImageHistoryView::Composed, ImageHistoryView::Quads})
	{
		FrameImageHistory history;
		history.Start({1});
		bool composedDestroyed = false, quadDestroyed = false;
		auto composed = std::make_shared<Output>();
		auto quad = std::make_shared<Output>();
		composed->destroyed = [&] { composedDestroyed = true; };
		quad->destroyed = [&] { quadDestroyed = true; };
		auto image = Image(1, composed);
		image.quadOutput = quad;
		image.quadVisible = true;
		image.quadBounds[1] = {{10, 12}, {20, 16}};
		history.Append(std::move(image));
		composed.reset();
		quad.reset();
		auto started = history.BeginWindow(view);
		history.Clear();
		const bool ui = view == ImageHistoryView::Quads;
		Require(composedDestroyed == ui && quadDestroyed != ui,
			"Window retained an image from the unselected history target");
		Require(started.window->Find({1}).view == view && started.window->Find({1}).quadBounds[1].offset.x == 10,
			"Window lost its target or accepted quad geometry");
		started.window.reset();
		Require(composedDestroyed && quadDestroyed, "Target-specific window leaked an image");
	}
}
void WindowDestructionReentry()
{
	FrameImageHistory history;
	history.Start({1});
	auto output = std::make_shared<Output>();
	history.Append(Image(1, output));
	auto started = history.BeginWindow();
	auto window = std::move(started.window);
	bool reentered = false;
	output->destroyed = [&, raw = window.get()]
	{
		Require(raw->Snapshot().entries.empty() && history.Snapshot().entries.empty(),
			"Window retirement reentered partly destroyed ownership");
		reentered = true;
	};
	output.reset();
	history.Clear();
	Require(window->Snapshot().interrupted, "Clearing history did not interrupt its window");
	window.reset();
	Require(reentered, "Released window leaked its final native output");
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
		WindowOwnership();
		IndependentWindowTargets();
		WindowDestructionReentry();
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
