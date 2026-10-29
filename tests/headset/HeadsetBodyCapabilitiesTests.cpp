#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetBodyCapabilities.h"
#include "headset/OpenXrRuntime.h"

#include <iostream>

namespace simxr::headset::test
{
static std::set<std::string_view> s_extensions;
static HeadsetBodyCapabilities s_system;
static std::set<XrStructureType> s_queries;
static XrResult s_result = XR_SUCCESS;
static unsigned s_calls = 0;
} // namespace simxr::headset::test

using namespace simxr::headset::test;

namespace simxr::headset
{
OpenXrRuntime::~OpenXrRuntime() = default;

XrInstance OpenXrRuntime::Instance() const
{
	return reinterpret_cast<XrInstance>(1);
}

XrSystemId OpenXrRuntime::System() const
{
	return 2;
}

bool OpenXrRuntime::Supports(std::string_view name) const
{
	return s_extensions.contains(name);
}

void CheckXr(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(operation);
}
} // namespace simxr::headset

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId system,
																XrSystemProperties* properties)
{
	Require(instance == reinterpret_cast<XrInstance>(1) && system == 2, "Query the selected physical system");
	Require(properties->type == XR_TYPE_SYSTEM_PROPERTIES, "System query structure");
	++s_calls;
	s_queries.clear();
	for (auto* item = static_cast<XrBaseOutStructure*>(properties->next); item; item = item->next)
	{
		Require(s_queries.insert(item->type).second, "No duplicate or cyclic capability structures");
		switch (item->type)
		{
		case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB:
			Require(s_extensions.contains(XR_FB_BODY_TRACKING_EXTENSION_NAME), "FB must be available");
			reinterpret_cast<XrSystemBodyTrackingPropertiesFB*>(item)->supportsBodyTracking = s_system.fb;
			break;
		case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META:
			Require(s_extensions.contains(XR_FB_BODY_TRACKING_EXTENSION_NAME) &&
						s_extensions.contains(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME),
					"META requires both extensions");
			reinterpret_cast<XrSystemPropertiesBodyTrackingFullBodyMETA*>(item)->supportsFullBodyTracking =
				s_system.fullBodyMETA;
			break;
		case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD:
			Require(s_extensions.contains(XR_BD_BODY_TRACKING_EXTENSION_NAME), "BD must be available");
			reinterpret_cast<XrSystemBodyTrackingPropertiesBD*>(item)->supportsBodyTracking = s_system.bd;
			break;
		default:
			throw std::runtime_error("Unexpected physical capability query");
		}
	}
	return s_result;
}

int main()
{
	try
	{
		using namespace simxr::headset;
		OpenXrRuntime runtime;
		const std::array names{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
							   XR_BD_BODY_TRACKING_EXTENSION_NAME};
		for (unsigned extensions = 0; extensions < 8; ++extensions)
		{
			s_extensions.clear();
			for (unsigned index = 0; index < names.size(); ++index)
				if (extensions & (1U << index))
					s_extensions.insert(names[index]);
			for (unsigned supported = 0; supported < 8; ++supported)
			{
				s_system = {bool(supported & 1), bool(supported & 2), bool(supported & 4)};
				s_calls = 0;
				s_queries.clear();
				const auto actual = ReadHeadsetBodyCapabilities(runtime);
				Require(actual.fb == bool(extensions & supported & 1) &&
							actual.bd == bool(extensions & supported & 4) &&
							actual.fullBodyMETA == ((extensions & supported & 3) == 3),
						"Extension availability must not fabricate selected-system body support");
				Require(s_calls == ((extensions & 5) ? 1U : 0U),
						"Missing body extensions must not issue a native body query");
				Require(s_queries.size() == unsigned(bool(extensions & 1)) + unsigned(bool(extensions & 4)) +
												unsigned((extensions & 3) == 3),
						"Query every enabled body schema exactly once");
			}
		}
		s_result = XR_ERROR_RUNTIME_FAILURE;
		bool rejected = false;
		try
		{
			ReadHeadsetBodyCapabilities(runtime);
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Require(rejected, "Native query failure must not masquerade as unsupported body tracking");
		std::cout << "Headset body capabilities passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
