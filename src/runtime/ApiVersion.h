#pragma once

#include <openxr/openxr.h>

namespace simxr
{
inline constexpr XrVersion SupportedApiVersion = XR_MAKE_VERSION(1, 0, 27);

constexpr XrVersion NegotiateApiVersion(XrVersion minimum, XrVersion maximum)
{
	// OpenXR patch revisions do not change API compatibility.
	const auto interfaceVersion = [](XrVersion version) {
		return XR_MAKE_VERSION(XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), 0);
	};
	const auto supported = interfaceVersion(SupportedApiVersion);
	return interfaceVersion(minimum) <= supported && supported <= interfaceVersion(maximum) ? SupportedApiVersion : 0;
}
} // namespace simxr
