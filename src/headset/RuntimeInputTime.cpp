#include "Pch.h"
#include "headset/HeadsetActions.h"
#include "headset/RuntimeClock.h"

namespace simxr::headset
{
HeadsetNativeTimes RuntimeInputTimes(const OpenXrRuntime& runtime, const HeadsetInputSample& input)
{
	HeadsetNativeTimes result;
	result.predicted = RuntimeTimeToHostNanoseconds(runtime, input.time);
	if (input.body)
	{
		if (input.body->requestedTime != input.time)
			throw std::invalid_argument("Body request time differs from its tracking batch");
		if (const auto time = input.body->returnedTime)
		{
			if (*time < 0 || (*time == 0 && input.body->active == true))
				throw std::invalid_argument("Invalid provider body sample time");
			result.bodyReturned = *time == 0 ? 0 : RuntimeTimeToHostNanoseconds(runtime, *time);
		}
	}
	return result;
}
} // namespace simxr::headset
