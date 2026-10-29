#include "VulkanAdmissionChecks.h"
#include "Pch.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <future>
#include <mutex>
#include <vulkan/vulkan.h>

namespace simxr::probe::admission
{
static std::atomic<bool> s_armed{false};
static std::mutex s_mutex;
static std::condition_variable s_changed;
static bool s_entered = false, s_released = false;

static void HoldAllocation()
{
	if (!s_armed.exchange(false))
		return;
	std::unique_lock lock(s_mutex);
	s_entered = true;
	s_changed.notify_all();
	s_changed.wait(lock, [] { return s_released; });
}

static void Release()
{
	{
		std::lock_guard lock(s_mutex);
		s_released = true;
	}
	s_changed.notify_all();
}
} // namespace simxr::probe::admission

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
														   const VkAllocationCallbacks* allocator,
														   VkDeviceMemory* memory)
{
	static const auto allocate = reinterpret_cast<PFN_vkAllocateMemory>(dlsym(RTLD_NEXT, "vkAllocateMemory"));
	if (!allocate)
		std::abort();
	simxr::probe::admission::HoldAllocation();
	return allocate(device, info, allocator, memory);
}

namespace simxr::probe
{
XrResult CheckNativeAdmission(XrInstance instance, XrSession session, const XrFrameEndInfo& frame)
{
	using namespace admission;
	std::future<XrResult> ended;
	std::future<std::pair<XrResult, double>> queried;

	struct Unblock
	{
		~Unblock()
		{
			Release();
		}
	} unblock;

	{
		std::lock_guard lock(s_mutex);
		s_entered = s_released = false;
		s_armed.store(true);
	}
	ended = std::async(std::launch::async, [&] { return xrEndFrame(session, &frame); });
	bool entered;
	{
		std::unique_lock lock(s_mutex);
		entered = s_changed.wait_for(lock, std::chrono::seconds(5), [] { return s_entered; });
	}
	bool responsive = false;
	if (entered)
	{
		queried = std::async(std::launch::async, [instance] {
			XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
			const auto start = std::chrono::steady_clock::now();
			const auto result = xrGetInstanceProperties(instance, &properties);
			return std::pair{
				result, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count()};
		});
		responsive = queried.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	}
	Release();
	s_armed.store(false);
	const auto result = ended.get();
	const auto query = queried.valid() ? queried.get() : std::pair{XR_ERROR_RUNTIME_FAILURE, 0.0};
	if (!entered || !responsive || query.first != XR_SUCCESS)
	{
		std::fprintf(stderr,
					 "FAIL: Native frame allocation blocked unrelated API admission (entered=%d, query_us=%.3f)\n",
					 entered, query.second);
		return XR_ERROR_RUNTIME_FAILURE;
	}
	std::printf("case=native-admission allocatorGated=yes query_us=%.3f independentQuery=ready PASS\n", query.second);
	return result;
}
} // namespace simxr::probe
