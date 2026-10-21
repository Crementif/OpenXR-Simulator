#include "../TestSupport.h"
#include "platform/linux/ControlFd.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <unistd.h>

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
thread_local bool failAllocation = false;
}
void* operator new(std::size_t size)
{
	if (failAllocation)
	{
		failAllocation = false;
		throw std::bad_alloc{};
	}
	if (void* value = std::malloc(size ? size : 1))
		return value;
	throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
	return ::operator new(size);
}
[[gnu::noinline]] void operator delete(void* value) noexcept
{
	std::free(value);
}
[[gnu::noinline]] void operator delete[](void* value) noexcept
{
	std::free(value);
}
[[gnu::noinline]] void operator delete(void* value, std::size_t) noexcept
{
	std::free(value);
}
[[gnu::noinline]] void operator delete[](void* value, std::size_t) noexcept
{
	std::free(value);
}
#endif

int main()
{
	using namespace simxr::control;
	try
	{
		int pipe[2];
		Require(::pipe(pipe) == 0, "Create FD ownership fixture");
		auto original = OwnControlFd(pipe[0]);
		Require(original->Kind() == TransferHandleKind::OpaqueFd && original->Value() == pipe[0] &&
					(fcntl(pipe[0], F_GETFD) & FD_CLOEXEC),
				"Owned control FD did not retain identity/CLOEXEC");
		auto reader = original;
		auto duplicate = original->Duplicate();
		const auto cloned = static_cast<int>(duplicate->Value());
		Require(cloned != pipe[0] && (fcntl(cloned, F_GETFD) & FD_CLOEXEC),
				"Control FD clone reused descriptor or lost CLOEXEC");
		original.reset();
		Require(fcntl(pipe[0], F_GETFD) >= 0, "Copied handle lost shared native ownership");
		reader.reset();
		Require(fcntl(pipe[0], F_GETFD) < 0 && errno == EBADF && fcntl(cloned, F_GETFD) >= 0,
				"Last reader did not close exactly its FD");
		duplicate.reset();
		Require(fcntl(cloned, F_GETFD) < 0 && errno == EBADF, "Duplicate reader leaked native FD");
		Require(fcntl(pipe[1], F_SETFD, 0) == 0, "Clear caller inheritance fixture");
		auto callerClone = DuplicateControlFd(pipe[1]);
		Require(fcntl(pipe[1], F_GETFD) == 0 && (fcntl(static_cast<int>(callerClone->Value()), F_GETFD) & FD_CLOEXEC),
				"Duplicate mutated/consumed caller ownership");
		callerClone.reset();
		Require(fcntl(pipe[1], F_GETFD) >= 0, "Duplicate consumed caller FD");
		close(pipe[1]);
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
		Require(::pipe(pipe) == 0, "Create failed-adoption fixture");
		bool rejected = false;
		failAllocation = true;
		try
		{
			OwnControlFd(pipe[0]);
		}
		catch (const std::bad_alloc&)
		{
			rejected = true;
		}
		failAllocation = false;
		Require(rejected && fcntl(pipe[0], F_GETFD) < 0 && errno == EBADF,
				"Failed ownership allocation leaked adopted FD");
		close(pipe[1]);
#endif
		std::cout << "Control FD ownership, duplicate/CLOEXEC, last-reader and allocation rollback passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
