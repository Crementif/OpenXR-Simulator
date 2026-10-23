#pragma once

#include <new>
#include <openxr/openxr.h>

namespace simxr
{
template <auto Function> struct AbiBoundary;

template <typename... Arguments, XrResult(XRAPI_PTR* Function)(Arguments...)> struct AbiBoundary<Function>
{
	static XrResult XRAPI_PTR Invoke(Arguments... arguments) noexcept
	{
		try
		{
			return Function(arguments...);
		}
		catch (const std::bad_alloc&)
		{
			return XR_ERROR_OUT_OF_MEMORY;
		}
		catch (...)
		{
			return XR_ERROR_RUNTIME_FAILURE;
		}
	}
};
} // namespace simxr
