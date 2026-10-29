#include "../TestSupport.h"
#include "graphics/SwapchainAccess.h"
#include "graphics/SwapchainWaits.h"

#include <atomic>
#include <future>
#include <iostream>

namespace
{
using namespace simxr;

class Completion final : public GpuCompletion
{
public:
	std::atomic<CompletionStatus> status{CompletionStatus::Pending};
	mutable std::atomic<unsigned> polls{0};
	std::function<void()> observe;
	std::function<void()> retired;

	~Completion() override
	{
		if (retired)
			retired();
	}

	CompletionStatus PollCompletion() const noexcept override
	{
		++polls;
		if (observe)
			observe();
		return status.load();
	}
};

void FifoInitializationAndReuse()
{
	SwapchainAccess access(1);
	auto initialized = std::make_shared<Completion>();
	std::uint32_t index = 99;
	Require(access.Acquire(index, [&](auto) { return initialized; }) == XR_SUCCESS && index == 0,
			"Initialization publishes one acquired image");
	Require(access.Wait(0) == XR_TIMEOUT_EXPIRED && access.Release() == XR_ERROR_CALL_ORDER_INVALID,
			"Pending initialization does not make an image writable or releasable");
	initialized->status = CompletionStatus::Complete;
	Require(access.Wait(0) == XR_SUCCESS && access.Release() == XR_SUCCESS && access.ReleasedImage() == 0,
			"Actual initialization completion permits FIFO release");
	auto render = std::make_shared<Completion>();
	access.MarkReleasedUsed(render);
	access.MarkReleasedUsed({});
	Require(access.Acquire(index) == XR_SUCCESS && access.Wait(0) == XR_TIMEOUT_EXPIRED,
			"Reuse retains the compositor's last-use dependency");
	render->status = CompletionStatus::Complete;
	Require(access.Wait(XR_INFINITE_DURATION) == XR_SUCCESS && access.Release() == XR_SUCCESS,
			"Actual compositor completion permits application reuse");
	Require(access.Wait(0) == XR_ERROR_CALL_ORDER_INVALID, "Wait without an acquisition is rejected");

	SwapchainAccess rollback(2);
	index = 99;
	bool failed = false;
	try
	{
		rollback.Acquire(index, [](auto) -> std::shared_ptr<const GpuCompletion> {
			throw std::runtime_error("initialization failed");
		});
	}
	catch (const std::runtime_error&)
	{
		failed = true;
	}
	Require(failed && index == 99 && rollback.Acquire(index) == XR_SUCCESS && index == 0,
			"Failed native preparation preserves FIFO identity and caller output");
}

void ReplacedCompletionCannotAuthorizeReuse()
{
	SwapchainAccess access(1);
	auto previous = std::make_shared<Completion>();
	auto replacement = std::make_shared<Completion>();
	previous->status = CompletionStatus::Complete;
	previous->observe = [&] {
		access.MarkUsed(0, replacement);
	};
	std::uint32_t index;
	Require(access.Acquire(index, [&](auto) { return previous; }) == XR_SUCCESS, "Acquire replacement fixture");
	Require(access.Wait(0) == XR_TIMEOUT_EXPIRED, "An obsolete completion cannot authorize access after the deadline");
	replacement->status = CompletionStatus::Complete;
	Require(access.Wait(0) == XR_SUCCESS && access.Release() == XR_SUCCESS,
			"Only the current completion authorizes reuse");
}

void ReplacedCompletionRespectsTimeout()
{
	bool bounded = true;
	for (const XrDuration timeout : {XrDuration{0}, XrDuration{1000000}})
	{
		SwapchainAccess access(1);
		auto first = std::make_shared<Completion>();
		auto second = std::make_shared<Completion>();
		first->status = CompletionStatus::Complete;
		second->status = CompletionStatus::Complete;
		first->observe = [&] {
			access.MarkUsed(0, second);
		};
		second->observe = [&] {
			access.MarkUsed(0, first);
		};
		std::uint32_t index;
		Require(access.Acquire(index, [&](auto) { return first; }) == XR_SUCCESS,
				"Acquire repeatedly replaced completion fixture");
		auto waiting = std::async(std::launch::async, [&] { return access.Wait(timeout); });
		const bool finished = waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
		if (!finished)
			access.Close();
		const auto result = waiting.get();
		bounded &= finished && result == XR_TIMEOUT_EXPIRED;
		std::cout << "Replacement timeout " << timeout << ": bounded=" << finished << ", result=" << result << '\n';
		if (finished)
		{
			Require(access.Release() == XR_ERROR_CALL_ORDER_INVALID,
					"Timeout authorized release of an image with an unobserved dependency");
			first->observe = second->observe = {};
			Require(access.Wait(0) == XR_SUCCESS && access.Release() == XR_SUCCESS,
					"Timed-out wait could not retry after the dependency settled");
		}
	}
	Require(bounded, "Completion replacement bypassed the swapchain wait timeout");
}

void CancellationDrainsBlockingReaders()
{
	SwapchainAccess access(1);
	SwapchainWaits readers;
	auto completion = std::make_shared<Completion>();
	std::uint32_t index;
	Require(access.Acquire(index, [&](auto) { return completion; }) == XR_SUCCESS, "Acquire cancellation fixture");
	auto waiting = std::async(std::launch::async, [&] {
		auto registered = readers.Enter();
		if (!registered)
			return XR_ERROR_SESSION_LOST;
		return access.Wait(XR_INFINITE_DURATION);
	});
	Eventually([&] { return completion->polls != 0; }, "Blocking image wait did not begin");
	Require(!readers.Drain(std::chrono::nanoseconds::zero()), "Native teardown cannot pass a live completion reader");
	Require(access.Wait(0) == XR_ERROR_CALL_ORDER_INVALID, "Concurrent wait cannot consume the same FIFO entry");
	readers.Close();
	Require(!readers.Enter(), "Session closure revokes new blocking readers");
	access.Close();
	Require(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
				waiting.get() == XR_ERROR_SESSION_LOST,
			"Closure wakes a pending image wait without fabricating native completion");
	Require(readers.Drain(std::chrono::seconds(2)) && completion->status == CompletionStatus::Pending,
			"Reader drain observes CPU departure, independently of GPU completion");
	Require(access.Acquire(index) == XR_ERROR_SESSION_LOST && access.Release() == XR_ERROR_SESSION_LOST &&
				!access.ReleasedImage(),
			"Closed swapchain rejects access and released-source publication");
}

void CompletionCallbacksDoNotHoldOwnerLock()
{
	SwapchainAccess access(1);
	auto completion = std::make_shared<Completion>();
	completion->status = CompletionStatus::Complete;
	completion->observe = [&] {
		completion.reset();
	};
	bool retired = false;
	completion->retired = [&] {
		access.Close();
		retired = true;
	};
	std::uint32_t index;
	Require(access.Acquire(index, [&](auto) { return completion; }) == XR_SUCCESS, "Acquire retiring-reader fixture");
	Require(access.Wait(0) == XR_SUCCESS && retired, "Last completion reader retires outside the swapchain mutex");
	Require(access.Release() == XR_ERROR_SESSION_LOST, "Completion destructor can reenter closure");

	SwapchainAccess cancelled(1);
	auto reentrant = std::make_shared<Completion>();
	reentrant->observe = [&] {
		cancelled.Close();
	};
	Require(cancelled.Acquire(index, [&](auto) { return reentrant; }) == XR_SUCCESS &&
				cancelled.Wait(0) == XR_ERROR_SESSION_LOST,
			"Completion observation itself can reenter closure");
}

void FailureAndStaticImages()
{
	SwapchainAccess access(1, true);
	auto failed = std::make_shared<Completion>();
	failed->status = CompletionStatus::Failed;
	std::uint32_t index;
	Require(access.Acquire(index, [&](auto) { return failed; }) == XR_SUCCESS &&
				access.Wait(0) == XR_ERROR_RUNTIME_FAILURE && access.Release() == XR_ERROR_CALL_ORDER_INVALID,
			"Failure does not authorize image access");
	Require(access.Acquire(index) == XR_ERROR_CALL_ORDER_INVALID, "Static image cannot be acquired twice");
	SwapchainWaits readers;
	auto first = readers.Enter();
	auto moved = std::move(first);
	first.reset();
	Require(!readers.Drain(std::chrono::nanoseconds::zero()), "Moving registration retains its active charge");
	moved.reset();
	Require(readers.Drain(std::chrono::nanoseconds::zero()), "Registration retires exactly once after moving");
}
} // namespace

int main()
{
	try
	{
		FifoInitializationAndReuse();
		ReplacedCompletionCannotAuthorizeReuse();
		ReplacedCompletionRespectsTimeout();
		CancellationDrainsBlockingReaders();
		CompletionCallbacksDoNotHoldOwnerLock();
		FailureAndStaticImages();
		std::cout << "Swapchain access, source reuse, cancellation and reader retirement checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
