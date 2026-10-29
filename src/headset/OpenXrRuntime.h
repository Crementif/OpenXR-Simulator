#pragma once

#include "headset/HeadsetBodyCapabilities.h"

#include <cstdint>
#include <openxr/openxr.h>
#include <string>
#include <string_view>
#include <vector>

namespace simxr::headset
{
void CheckXr(XrResult result, const char* operation);

class OpenXrRuntime
{
public:
	OpenXrRuntime() = default;
	~OpenXrRuntime();
	OpenXrRuntime(const OpenXrRuntime&) = delete;
	OpenXrRuntime& operator=(const OpenXrRuntime&) = delete;
	void Open();
	void PreserveNativeResources();
	bool ResourcesPreserved() const;
	bool Supports(std::string_view extension) const;
	PFN_xrVoidFunction Procedure(const char* name) const;
	XrInstance Instance() const;
	XrSystemId System() const;
	const XrInstanceProperties& Properties() const;
	const HeadsetBodyCapabilities& BodyCapabilities() const;
	const std::vector<XrViewConfigurationView>& Views() const;
	const std::vector<XrExtensionProperties>& Extensions() const;
	const std::vector<XrApiLayerProperties>& Layers() const;

private:
	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_system = XR_NULL_SYSTEM_ID;
	XrInstanceProperties m_properties{XR_TYPE_INSTANCE_PROPERTIES};
	std::vector<XrExtensionProperties> m_extensions;
	std::vector<XrApiLayerProperties> m_layers;
	std::vector<XrViewConfigurationView> m_views;
	bool m_preserved = false;
	HeadsetBodyCapabilities m_bodyCapabilities;
};
} // namespace simxr::headset
