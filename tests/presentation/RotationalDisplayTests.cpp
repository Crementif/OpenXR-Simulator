#include "../TestSupport.h"
#include "Math.h"
#include "presentation/VirtualDisplay.h"

#include <atomic>
#include <future>
#include <iostream>
#include <limits>

using namespace simxr;
using namespace std::chrono_literals;

namespace
{
class Output final : public FrameOutput
{
  public:
	CompletionStatus PollCompletion() const noexcept override
	{
		return state.load();
	}
	std::atomic<CompletionStatus> state{CompletionStatus::Pending};
};
std::shared_ptr<Output> Complete()
{
	auto result = std::make_shared<Output>();
	result->state = CompletionStatus::Complete;
	return result;
}
PoseSample Head(float yaw = 0, std::int64_t time = 100)
{
	return {{EulerAngles(yaw, 0, 0), {1, 2, 3}}, TrackedPoseFlags, {Duration{time}}};
}
class Source final : public RotationalSource
{
  public:
	PoseSample SubmittedHead() const noexcept override
	{
		return Head();
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	XrResult Reproject(const PoseSample& head, std::shared_ptr<const FrameOutput>& output) const override
	{
		++calls;
		if (observe)
			observe();
		if (allocationFailure)
			throw std::bad_alloc{};
		if (result != XR_SUCCESS)
			return result;
		auto image = std::make_shared<Output>();
		latest = image;
		requested = head;
		output = std::move(image);
		return XR_SUCCESS;
	}
	mutable unsigned calls = 0;
	mutable std::weak_ptr<Output> latest;
	mutable PoseSample requested;
	XrResult result = XR_SUCCESS;
	bool allocationFailure = false;
	std::function<void()> observe;
};
void PendingCompletionAndReplacement()
{
	VirtualDisplay display;
	auto head = Head();
	bool samplingAllocationFailure = false;
	display.ConfigurePolicy(PresentationPolicy::Rotational,
							[&](RuntimeTime)
							{
								if (samplingAllocationFailure)
									throw std::bad_alloc{};
								return head;
							});
	display.Start({1}, {{Duration{100}}, 10ns}, ExecutionMode::Deterministic);
	Require(display.Pause(), "Pause deterministic policy fixture");
	auto baseline = Complete();
	auto source = std::make_shared<Source>();
	source->observe = [&]
	{
		display.Snapshot();
	};
	Require(display.Submit({{{1}, {1}}, {Duration{100}}, baseline, {}, {source}}) == PresentationAdmission::Accepted,
			"Submit retained source");
	auto tick = display.Step();
	Require(tick && tick->output == baseline && !tick->reprojected && source->calls == 0 &&
				tick->imageTick == DisplayTick{0},
			"Unchanged orientation submitted unnecessary GPU work");
	head = Head(0.25f, 110);
	head.pose.position = {99, 88, 77};
	tick = display.Step();
	Require(tick && tick->output == baseline && source->calls == 1 && !tick->reprojected &&
				tick->reprojectionResult == XR_TIMEOUT_EXPIRED && tick->imageTick == DisplayTick{0},
			"Pending GPU result replaced the completed baseline");
	auto pending = source->latest.lock();
	Require(pending && source->requested.pose.position.x == 1, "Rotational policy applied head translation");
	display.Step();
	Require(source->calls == 1, "Pending reprojection admitted another GPU job");
	pending->state = CompletionStatus::Complete;
	tick = display.Step();
	Require(tick && tick->output == pending && tick->reprojected && tick->source == FrameKey{{1}, {1}} &&
				tick->imageTick == DisplayTick{3} && tick->displayHead->sampleTime == head.sampleTime &&
				tick->displayHead->pose.position.x == 1 && display.Snapshot().latest->output == pending,
			"Completed warp was not atomically published with its actual pose and source");
	const auto retained = *tick;
	tick = display.Step();
	Require(tick && tick->output == pending && tick->imageTick == retained.imageTick && source->calls == 1,
			"Unchanged warped image was recomposed or assigned another identity");
	samplingAllocationFailure = true;
	tick = display.Step();
	Require(tick && tick->output == pending && tick->reprojectionResult == XR_ERROR_OUT_OF_MEMORY &&
				!display.Faulted() && source->calls == 1,
			"Tracking allocation pressure lost completed display output");
	samplingAllocationFailure = false;
	head = Head(0.5f, 150);
	tick = display.Step();
	Require(tick && tick->output == pending && tick->reprojected && source->calls == 2,
			"New pending warp failed to repeat the completed warp");
	auto failed = source->latest.lock();
	failed->state = CompletionStatus::Failed;
	tick = display.Step();
	Require(tick && tick->output == pending && tick->reprojectionResult == XR_ERROR_RUNTIME_FAILURE,
			"Failed GPU result replaced good pixels or hid its failure");
	display.Step();
	Require(source->calls == 2, "Failed GPU source kept submitting work");
	auto newer = Complete();
	auto newerSource = std::make_shared<Source>();
	Require(display.Submit({{{1}, {2}}, {Duration{180}}, newer, {}, {newerSource}}) == PresentationAdmission::Accepted,
			"Replace failed retained source");
	tick = display.Step();
	Require(tick && tick->output == newer && !tick->reprojected && tick->source == FrameKey{{1}, {2}} &&
				newerSource->calls == 1,
			"New source reused an old warp or inherited a latched failure");
	display.Stop();
	Require(newerSource->latest.expired() && !display.Snapshot().latest->output && retained.output == pending,
			"Stop retained policy work or destroyed an independent displayed-image reader");
}
void FallbackAndPolicySelection()
{
	VirtualDisplay display;
	auto head = Head(0.25f, 110);
	unsigned samples = 0;
	display.ConfigurePolicy(PresentationPolicy::Repeat,
							[&](RuntimeTime)
							{
								++samples;
								return head;
							});
	display.Start({1}, {{Duration{100}}, 10ns}, ExecutionMode::Deterministic);
	display.Pause();
	auto baseline = Complete();
	auto source = std::make_shared<Source>();
	display.Submit({{{1}, {1}}, {Duration{100}}, baseline, {}, {source}});
	Require(display.Step()->output == baseline && source->calls == 0 && samples == 0,
			"Repeat policy sampled tracking or submitted reprojection");
	bool rejected = false;
	try
	{
		display.ConfigurePolicy(PresentationPolicy::Rotational,
								[&](RuntimeTime)
								{
									return head;
								});
	}
	catch (const std::logic_error&)
	{
		rejected = true;
	}
	Require(rejected, "Active policy changed without a lifecycle boundary");
	display.Stop();
	display.ConfigurePolicy(PresentationPolicy::Rotational,
							[&](RuntimeTime)
							{
								++samples;
								return head;
							});
	display.Start({2}, {{Duration{100}}, 10ns}, ExecutionMode::Deterministic);
	display.Pause();
	display.Submit({{{2}, {1}}, {Duration{100}}, baseline, {}, {{}, XR_ERROR_LIMIT_REACHED}});
	auto tick = display.Step();
	Require(tick->output == baseline && tick->reprojectionResult == XR_ERROR_LIMIT_REACHED && samples == 0,
			"Unavailable retention lost its baseline or failure reason");
	display.Submit({{{2}, {2}}, {Duration{110}}, baseline, {}, {source}});
	head = Head(0.25f, 99);
	Require(display.Step()->reprojectionResult == XR_TIMEOUT_EXPIRED && source->calls == 0,
			"Older tracking was used for late reprojection");
	head = Head(0.25f, 120);
	head.flags = 0;
	Require(display.Step()->reprojectionResult == XR_ERROR_POSE_INVALID && source->calls == 0,
			"Invalid tracking submitted a warp");
	head = Head(0.25f, 130);
	source->result = XR_ERROR_LIMIT_REACHED;
	Require(display.Step()->reprojectionResult == XR_ERROR_LIMIT_REACHED, "Output pressure was hidden");
	source->allocationFailure = true;
	Require(display.Step()->reprojectionResult == XR_ERROR_OUT_OF_MEMORY, "Allocation failure faulted the display");
	source->allocationFailure = false;
	source->result = XR_SUCCESS;
	display.Step();
	auto pending = source->latest.lock();
	Require(pending && source->calls == 3, "Transient pressure did not recover");
	display.Submit({{{2}, {3}}, {Duration{160}}, {}});
	tick = display.Step();
	Require(tick && !tick->output && !tick->imageTick && tick->kind == PresentationKind::Clear,
			"Clear retained a pending warped image");
	pending->state = CompletionStatus::Complete;
	Require(!display.Step()->output, "Late completion replaced a submitted clear");
}
class TestClock final : public Clock
{
  public:
	HostTime Now() const override
	{
		return HostTime{Duration{time.load()}};
	}
	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime) override
	{
		changed.wait_for(lock, 1ms);
	}
	std::atomic<std::int64_t> time{0};
};
void StopInvalidatesUnpublishedWork()
{
	auto clock = std::make_shared<TestClock>();
	VirtualDisplay display(clock);
	std::atomic<bool> entered{false}, released{false};
	std::future<void> stopped;
	struct Cleanup
	{
		VirtualDisplay& display;
		std::atomic<bool>& released;
		~Cleanup()
		{
			released = true;
			display.Stop();
		}
	} cleanup{display, released};
	auto source = std::make_shared<Source>();
	source->observe = [&]
	{
		entered = true;
		while (!released.load())
			std::this_thread::yield();
	};
	display.ConfigurePolicy(PresentationPolicy::Rotational,
							[](RuntimeTime time)
							{
								return Head(0.25f, time.value.count());
							});
	display.Start({1}, {{Duration{100}}, 10ns}, ExecutionMode::RealTime, HostTime{});
	Eventually(
		[&]
		{
			return display.Snapshot().latest.has_value();
		},
		"Real-time worker did not start");
	auto baseline = Complete();
	display.Submit({{{1}, {1}}, {Duration{110}}, baseline, {}, {source}});
	clock->time = 10;
	Eventually(
		[&]
		{
			return entered.load();
		},
		"Worker did not prepare a warp independently of XR submission");
	const auto before = display.Snapshot();
	Require(before.latest->tick == DisplayTick{0} && !before.latest->output,
			"Snapshot observed a partially published display tick");
	stopped = std::async(std::launch::async,
						 [&]
						 {
							 display.Stop();
						 });
	Eventually(
		[&]
		{
			return !display.Snapshot().running;
		},
		"Stop waited for GPU preparation before closing admission");
	released = true;
	Require(stopped.wait_for(1s) == std::future_status::ready, "Stop could not join display preparation");
	stopped.get();
	Require(!display.Snapshot().latest->output && display.Snapshot().latest->tick == DisplayTick{0} &&
				source->latest.expired(),
			"Stopped generation published or retained a stale prepared result");
	display.Start({2}, {{Duration{200}}, 10ns}, ExecutionMode::Deterministic);
	display.Pause();
	Require(!display.Step()->output, "Restart inherited an old display image");
}
} // namespace
int main()
{
	PendingCompletionAndReplacement();
	FallbackAndPolicySelection();
	StopInvalidatesUnpublishedWork();
	std::cout << "PASS rotational display publication, fallback, pressure and teardown; CPU completion fixtures\n";
}
