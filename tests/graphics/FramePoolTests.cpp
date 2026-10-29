#include "../TestSupport.h"
#include "graphics/FramePool.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SIMXR_TEST_TSAN_ALLOCATOR 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define SIMXR_TEST_TSAN_ALLOCATOR 1
#endif

#ifndef SIMXR_TEST_TSAN_ALLOCATOR
namespace
{
thread_local int failAllocationAfter = -1;

struct AllocationFailure
{
	explicit AllocationFailure(int after)
	{
		failAllocationAfter = after;
	}

	~AllocationFailure()
	{
		failAllocationAfter = -1;
	}
};
} // namespace

void* operator new(std::size_t size)
{
	if (failAllocationAfter == 0)
	{
		failAllocationAfter = -1;
		throw std::bad_alloc{};
	}
	if (failAllocationAfter > 0)
		--failAllocationAfter;
	if (void* result = std::malloc(size ? size : 1))
		return result;
	throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
	std::free(memory);
}

void operator delete[](void* memory) noexcept
{
	std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
	std::free(memory);
}

#endif // TSan owns the global allocator interceptors.

namespace
{
using namespace simxr;

void OwnershipAndReaders()
{
	bool invalid = false;
	try
	{
		FramePool zero(0);
	}
	catch (const std::invalid_argument&)
	{
		invalid = true;
	}
	Require(invalid, "Zero-sized pool accepted");
	FramePool pool(2);
	auto copy = pool; // NOLINT(performance-unnecessary-copy-initialization)
	FramePool foreign(1);
	auto first = copy.Reserve();
	auto second = pool.Reserve();
	auto other = foreign.Reserve();
	Require(first && second && pool.Capacity() == 2 && pool.InUse() == 2 && !copy.Reserve(),
			"Shared pool budget not bounded");
	Require(pool.Owns(first) && copy.Owns(first) && !foreign.Owns(first) && !pool.Owns(other) && !pool.Owns({}) &&
				!pool.Consume(other) && !pool.Consume({}),
			"Foreign/null reservation accepted");
	Require(!first->Consumed() && copy.Consume(first) && first->Consumed() && !pool.Consume(first),
			"Consumption was not exactly once");
	auto reader = first;
	first.reset();
	Require(pool.InUse() == 2 && !pool.Reserve(), "Releasing one reader freed reserved capacity");
	reader.reset();
	Require(pool.InUse() == 1, "Last reader did not release capacity");
	auto replacement = pool.Reserve();
	Require(replacement && !replacement->Consumed(), "Capacity was not reusable after retirement");
	copy.CloseAdmission();
	Require(!pool.Reserve() && !copy.Reserve() && pool.Consume(second) && pool.Consume(replacement),
			"Closure invalidated pre-existing reservations or admitted new ones");
	second.reset();
	replacement.reset();
	Require(pool.InUse() == 0, "Closed pool retained released budget");
	std::shared_ptr<FrameReservation> retained;
	{
		FramePool ephemeral(1);
		retained = ephemeral.Reserve();
	}
	Require(retained && !retained->Consumed(),
			"Reservation did not preserve shared state after pool owner destruction");
	retained.reset();
}

void AllocationRollback()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	FramePool pool(1);
	for (int failure = 0; failure < 2; ++failure)
	{
		bool failed = false;
		try
		{
			AllocationFailure allocation(failure);
			(void)pool.Reserve();
		}
		catch (const std::bad_alloc&)
		{
			failed = true;
		}
		Require(failed && pool.InUse() == 0, "Allocation failure consumed pool capacity");
		auto retry = pool.Reserve();
		Require(retry && pool.InUse() == 1, "Failed reservation could not retry");
	}
	pool.CloseAdmission();
	{
		AllocationFailure allocation(0);
		Require(!pool.Reserve(), "Closed reservation attempted allocation");
	}
#else
	std::cout << "Allocation fault injection covered by PCH/ASan; TSan owns allocator interceptors\n";
#endif
}

void CompletedOwnerReclamation()
{
	FramePool* observed = nullptr;
	std::shared_ptr<FrameReservation> retired;
	unsigned polls = 0;
	FramePool pool(1, [&] {
		++polls;
		if (observed)
			observed->InUse();
		retired.reset();
	});
	observed = &pool;
	retired = pool.Reserve();
	Require(retired && pool.InUse() == 1 && polls == 1, "Initial reserve did not collect outside its mutex");
	auto replacement = pool.Reserve();
	Require(replacement && !retired && pool.InUse() == 1 && polls == 2,
			"Reservation did not reclaim a completed producer-only owner before capacity check");
	pool.Collect();
	Require(polls == 3 && !pool.Reserve(false) && polls == 3, "Pre-collected reservation repeated reclamation");
	pool.CloseAdmission();
	pool.Collect();
	Require(!pool.Reserve() && polls == 3 && pool.Consume(replacement),
			"Closed pool invoked native reclamation or rejected its existing token");
}

void ConcurrentConsumptionAndBudget()
{
	FramePool pool(4);
	auto token = pool.Reserve();
	std::atomic<unsigned> winners = 0;
	std::vector<std::future<void>> operations;
	for (unsigned i = 0; i < 16; ++i)
		operations.push_back(std::async(std::launch::async, [pool, token, &winners] {
			if (pool.Consume(token))
				++winners;
		}));
	for (auto& operation : operations)
		operation.get();
	Require(winners == 1 && token->Consumed() && pool.InUse() == 1, "Concurrent consumption won multiple times");
	operations.clear();
	token.reset();
	std::atomic<unsigned> acquired = 0;
	for (unsigned worker = 0; worker < 8; ++worker)
		operations.push_back(std::async(std::launch::async, [pool, &acquired] {
			for (unsigned attempt = 0; attempt < 1000; ++attempt)
			{
				auto reserved = pool.Reserve();
				if (!reserved)
				{
					std::this_thread::yield();
					continue;
				}
				++acquired;
				Require(pool.InUse() <= pool.Capacity(), "Concurrent reserves exceeded pool budget");
				auto reader = reserved;
				Require(pool.Consume(reserved), "Fresh concurrent reservation was consumed");
				reserved.reset();
				std::this_thread::yield();
			}
		}));
	for (auto& operation : operations)
		operation.get();
	Require(acquired > 0 && pool.InUse() == 0, "Concurrent reservation readers leaked capacity");
	auto beforeClosure = pool.Reserve();
	operations.clear();
	for (unsigned worker = 0; worker < 8; ++worker)
		operations.push_back(std::async(std::launch::async, [pool] {
			for (unsigned i = 0; i < 100; ++i)
			{
				auto token = pool.Reserve();
				std::this_thread::yield();
			}
		}));
	pool.CloseAdmission();
	for (auto& operation : operations)
		operation.get();
	Require(!pool.Reserve() && pool.InUse() == 1 && pool.Consume(beforeClosure),
			"Concurrent closure invalidated existing reservation");
	beforeClosure.reset();
	Require(pool.InUse() == 0, "Concurrent closure leaked budget");
}
} // namespace

int main()
{
	OwnershipAndReaders();
	AllocationRollback();
	CompletedOwnerReclamation();
	ConcurrentConsumptionAndBudget();
	std::cout << "Frame reservation pool tests passed\n";
}
