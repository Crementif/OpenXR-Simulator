#pragma once

#include <openxr/openxr.h>
#include <initializer_list>

namespace simxr
{
inline constexpr XrVersion SupportedApiVersion = XR_MAKE_VERSION(1, 1, 0);

constexpr XrVersion NegotiateApiVersion(XrVersion minimum, XrVersion maximum)
{
	// OpenXR patch revisions do not change API compatibility.
	const auto interfaceVersion = [](XrVersion version) {
		return XR_MAKE_VERSION(XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), 0);
	};
	for (const auto supported : std::initializer_list<XrVersion>{SupportedApiVersion, XR_MAKE_VERSION(1, 0, 27)})
		if (interfaceVersion(minimum) <= interfaceVersion(supported) &&
			interfaceVersion(supported) <= interfaceVersion(maximum))
			return supported;
	return 0;
}
} // namespace simxr
