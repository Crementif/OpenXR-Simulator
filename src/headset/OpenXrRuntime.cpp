#include "headset/OpenXrRuntime.h"
#include "Pch.h"
#include "headset/RuntimeSelection.h"

namespace simxr::headset
{
void CheckXr(XrResult result, const char* operation)
{
	if (result == XR_ERROR_RUNTIME_UNAVAILABLE)
		throw std::runtime_error(
			"No usable physical OpenXR runtime was found. Install or select the headset runtime, then retry");
	if (XR_FAILED(result))
		throw std::runtime_error(std::string(operation) + " failed with OpenXR result " + std::to_string(result));
}

OpenXrRuntime::~OpenXrRuntime()
{
	if (m_instance && !m_preserved)
		xrDestroyInstance(m_instance);
}

void OpenXrRuntime::Open()
{
	if (m_instance)
		throw std::logic_error("Headset runtime is already open");
	std::uint32_t count = 0;
	CheckXr(xrEnumerateApiLayerProperties(0, &count, nullptr), "Enumerate API layers");
	m_layers.assign(count, {XR_TYPE_API_LAYER_PROPERTIES});
	CheckXr(xrEnumerateApiLayerProperties(count, &count, m_layers.data()), "Read API layers");
	m_layers.resize(count);
	CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "Find physical runtime extensions");
	m_extensions.assign(count, {XR_TYPE_EXTENSION_PROPERTIES});
	CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, m_extensions.data()),
			"Read runtime extensions");
	m_extensions.resize(count);
	std::vector<const char*> enabled;
	const char* required[] = {"XR_KHR_vulkan_enable2",
#ifdef _WIN32
							  "XR_KHR_win32_convert_performance_counter_time"
#else
							  "XR_KHR_convert_timespec_time"
#endif
	};
	for (const auto* extension : required)
	{
		if (Supports(extension))
			enabled.push_back(extension);
	}
	if (Supports("XR_EXT_local_floor"))
		enabled.push_back("XR_EXT_local_floor");
	if (Supports(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME))
		enabled.push_back(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
	if (Supports(XR_FB_BODY_TRACKING_EXTENSION_NAME))
	{
		enabled.push_back(XR_FB_BODY_TRACKING_EXTENSION_NAME);
		if (Supports(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME))
			enabled.push_back(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
	}
	if (Supports(XR_BD_BODY_TRACKING_EXTENSION_NAME))
		enabled.push_back(XR_BD_BODY_TRACKING_EXTENSION_NAME);
	XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(info.applicationInfo.applicationName, "SimXR Headset Host");
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
	info.enabledExtensionNames = enabled.data();
	CheckXr(xrCreateInstance(&info, &m_instance), "Open physical runtime");
	CheckXr(xrGetInstanceProperties(m_instance, &m_properties), "Read runtime identity");
	if (IsSimulatorRuntime(m_properties.runtimeName))
		throw std::runtime_error("The selected runtime is the simulator; choose a physical OpenXR runtime");
	for (const auto* extension : required)
		if (!Supports(extension))
			throw std::runtime_error(std::string("Physical runtime lacks required extension ") + extension);
	XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	const auto result = xrGetSystem(m_instance, &systemInfo, &m_system);
	if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
		throw std::runtime_error("No headset is available in the selected runtime; connect it and retry");
	CheckXr(result, "Find physical headset");
	m_bodyCapabilities = ReadHeadsetBodyCapabilities(*this);
	CheckXr(xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
											  &count, nullptr),
			"Find stereo views");
	if (count != 2)
		throw std::runtime_error("Physical runtime does not provide two stereo views");
	m_views.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	CheckXr(xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
											  &count, m_views.data()),
			"Read stereo views");
}

void OpenXrRuntime::PreserveNativeResources()
{
	m_preserved = true;
}

bool OpenXrRuntime::ResourcesPreserved() const
{
	return m_preserved;
}

bool OpenXrRuntime::Supports(std::string_view extension) const
{
	return std::any_of(m_extensions.begin(), m_extensions.end(),
					   [&](const auto& item) { return item.extensionName == extension; });
}

PFN_xrVoidFunction OpenXrRuntime::Procedure(const char* name) const
{
	PFN_xrVoidFunction procedure = nullptr;
	CheckXr(xrGetInstanceProcAddr(m_instance, name, &procedure), name);
	if (!procedure)
		throw std::runtime_error(std::string("Missing OpenXR procedure ") + name);
	return procedure;
}

XrInstance OpenXrRuntime::Instance() const
{
	return m_instance;
}

XrSystemId OpenXrRuntime::System() const
{
	return m_system;
}

const XrInstanceProperties& OpenXrRuntime::Properties() const
{
	return m_properties;
}

const HeadsetBodyCapabilities& OpenXrRuntime::BodyCapabilities() const
{
	return m_bodyCapabilities;
}

const std::vector<XrViewConfigurationView>& OpenXrRuntime::Views() const
{
	return m_views;
}

const std::vector<XrExtensionProperties>& OpenXrRuntime::Extensions() const
{
	return m_extensions;
}

const std::vector<XrApiLayerProperties>& OpenXrRuntime::Layers() const
{
	return m_layers;
}
} // namespace simxr::headset
