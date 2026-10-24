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
	Require(history.Frames({1}).size() == 512 && history.Find({{1}, {489}}).output,
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
		std::cout << "PASS consecutive previews, oldest eviction, independent readers, clear and run identity\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
