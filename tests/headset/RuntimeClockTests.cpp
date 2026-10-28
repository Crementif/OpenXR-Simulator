#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetActions.h"
#include "headset/OpenXrRuntime.h"
#include "headset/RuntimeClock.h"

#include <ctime>
#define XR_USE_TIMESPEC
#include <iostream>
#include <openxr/openxr_platform.h>

namespace simxr::headset
{
static constexpr XrTime s_vendorOffset = 12345678;
static bool s_invalid = false, s_failure = false;
static XrResult XRAPI_PTR ToHost(XrInstance, XrTime time, timespec* result)
{
	if (s_failure)
		return XR_ERROR_TIME_INVALID;
	const auto host = time - s_vendorOffset;
	result->tv_sec = host / 1000000000;
	result->tv_nsec = s_invalid ? 1000000000 : host % 1000000000;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR ToRuntime(XrInstance, const timespec* time, XrTime* result)
{
	*result = time->tv_sec * 1000000000 + time->tv_nsec + s_vendorOffset;
	return XR_SUCCESS;
}
OpenXrRuntime::~OpenXrRuntime() = default;
XrInstance OpenXrRuntime::Instance() const
{
	return reinterpret_cast<XrInstance>(1);
}
PFN_xrVoidFunction OpenXrRuntime::Procedure(const char* name) const
{
	if (std::string_view(name) == "xrConvertTimeToTimespecTimeKHR")
		return reinterpret_cast<PFN_xrVoidFunction>(ToHost);
	if (std::string_view(name) == "xrConvertTimespecTimeToTimeKHR")
		return reinterpret_cast<PFN_xrVoidFunction>(ToRuntime);
	throw std::runtime_error("Unexpected clock procedure");
}
void CheckXr(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(operation);
}
static void CheckConversion()
{
	OpenXrRuntime runtime;
	Require(RuntimeTimeToHostNanoseconds(runtime, 1000000000 + s_vendorOffset) == 1000000000,
			"Vendor clock epoch must be removed through its extension");
	Require(VerifyRuntimeClock(runtime) == 0, "Roundtrip probe exercises production conversion");
	HeadsetInputSample input;
	input.time = 1000000000 + s_vendorOffset;
	input.body.emplace();
	input.body->requestedTime = input.time;
	input.body->returnedTime = input.time - 100;
	input.body->active = true;
	const auto bodyTimes = RuntimeInputTimes(runtime, input);
	Require(bodyTimes.predicted == 1000000000 && bodyTimes.bodyReturned == 999999900,
			"Body returned time must independently cross the vendor clock epoch");
	input.body->active = false;
	input.body->returnedTime = 0;
	Require(RuntimeInputTimes(runtime, input).bodyReturned == 0, "Inactive zero time was sent to vendor conversion");
	input.body->returnedTime.reset();
	Require(!RuntimeInputTimes(runtime, input).bodyReturned, "Absent body returned time was fabricated");
	for (unsigned fault = 0; fault < 3; ++fault)
	{
		auto malformed = input;
		if (fault == 0)
			malformed.body->requestedTime++;
		if (fault == 1)
			malformed.body->returnedTime = -1;
		if (fault == 2)
		{
			malformed.body->active = true;
			malformed.body->returnedTime = 0;
		}
		bool rejected = false;
		try
		{
			RuntimeInputTimes(runtime, malformed);
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Require(rejected, "Malformed native body time reached the host clock");
	}
	for (unsigned fault = 0; fault < 3; ++fault)
	{
		s_invalid = fault == 0;
		s_failure = fault == 1;
		bool rejected = false;
		try
		{
			RuntimeTimeToHostNanoseconds(runtime, fault == 2 ? 0 : 1000000000 + s_vendorOffset);
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Require(rejected, "Invalid vendor timestamps and conversion errors must not become input timestamps");
	}
}
} // namespace simxr::headset
int main()
{
	try
	{
		simxr::headset::CheckConversion();
		std::cout << "Vendor clock conversion checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
