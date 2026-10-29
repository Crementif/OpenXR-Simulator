#include "graphics/Swapchain.h"
#include "../TestSupport.h"

#include <iostream>

int main()
{
	simxr::Swapchain chain(3);
	std::uint32_t first, second, third, released;
	Require(chain.Wait(0, true) == XR_ERROR_CALL_ORDER_INVALID, "Wait without acquire");
	Require(chain.Release(released) == XR_ERROR_CALL_ORDER_INVALID, "Release without acquire");
	Require(chain.Acquire(first) == XR_SUCCESS, "Acquire first");
	Require(chain.Acquire(second) == XR_SUCCESS, "Acquire second");
	Require(first != second, "Duplicate acquired image");
	Require(chain.Wait(0, false) == XR_TIMEOUT_EXPIRED, "Poll readiness");
	Require(chain.PendingImage() == first, "Timeout changed FIFO");
	Require(chain.Wait(7, false) == XR_TIMEOUT_EXPIRED, "Finite timeout");
	Require(chain.Wait(-1, true) == XR_ERROR_VALIDATION_FAILURE, "Negative timeout");
	Require(chain.Wait(XR_INFINITE_DURATION, true) == XR_SUCCESS, "Infinite timeout sentinel");
	Require(chain.Wait(0, true) == XR_ERROR_CALL_ORDER_INVALID, "Wait advanced before release");
	Require(chain.Acquire(third) == XR_SUCCESS, "Acquire while first waited");
	Require(chain.Acquire(released) == XR_ERROR_CALL_ORDER_INVALID, "Exhausted acquisition");
	Require(chain.Release(released) == XR_SUCCESS && released == first, "Release first FIFO image");
	Require(chain.ReleasedImage() == first, "Released identity");
	Require(chain.Wait(0, true) == XR_SUCCESS, "Wait second");
	Require(chain.Release(released) == XR_SUCCESS && released == second, "Release second");
	Require(chain.Wait(0, true) == XR_SUCCESS, "Wait third");
	Require(chain.Release(released) == XR_SUCCESS && released == third, "Release third");
	Require(chain.Release(released) == XR_ERROR_CALL_ORDER_INVALID, "Double release");
	for (int cycle = 0; cycle < 50; ++cycle)
	{
		Require(chain.Acquire(first) == XR_SUCCESS, "Reuse acquire");
		Require(chain.Wait(0, true) == XR_SUCCESS, "Reuse wait");
		Require(chain.Release(released) == XR_SUCCESS && released == first, "Reuse release");
	}
	simxr::Swapchain still(1, true);
	Require(still.Acquire(first) == XR_SUCCESS, "Static acquire");
	Require(still.Wait(0, true) == XR_SUCCESS, "Static wait");
	Require(still.Release(released) == XR_SUCCESS, "Static release");
	Require(still.Acquire(first) == XR_ERROR_CALL_ORDER_INVALID, "Static image acquired twice");
	simxr::Swapchain transactional(2);
	std::uint32_t untouched = 999;
	Require(transactional.Acquire(untouched, [](auto) { return XR_ERROR_RUNTIME_FAILURE; }) ==
					XR_ERROR_RUNTIME_FAILURE &&
				untouched == 999 && !transactional.PendingImage(),
			"Failed initialization consumed acquisition");
	Require(transactional.Acquire(first) == XR_SUCCESS && first == 0, "Initialization retry advanced image identity");
	Require(transactional.Acquire(untouched, [](auto) { return XR_ERROR_OUT_OF_MEMORY; }) == XR_ERROR_OUT_OF_MEMORY &&
				transactional.PendingImage() == first && untouched == 999,
			"Failed second initialization changed oldest acquired image");
	Require(transactional.Wait(0, true) == XR_SUCCESS && transactional.Release(released) == XR_SUCCESS &&
				released == first,
			"Initialization failure damaged existing FIFO");
	std::cout << "Swapchain tests passed\n";
}
