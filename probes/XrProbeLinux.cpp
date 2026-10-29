// A real loader/Vulkan integration test: renders distinctive per-eye colors into
// runtime-owned images, checks poses/actions, and verifies the captured pixels.
#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#include "Pch.h"
#include "ProbeEnvironment.h"
#include "ProbeSession.h"
#include "control/LocalControlClient.h"
#include "control/ControlSchema.h"
#include "control/ImageInspection.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "VulkanAdmissionChecks.h"
#include "VulkanCreationChecks.h"
#include "platform/linux/InstallUse.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <openxr/openxr_platform.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace simxr::probe
{
static std::uint64_t s_checks = 0;
static std::atomic<int> s_validationErrors{0};

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
													VkDebugUtilsMessageTypeFlagsEXT,
													const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		++s_validationErrors;
	std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
	return VK_FALSE;
}

#define CHECK(c)                                                                                                       \
	do                                                                                                                 \
	{                                                                                                                  \
		++s_checks;                                                                                                    \
		if (!(c))                                                                                                      \
		{                                                                                                              \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                                  \
			std::fflush(stderr);                                                                                       \
			std::_Exit(1);                                                                                             \
		}                                                                                                              \
	}                                                                                                                  \
	while (0)
#define XR(c) CHECK((c) == XR_SUCCESS)
#define VK(c) CHECK((c) == VK_SUCCESS)

template<typename T>
static T Command(XrInstance i, const char* name)
{
	PFN_xrVoidFunction p = nullptr;
	if (xrGetInstanceProcAddr(i, name, &p) != XR_SUCCESS || !p)
	{
		std::fprintf(stderr, "missing %s\n", name);
		std::_Exit(1);
	}
	return reinterpret_cast<T>(p);
}

[[noreturn]] static void Fail(const char* message)
{
	std::fprintf(stderr, "FAIL: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

static void WriteText(const std::filesystem::path& path, std::string_view text)
{
	std::ofstream file(path, std::ios::binary);
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	file.close();
	if (!file)
		Fail("Cannot write or close probe output");
}

static int RunProbe()
{
	const int frames = std::max(4, EnvironmentCount("SIMXR_PROBE_FRAMES", 4, 0, std::numeric_limits<int>::max() - 3));
	const int slowEvery = EnvironmentCount("SIMXR_PROBE_SLOW_EVERY", 0, 0);
	const int clearFrame = EnvironmentCount("SIMXR_PROBE_CLEAR_FRAME", 0, 0);
	const char* env = std::getenv("SIMXR_DATA_DIR");
	CHECK(env && *env);
	std::filesystem::path data(env);
	const bool creationRollback = EnvOn("SIMXR_PROBE_CREATION_ROLLBACK");
	const auto unselectedDirectory = data / ("pre-instance-unselected." + std::to_string(getpid()));
	if (creationRollback)
		CHECK(setenv("SIMXR_DATA_DIR", unselectedDirectory.c_str(), 1) == 0);
	bool v1 = EnvOn("SIMXR_PROBE_ENABLE1");
	const bool bodyTracking = EnvOn("SIMXR_PROBE_BODY");
	const bool depthMode = EnvOn("SIMXR_PROBE_DEPTH");
	const bool quadMode = EnvOn("SIMXR_PROBE_QUAD");
	const bool foveatedMode = EnvOn("SIMXR_PROBE_FOVEATED");
	CHECK(!foveatedMode || quadMode);
	const bool verifyCapture = !foveatedMode && (quadMode || !EnvOn("SIMXR_PROBE_GPU_INSPECTION"));
	const uint32_t activeViewCount = quadMode ? 4 : 2;
	const auto activeViewType =
		quadMode ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	const bool devicesMode = EnvOn("SIMXR_PROBE_DEVICES");
	const bool interactive = EnvOn("SIMXR_PROBE_INTERACTIVE");
	uint32_t count = 0;
	XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
	CHECK(count >= 3);
	std::vector<XrExtensionProperties> advertised(count, {XR_TYPE_EXTENSION_PROPERTIES});
	XR(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, advertised.data()));
	const auto quadExtension = std::find_if(advertised.begin(), advertised.end(), [](const auto& extension) {
		return std::strcmp(extension.extensionName, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME) == 0;
	});
	CHECK(quadExtension != advertised.end() && quadExtension->extensionVersion == XR_VARJO_quad_views_SPEC_VERSION);
	CHECK(std::any_of(advertised.begin(), advertised.end(), [](const auto& extension) {
		return std::strcmp(extension.extensionName, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME) == 0;
	}));
	CHECK(std::none_of(advertised.begin(), advertised.end(), [](const auto& extension) {
		return std::strcmp(extension.extensionName, XR_FB_FOVEATION_EXTENSION_NAME) == 0;
	}));
	std::vector<const char*> extensions = {
		v1 ? XR_KHR_VULKAN_ENABLE_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
		XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
	if (quadMode)
		extensions.push_back(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
	if (foveatedMode)
		extensions.push_back(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
	if (bodyTracking)
		extensions.insert(extensions.end(),
						  {XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
						   XR_BD_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME,
						   XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME});
	if (devicesMode)
		extensions.insert(extensions.end(), {XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
											 XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME});
	XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(ci.applicationInfo.applicationName, "SimXR Linux Probe");
	ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	ci.enabledExtensionNames = extensions.data();
	if (creationRollback)
		CHECK(setenv("SIMXR_DATA_DIR", data.c_str(), 1) == 0);
	// Native configuration failure must release CPU admission and file workers,
	// so correcting the profile can create a fresh instance in the same process.
	if (creationRollback)
	{
		const auto* configured = std::getenv("SIMXR_HEADSET_PROFILE");
		const bool hadProfile = configured != nullptr;
		const std::string previous = configured ? configured : "";
		CHECK(setenv("SIMXR_HEADSET_PROFILE", "simxr-deliberately-invalid-profile", 1) == 0);
		XrInstance rejected = XR_NULL_HANDLE;
		const auto failure = xrCreateInstance(&ci, &rejected);
		if (hadProfile)
			CHECK(setenv("SIMXR_HEADSET_PROFILE", previous.c_str(), 1) == 0);
		else
			CHECK(unsetenv("SIMXR_HEADSET_PROFILE") == 0);
		CHECK(failure == XR_ERROR_RUNTIME_FAILURE);
		CHECK(rejected == XR_NULL_HANDLE);
		for (const auto* name : {"SIMXR_WIDTH", "SIMXR_HEIGHT"})
		{
			const auto* original = std::getenv(name);
			const bool hadExtent = original != nullptr;
			const std::string previousExtent = original ? original : "";
			for (const auto* value : {"64px", "999999999999999999999999999999999999"})
			{
				CHECK(setenv(name, value, 1) == 0);
				CHECK(xrCreateInstance(&ci, &rejected) == XR_ERROR_RUNTIME_FAILURE);
				CHECK(rejected == XR_NULL_HANDLE);
			}
			if (hadExtent)
				CHECK(setenv(name, previousExtent.c_str(), 1) == 0);
			else
				CHECK(unsetenv(name) == 0);
		}
	}
	XrInstance instance = XR_NULL_HANDLE;
	XR(xrCreateInstance(&ci, &instance));
	if (creationRollback)
		CHECK(!std::filesystem::exists(unselectedDirectory));
	XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
	XR(xrGetInstanceProperties(instance, &props));
	std::printf("runtime: %s %u.%u.%u\n", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
				XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
	XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	XR(xrGetSystem(instance, &si, &system));
	CHECK(xrGetInstanceProperties(instance, nullptr) == XR_ERROR_VALIDATION_FAILURE);
	XrInstanceProperties invalidProperties{};
	CHECK(xrGetInstanceProperties(instance, &invalidProperties) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(xrGetSystem(instance, nullptr, &system) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(xrGetSystem(instance, &si, nullptr) == XR_ERROR_VALIDATION_FAILURE);
	XrSystemGetInfo invalidSystemInfo{};
	CHECK(xrGetSystem(instance, &invalidSystemInfo, &system) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(xrResultToString(instance, XR_SUCCESS, nullptr) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(xrStructureTypeToString(instance, XR_TYPE_INSTANCE_PROPERTIES, nullptr) == XR_ERROR_VALIDATION_FAILURE);
	char resultName[XR_MAX_RESULT_STRING_SIZE]{};
	char structureName[XR_MAX_STRUCTURE_NAME_SIZE]{};
	XR(xrResultToString(instance, XR_ERROR_HANDLE_INVALID, resultName));
	CHECK(std::strcmp(resultName, "XR_ERROR_HANDLE_INVALID") == 0);
	XR(xrResultToString(instance, static_cast<XrResult>(123456789), resultName));
	CHECK(std::strcmp(resultName, "XR_UNKNOWN_SUCCESS_123456789") == 0);
	XR(xrStructureTypeToString(instance, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, structureName));
	CHECK(std::strcmp(structureName, "XR_TYPE_GRAPHICS_BINDING_D3D11_KHR") == 0);
	CHECK(xrResultToString(XR_NULL_HANDLE, XR_SUCCESS, resultName) == XR_ERROR_HANDLE_INVALID);
	CHECK(xrStructureTypeToString(XR_NULL_HANDLE, XR_TYPE_INSTANCE_PROPERTIES, structureName) ==
		  XR_ERROR_HANDLE_INVALID);
	const auto fromTimespec = Command<PFN_xrConvertTimespecTimeToTimeKHR>(instance, "xrConvertTimespecTimeToTimeKHR");
	const auto toTimespec = Command<PFN_xrConvertTimeToTimespecTimeKHR>(instance, "xrConvertTimeToTimespecTimeKHR");
	const timespec nativeTime{17, 23};
	XrTime convertedTime = 0;
	XR(fromTimespec(instance, &nativeTime, &convertedTime));
	CHECK(convertedTime == 17000000023LL);
	timespec convertedNative{};
	XR(toTimespec(instance, convertedTime, &convertedNative));
	CHECK(convertedNative.tv_sec == nativeTime.tv_sec && convertedNative.tv_nsec == nativeTime.tv_nsec);
	const auto maximumTime = std::numeric_limits<XrTime>::max();
	const bool fitsNativeTime = std::in_range<decltype(convertedNative.tv_sec)>(maximumTime / 1000000000);
	convertedNative = nativeTime;
	CHECK(toTimespec(instance, maximumTime, &convertedNative) == (fitsNativeTime ? XR_SUCCESS : XR_ERROR_TIME_INVALID));
	if (fitsNativeTime)
	{
		XR(fromTimespec(instance, &convertedNative, &convertedTime));
		CHECK(convertedTime == maximumTime);
	}
	else
		CHECK(convertedNative.tv_sec == nativeTime.tv_sec && convertedNative.tv_nsec == nativeTime.tv_nsec);

	XR(xrEnumerateViewConfigurations(instance, system, 0, &count, nullptr));
	CHECK(count == (quadMode ? 2u : 1u));
	std::vector<XrViewConfigurationType> configurations(count);
	XR(xrEnumerateViewConfigurations(instance, system, count, &count, configurations.data()));
	CHECK(configurations[0] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	if (quadMode)
		CHECK(configurations[1] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
	XrViewConfigurationProperties viewProperties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
	if (!quadMode)
		CHECK(xrGetViewConfigurationProperties(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO,
											   &viewProperties) == XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
	XR(xrGetViewConfigurationProperties(instance, system, activeViewType, &viewProperties));
	CHECK(viewProperties.viewConfigurationType == activeViewType && viewProperties.fovMutable);
	CHECK(xrEnumerateViewConfigurationViews(XR_NULL_HANDLE, system, activeViewType, 0, &count, nullptr) ==
		  XR_ERROR_HANDLE_INVALID);
	CHECK(xrEnumerateViewConfigurationViews(instance, XR_NULL_SYSTEM_ID, activeViewType, 0, &count, nullptr) ==
		  XR_ERROR_SYSTEM_INVALID);
	XR(xrEnumerateViewConfigurationViews(instance, system, activeViewType, 0, &count, nullptr));
	CHECK(count == activeViewCount);
	std::vector<XrViewConfigurationView> recommended(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	recommended[0].recommendedImageRectWidth = 123;
	CHECK(xrEnumerateViewConfigurationViews(instance, system, activeViewType, count - 1, &count, recommended.data()) ==
			  XR_ERROR_SIZE_INSUFFICIENT &&
		  recommended[0].recommendedImageRectWidth == 123);
	XrBaseOutStructure unknown{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, nullptr};
	recommended[0].next = &unknown;
	XR(xrEnumerateViewConfigurationViews(instance, system, activeViewType, count, &count, recommended.data()));
	CHECK(recommended[0].next == &unknown && recommended[0].recommendedImageRectWidth > 0 &&
		  recommended[0].recommendedImageRectHeight > 0);
	if (quadMode)
		CHECK(recommended[2].recommendedImageRectWidth > 0 && recommended[2].recommendedImageRectHeight > 0);
	if (foveatedMode)
	{
		std::vector<XrFoveatedViewConfigurationViewVARJO> requests(
			activeViewCount, {XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO, nullptr, XR_TRUE});
		for (uint32_t index = 0; index < activeViewCount; ++index)
			recommended[index].next = &requests[index];
		XR(xrEnumerateViewConfigurationViews(instance, system, activeViewType, activeViewCount, &count,
											 recommended.data()));
		CHECK(recommended[0].recommendedImageRectWidth > 0 && recommended[2].recommendedImageRectWidth > 0);
		requests[0].foveatedRenderingActive = 2;
		CHECK(xrEnumerateViewConfigurationViews(instance, system, activeViewType, activeViewCount, &count,
												recommended.data()) == XR_ERROR_VALIDATION_FAILURE);
		for (auto& view : recommended)
			view.next = nullptr;
		XrSystemFoveatedRenderingPropertiesVARJO foveation{XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO};
		XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &foveation};
		XR(xrGetSystemProperties(instance, system, &properties));
		CHECK(foveation.supportsFoveatedRendering);
	}
	XrSystemBodyTrackingPropertiesFB fbProperties{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB};
	XrSystemBodyTrackingPropertiesBD bdProperties{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD};
	XrSystemPropertiesBodyTrackingFullBodyMETA fullProperties{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META};
	XrSystemPropertiesBodyTrackingCalibrationMETA calibrationProperties{
		XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META};
	XrSystemPropertiesBodyTrackingFidelityMETA fidelityProperties{
		XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META};
	XrSystemEyeGazeInteractionPropertiesEXT eyeProperties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, nullptr,
														  42};
	fidelityProperties.next = &eyeProperties;
	fbProperties.next = &bdProperties;
	bdProperties.next = &fullProperties;
	fullProperties.next = &calibrationProperties;
	calibrationProperties.next = &fidelityProperties;
	XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
	systemProperties.next = &fbProperties;
	XR(xrGetSystemProperties(instance, system, &systemProperties));
	CHECK(systemProperties.next == &fbProperties && fbProperties.next == &bdProperties);
	if (bodyTracking)
		CHECK(fbProperties.supportsBodyTracking && bdProperties.supportsBodyTracking &&
			  fullProperties.supportsFullBodyTracking && calibrationProperties.supportsHeightOverride &&
			  fidelityProperties.supportsBodyTrackingFidelity);
	else
	{
		CHECK(!fbProperties.supportsBodyTracking && !bdProperties.supportsBodyTracking);
		PFN_xrVoidFunction disabled = nullptr;
		CHECK(xrGetInstanceProcAddr(instance, "xrCreateBodyTrackerFB", &disabled) == XR_ERROR_FUNCTION_UNSUPPORTED &&
			  disabled == nullptr);
	}
	XrPath trackerPersistent = XR_NULL_PATH, trackerRole = XR_NULL_PATH;
	PFN_xrEnumerateViveTrackerPathsHTCX enumerateTrackers = nullptr;
	if (devicesMode)
	{
		CHECK(eyeProperties.supportsEyeGazeInteraction == XR_TRUE && fidelityProperties.next == &eyeProperties);
		enumerateTrackers = Command<PFN_xrEnumerateViveTrackerPathsHTCX>(instance, "xrEnumerateViveTrackerPathsHTCX");
		XR(enumerateTrackers(instance, 0, &count, nullptr));
		CHECK(count == 4);
		std::vector<XrViveTrackerPathsHTCX> paths(count, {XR_TYPE_VIVE_TRACKER_PATHS_HTCX});
		CHECK(enumerateTrackers(instance, count - 1, &count, paths.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == 4);
		XR(enumerateTrackers(instance, count, &count, paths.data()));
		trackerPersistent = paths[0].persistentPath;
		trackerRole = paths[0].rolePath;
		CHECK(trackerPersistent != XR_NULL_PATH && trackerRole != XR_NULL_PATH);
		CHECK(enumerateTrackers(XR_NULL_HANDLE, 0, &count, nullptr) == XR_ERROR_HANDLE_INVALID);
		CHECK(enumerateTrackers(instance, 0, nullptr, nullptr) == XR_ERROR_VALIDATION_FAILURE);
		uint32_t trackerEvents = 0;
		PollXrEvents(instance, [&](const XrEventDataBuffer& initialEvent) {
			CHECK(initialEvent.type == XR_TYPE_EVENT_DATA_VIVE_TRACKER_CONNECTED_HTCX);
			const auto* connected = reinterpret_cast<const XrEventDataViveTrackerConnectedHTCX*>(&initialEvent);
			const auto address = reinterpret_cast<std::uintptr_t>(connected->paths);
			CHECK(address >= reinterpret_cast<std::uintptr_t>(&initialEvent) &&
				  address + sizeof(*connected->paths) <=
					  reinterpret_cast<std::uintptr_t>(&initialEvent) + sizeof(initialEvent));
			CHECK(connected->paths->type == XR_TYPE_VIVE_TRACKER_PATHS_HTCX && connected->paths->next == nullptr);
			char persistentName[XR_MAX_PATH_LENGTH];
			XR(xrPathToString(instance, connected->paths->persistentPath, sizeof(persistentName), &count,
							  persistentName));
			CHECK(std::strstr(persistentName, "/user/vive_tracker_htcx/serial/simxr_") == persistentName);
			++trackerEvents;
		});
		CHECK(trackerEvents == 4);
	}
	else
	{
		CHECK(eyeProperties.supportsEyeGazeInteraction == 42);
		PFN_xrVoidFunction disabled = nullptr;
		CHECK(xrGetInstanceProcAddr(instance, "xrEnumerateViveTrackerPathsHTCX", &disabled) ==
				  XR_ERROR_FUNCTION_UNSUPPORTED &&
			  !disabled);
	}
	auto requirements = Command<PFN_xrGetVulkanGraphicsRequirementsKHR>(
		instance, v1 ? "xrGetVulkanGraphicsRequirementsKHR" : "xrGetVulkanGraphicsRequirements2KHR");
	XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vci.pApplicationInfo = &app;
	// Enable validation when available, without making installed layers mandatory.
	uint32_t layerCount = 0;
	VK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
	std::vector<VkLayerProperties> layers(layerCount);
	VK(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()));
	const char* validation = "VK_LAYER_KHRONOS_validation";
	bool hasValidation = false;
	for (auto& l : layers)
		hasValidation |= std::strcmp(l.layerName, validation) == 0;
	if (hasValidation)
	{
		vci.enabledLayerCount = 1;
		vci.ppEnabledLayerNames = &validation;
		static const char* s_debugExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		vci.enabledExtensionCount = 1;
		vci.ppEnabledExtensionNames = &s_debugExtension;
	}
	VkInstance vkInstance = VK_NULL_HANDLE;
	VkResult result;
	if (v1)
		VK(vkCreateInstance(&vci, nullptr, &vkInstance));
	else
	{
		XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
		create.systemId = system;
		create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		create.vulkanCreateInfo = &vci;
		XR(Command<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR")(instance, &create, &vkInstance,
																						 &result));
		VK(result);
	}
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
	if (hasValidation)
	{
		CHECK(createDebug);
		VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		debug.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
							VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
							VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debug.pfnUserCallback = DebugCallback;
		VK(createDebug(vkInstance, &debug, nullptr, &messenger));
	}
	VkPhysicalDevice physical;
	if (v1)
		XR(Command<PFN_xrGetVulkanGraphicsDeviceKHR>(instance, "xrGetVulkanGraphicsDeviceKHR")(instance, system,
																							   vkInstance, &physical));
	else
	{
		XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		get.systemId = system;
		get.vulkanInstance = vkInstance;
		XR(Command<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR")(instance, &get,
																								 &physical));
	}
	VkPhysicalDeviceProperties gpu{};
	if (!v1)
		CHECK(VerifyVulkanCreation(instance, system, vkInstance, physical));
	XR(requirements(instance, system, &req));
	vkGetPhysicalDeviceProperties(physical, &gpu);
	std::printf("GPU: %s; validation=%s; Vulkan path=%s\n", gpu.deviceName, hasValidation ? "on" : "unavailable",
				v1 ? "v1" : "v2");
	uint32_t queues = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &queues, nullptr);
	std::vector<VkQueueFamilyProperties> qprops(queues);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &queues, qprops.data());
	uint32_t family = 0;
	while (family < queues && !(qprops[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		++family;
	CHECK(family < queues);
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	std::vector<std::string> requiredDeviceExtensions;
	std::vector<const char*> deviceExtensionNames;
	if (v1)
	{
		const auto get = Command<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR");
		uint32_t bytes = 0;
		XR(get(instance, system, 0, &bytes, nullptr));
		CHECK(bytes > 0);
		std::vector<char> names(bytes);
		if (bytes > 1)
		{
			CHECK(get(instance, system, bytes - 1, &bytes, names.data()) == XR_ERROR_SIZE_INSUFFICIENT);
			CHECK(names.front() == 0);
		}
		XR(get(instance, system, bytes, &bytes, names.data()));
		std::istringstream split(names.data());
		for (std::string name; split >> name;)
			requiredDeviceExtensions.push_back(std::move(name));
		for (const auto& name : requiredDeviceExtensions)
			deviceExtensionNames.push_back(name.c_str());
		dci.enabledExtensionCount = static_cast<uint32_t>(deviceExtensionNames.size());
		dci.ppEnabledExtensionNames = deviceExtensionNames.data();
	}
	VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	VkPhysicalDeviceFeatures2 availableFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	availableFeatures.pNext = &timelineFeature;
	vkGetPhysicalDeviceFeatures2(physical, &availableFeatures);
	CHECK(timelineFeature.timelineSemaphore);
	dci.pNext = &timelineFeature;
	VkDevice device;
	if (v1)
		VK(vkCreateDevice(physical, &dci, nullptr, &device));
	else
	{
		XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
		create.systemId = system;
		create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		create.vulkanPhysicalDevice = physical;
		create.vulkanCreateInfo = &dci;
		XR(Command<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR")(instance, &create, &device,
																					 &result));
		VK(result);
	}
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);
	XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance = vkInstance;
	binding.physicalDevice = physical;
	binding.device = device;
	binding.queueFamilyIndex = family;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = system;
	sci.next = &binding;
	const bool externalEnabled = vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR") &&
								 vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR") &&
								 vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR");
	if (externalEnabled)
	{
		auto minimal = dci;
		minimal.enabledExtensionCount = 0;
		minimal.ppEnabledExtensionNames = nullptr;
		VkDevice unconfigured = VK_NULL_HANDLE;
		VK(vkCreateDevice(physical, &minimal, nullptr, &unconfigured));
		auto invalidBinding = binding;
		invalidBinding.device = unconfigured;
		auto invalidCreate = sci;
		invalidCreate.next = &invalidBinding;
		XrSession untouched = XR_NULL_HANDLE;
		CHECK(xrCreateSession(instance, &invalidCreate, &untouched) == XR_ERROR_GRAPHICS_DEVICE_INVALID &&
			  untouched == XR_NULL_HANDLE);
		vkDestroyDevice(unconfigured, nullptr);
		std::puts("External sharing negotiation: actual logical extension enablement checked");
	}
	for (const bool invalidFamily : {true, false})
	{
		auto invalidBinding = binding;
		if (invalidFamily)
			invalidBinding.queueFamilyIndex = std::numeric_limits<std::uint32_t>::max();
		else
			invalidBinding.queueIndex = std::numeric_limits<std::uint32_t>::max();
		auto invalidCreate = sci;
		invalidCreate.next = &invalidBinding;
		XrSession untouched = XR_NULL_HANDLE;
		CHECK(xrCreateSession(instance, &invalidCreate, &untouched) == XR_ERROR_GRAPHICS_DEVICE_INVALID &&
			  untouched == XR_NULL_HANDLE);
	}
	XrSession session;
	XR(xrCreateSession(instance, &sci, &session));
	bool ready = false;
	PollXrEvents(instance, [&](const XrEventDataBuffer& event) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
			CHECK(changed.session == session);
			ready |= changed.state == XR_SESSION_STATE_READY;
		}
	});
	CHECK(ready);
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
	if (!quadMode)
	{
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
		CHECK(xrBeginSession(session, &begin) == XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
	}
	begin.primaryViewConfigurationType = activeViewType;
	XR(xrBeginSession(session, &begin));
	CHECK(xrBeginSession(session, &begin) == XR_ERROR_SESSION_RUNNING);
	XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	rs.poseInReferenceSpace.orientation.w = 1;
	XrSpace stage;
	XR(xrCreateReferenceSpace(session, &rs, &stage));
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XrSpace head;
	XR(xrCreateReferenceSpace(session, &rs, &head));
	XrBodyTrackerFB body = XR_NULL_HANDLE;
	XrBodyTrackerFB upperBody = XR_NULL_HANDLE;
	XrBodyTrackerBD bodyBD = XR_NULL_HANDLE;
	PFN_xrLocateBodyJointsFB locateBody = nullptr;
	PFN_xrLocateBodyJointsBD locateBodyBD = nullptr;
	PFN_xrGetBodySkeletonFB skeletonBody = nullptr;
	PFN_xrDestroyBodyTrackerFB destroyBody = nullptr;
	PFN_xrDestroyBodyTrackerBD destroyBodyBD = nullptr;
	if (bodyTracking)
	{
		locateBody = Command<PFN_xrLocateBodyJointsFB>(instance, "xrLocateBodyJointsFB");
		locateBodyBD = Command<PFN_xrLocateBodyJointsBD>(instance, "xrLocateBodyJointsBD");
		skeletonBody = Command<PFN_xrGetBodySkeletonFB>(instance, "xrGetBodySkeletonFB");
		destroyBody = Command<PFN_xrDestroyBodyTrackerFB>(instance, "xrDestroyBodyTrackerFB");
		destroyBodyBD = Command<PFN_xrDestroyBodyTrackerBD>(instance, "xrDestroyBodyTrackerBD");
		auto create = Command<PFN_xrCreateBodyTrackerFB>(instance, "xrCreateBodyTrackerFB");
		XrBodyTrackerCreateInfoFB createInfo{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB};
		createInfo.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
		XR(create(session, &createInfo, &body));
		createInfo.bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB;
		XR(create(session, &createInfo, &upperBody));
		XrBodyTrackerCreateInfoBD createBD{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD};
		createBD.jointSet = XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD;
		XR(Command<PFN_xrCreateBodyTrackerBD>(instance, "xrCreateBodyTrackerBD")(session, &createBD, &bodyBD));
		XrBodyTrackingCalibrationInfoMETA calibration{XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META};
		calibration.bodyHeight = 1.7f;
		XR(Command<PFN_xrSuggestBodyTrackingCalibrationOverrideMETA>(
			instance, "xrSuggestBodyTrackingCalibrationOverrideMETA")(body, &calibration));
		XR(Command<PFN_xrResetBodyTrackingCalibrationMETA>(instance, "xrResetBodyTrackingCalibrationMETA")(body));
		XR(Command<PFN_xrRequestBodyTrackingFidelityMETA>(instance, "xrRequestBodyTrackingFidelityMETA")(
			body, XR_BODY_TRACKING_FIDELITY_LOW_META));
	}
	XrPath right, triggerPath, posePath, profile;
	XR(xrStringToPath(instance, "/user/hand/right", &right));
	XR(xrStringToPath(instance, "/user/hand/right/input/trigger/value", &triggerPath));
	XR(xrStringToPath(instance, "/user/hand/right/input/grip/pose", &posePath));
	XR(xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile));
	XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(asci.actionSetName, "probe");
	std::strcpy(asci.localizedActionSetName, "Probe");
	XrActionSet set;
	XR(xrCreateActionSet(instance, &asci, &set));
	XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
	aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
	std::strcpy(aci.actionName, "trigger");
	std::strcpy(aci.localizedActionName, "Trigger");
	aci.countSubactionPaths = 1;
	aci.subactionPaths = &right;
	XrAction trigger;
	XR(xrCreateAction(set, &aci, &trigger));
	aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
	std::strcpy(aci.actionName, "pose");
	std::strcpy(aci.localizedActionName, "Pose");
	XrAction pose;
	XR(xrCreateAction(set, &aci, &pose));
	aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	std::strcpy(aci.actionName, "haptic");
	std::strcpy(aci.localizedActionName, "Haptic");
	XrAction haptic;
	XR(xrCreateAction(set, &aci, &haptic));
	XrPath hapticPath;
	XR(xrStringToPath(instance, "/user/hand/right/output/haptic", &hapticPath));
	XrActionSuggestedBinding suggested[] = {{trigger, triggerPath}, {pose, posePath}, {haptic, hapticPath}};
	XrInteractionProfileSuggestedBinding suggestions{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggestions.interactionProfile = profile;
	suggestions.countSuggestedBindings = 3;
	suggestions.suggestedBindings = suggested;
	XR(xrSuggestInteractionProfileBindings(instance, &suggestions));
	XrAction gazeAction = XR_NULL_HANDLE, trackerPose = XR_NULL_HANDLE, trackerHaptic = XR_NULL_HANDLE;
	XrPath eyes = XR_NULL_PATH, gazeProfile = XR_NULL_PATH, trackerProfile = XR_NULL_PATH;
	if (devicesMode)
	{
		XR(xrStringToPath(instance, "/user/eyes_ext", &eyes));
		XR(xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &gazeProfile));
		XR(xrStringToPath(instance, "/interaction_profiles/htc/vive_tracker_htcx", &trackerProfile));
		aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
		aci.countSubactionPaths = 1;
		aci.subactionPaths = &eyes;
		std::strcpy(aci.actionName, "gaze");
		std::strcpy(aci.localizedActionName, "Gaze");
		XR(xrCreateAction(set, &aci, &gazeAction));
		XrPath gazePath;
		XR(xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath));
		XrActionSuggestedBinding gazeBinding{gazeAction, gazePath};
		suggestions.interactionProfile = gazeProfile;
		suggestions.countSuggestedBindings = 1;
		suggestions.suggestedBindings = &gazeBinding;
		XR(xrSuggestInteractionProfileBindings(instance, &suggestions));
		const XrPath trackerUsers[]{trackerRole, trackerPersistent};
		aci.countSubactionPaths = 2;
		aci.subactionPaths = trackerUsers;
		std::strcpy(aci.actionName, "tracker_pose");
		std::strcpy(aci.localizedActionName, "Tracker Pose");
		XR(xrCreateAction(set, &aci, &trackerPose));
		aci.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
		std::strcpy(aci.actionName, "tracker_haptic");
		std::strcpy(aci.localizedActionName, "Tracker Haptic");
		XR(xrCreateAction(set, &aci, &trackerHaptic));
		char roleText[XR_MAX_PATH_LENGTH], persistentText[XR_MAX_PATH_LENGTH];
		XR(xrPathToString(instance, trackerRole, sizeof(roleText), &count, roleText));
		XR(xrPathToString(instance, trackerPersistent, sizeof(persistentText), &count, persistentText));
		std::vector<XrActionSuggestedBinding> deviceBindings;
		for (const char* user : {roleText, persistentText})
		{
			XrPath poseSource, hapticSource;
			XR(xrStringToPath(instance, (std::string(user) + "/input/grip/pose").c_str(), &poseSource));
			XR(xrStringToPath(instance, (std::string(user) + "/output/haptic").c_str(), &hapticSource));
			deviceBindings.push_back({trackerPose, poseSource});
			deviceBindings.push_back({trackerHaptic, hapticSource});
		}
		suggestions.interactionProfile = trackerProfile;
		suggestions.countSuggestedBindings = static_cast<uint32_t>(deviceBindings.size());
		suggestions.suggestedBindings = deviceBindings.data();
		XR(xrSuggestInteractionProfileBindings(instance, &suggestions));
	}
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &set;
	XR(xrAttachSessionActionSets(session, &attach));
	XrActionSpaceCreateInfo actionSpace{XR_TYPE_ACTION_SPACE_CREATE_INFO};
	actionSpace.action = pose;
	actionSpace.subactionPath = right;
	actionSpace.poseInActionSpace.orientation.w = 1;
	XrSpace hand;
	XR(xrCreateActionSpace(session, &actionSpace, &hand));
	XrSpace renderGaze = XR_NULL_HANDLE;
	XrReferenceSpaceCreateInfo renderGazeInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
											  nullptr,
											  XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO,
											  {{0, 0, 0, 1}, {0, 0, 0}}};
	if (foveatedMode)
	{
		XR(xrCreateReferenceSpace(session, &renderGazeInfo, &renderGaze));
		XR(xrEnumerateReferenceSpaces(session, 0, &count, nullptr));
		CHECK(count == 4);
	}
	else
		CHECK(xrCreateReferenceSpace(session, &renderGazeInfo, &renderGaze) == XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
	XrSpace gazeSpace = XR_NULL_HANDLE, trackerSpace = XR_NULL_HANDLE, persistentSpace = XR_NULL_HANDLE;
	XrPosef firstGaze{};
	if (devicesMode)
	{
		actionSpace.action = gazeAction;
		actionSpace.subactionPath = eyes;
		actionSpace.poseInActionSpace.position.x = 0.05f;
		XR(xrCreateActionSpace(session, &actionSpace, &gazeSpace));
		CHECK(xrCreateActionSpace(XR_NULL_HANDLE, &actionSpace, &persistentSpace) == XR_ERROR_HANDLE_INVALID);
		actionSpace.subactionPath = right;
		CHECK(xrCreateActionSpace(session, &actionSpace, &persistentSpace) == XR_ERROR_PATH_UNSUPPORTED);
		actionSpace.action = trackerPose;
		actionSpace.subactionPath = trackerRole;
		actionSpace.poseInActionSpace.position.x = 0;
		XR(xrCreateActionSpace(session, &actionSpace, &trackerSpace));
		actionSpace.subactionPath = trackerPersistent;
		XR(xrCreateActionSpace(session, &actionSpace, &persistentSpace));
		XrInteractionProfileState current{XR_TYPE_INTERACTION_PROFILE_STATE};
		XR(xrGetCurrentInteractionProfile(session, eyes, &current));
		CHECK(current.interactionProfile == XR_NULL_PATH);
		XR(xrGetCurrentInteractionProfile(session, trackerPersistent, &current));
		CHECK(current.interactionProfile == XR_NULL_PATH);
		bool profileChanged = false;
		PollXrEvents(instance, [&](const XrEventDataBuffer& event) {
			if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
				profileChanged |=
					reinterpret_cast<const XrEventDataInteractionProfileChanged*>(&event)->session == session;
		});
		CHECK(!profileChanged);
	}
	XrSwapchainCreateInfo chain{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	chain.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	const bool bgra = EnvOn("SIMXR_PROBE_BGRA");
	const bool srgb = EnvOn("SIMXR_PROBE_SRGB");
	const bool mutableFormat = bgra && srgb;
	if (mutableFormat)
	{
		CHECK(hasValidation);
		chain.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	}
	chain.format = bgra ? (srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM)
						: (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
	if (EnvOn("SIMXR_PROBE_HDR32"))
		chain.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	chain.sampleCount = chain.faceCount = chain.mipCount = 1;
	const uint32_t mipLevels = EnvOn("SIMXR_PROBE_MIPS") ? 4 : 1;
	chain.mipCount = mipLevels;
	chain.sampleCount = EnvOn("SIMXR_PROBE_MSAA") ? 4 : 1;
	chain.width = 64;
	chain.height = 48;
	chain.arraySize = activeViewCount;
	VkSampleCountFlags commonSamples = 127;
	for (const auto format :
		 {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
		  VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
	{
		VkImageFormatProperties supported{};
		VK(vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
													VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
														VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
														VK_IMAGE_USAGE_TRANSFER_DST_BIT,
													0, &supported));
		commonSamples &= supported.sampleCounts;
	}
	uint32_t maximumSamples = 1;
	while (commonSamples >>= 1)
		maximumSamples <<= 1;
	for (const auto& view : recommended)
		CHECK(view.recommendedSwapchainSampleCount == 1 && view.maxSwapchainSampleCount == maximumSamples);
	XrSwapchain rejectedSwapchain = XR_NULL_HANDLE;
	auto unsupported = chain;
	unsupported.createFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
	CHECK(xrCreateSwapchain(session, &unsupported, &rejectedSwapchain) == XR_ERROR_FEATURE_UNSUPPORTED &&
		  rejectedSwapchain == XR_NULL_HANDLE);
	unsupported = chain;
	unsupported.usageFlags |= XrSwapchainUsageFlags{1} << 63;
	CHECK(xrCreateSwapchain(session, &unsupported, &rejectedSwapchain) == XR_ERROR_FEATURE_UNSUPPORTED &&
		  rejectedSwapchain == XR_NULL_HANDLE);
	VkPhysicalDeviceProperties imageLimits{};
	vkGetPhysicalDeviceProperties(physical, &imageLimits);
	unsupported = chain;
	unsupported.width = imageLimits.limits.maxImageDimension2D + 1;
	CHECK(xrCreateSwapchain(session, &unsupported, &rejectedSwapchain) == XR_ERROR_VALIDATION_FAILURE &&
		  rejectedSwapchain == XR_NULL_HANDLE);
	unsupported = chain;
	unsupported.arraySize = imageLimits.limits.maxImageArrayLayers + 1;
	CHECK(xrCreateSwapchain(session, &unsupported, &rejectedSwapchain) == XR_ERROR_VALIDATION_FAILURE &&
		  rejectedSwapchain == XR_NULL_HANDLE);
	unsupported = chain;
	unsupported.sampleCount = 3;
	CHECK(xrCreateSwapchain(session, &unsupported, &rejectedSwapchain) == XR_ERROR_VALIDATION_FAILURE &&
		  rejectedSwapchain == XR_NULL_HANDLE);
	unsupported = chain;
	unsupported.mipCount = 8;
	CHECK(xrCreateSwapchain(session, &unsupported, &rejectedSwapchain) == XR_ERROR_VALIDATION_FAILURE &&
		  rejectedSwapchain == XR_NULL_HANDLE);
	uint32_t formatCount = 0;
	XR(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
	std::vector<int64_t> advertisedFormats(formatCount);
	XR(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, advertisedFormats.data()));
	for (const auto format :
		 {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
	{
		CHECK(std::find(advertisedFormats.begin(), advertisedFormats.end(), format) != advertisedFormats.end());
		auto hdr = chain;
		hdr.format = format;
		XrSwapchain hdrSwapchain = XR_NULL_HANDLE;
		XR(xrCreateSwapchain(session, &hdr, &hdrSwapchain));
		uint32_t imageCount = 0;
		XR(xrEnumerateSwapchainImages(hdrSwapchain, 0, &imageCount, nullptr));
		CHECK(imageCount == 3);
		std::vector<XrSwapchainImageVulkanKHR> hdrImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		XR(xrEnumerateSwapchainImages(hdrSwapchain, imageCount, &imageCount,
									  reinterpret_cast<XrSwapchainImageBaseHeader*>(hdrImages.data())));
		for (const auto& image : hdrImages)
			CHECK(image.image != VK_NULL_HANDLE);
		XR(xrDestroySwapchain(hdrSwapchain));
	}
	XrSwapchain swapchain;
	XR(xrCreateSwapchain(session, &chain, &swapchain));
	XR(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr));
	CHECK(count == 3);
	std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
	CHECK(xrEnumerateSwapchainImages(swapchain, 0, nullptr, nullptr) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(xrEnumerateSwapchainImages(swapchain, count, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE);
	auto invalidImages = images;
	invalidImages[1].type = XR_TYPE_UNKNOWN;
	CHECK(xrEnumerateSwapchainImages(swapchain, count, &count,
									 reinterpret_cast<XrSwapchainImageBaseHeader*>(invalidImages.data())) ==
			  XR_ERROR_VALIDATION_FAILURE &&
		  invalidImages[0].image == VK_NULL_HANDLE);
	XR(xrEnumerateSwapchainImages(swapchain, count, &count,
								  reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
	VkRenderPass mutablePass = VK_NULL_HANDLE;
	std::vector<VkImageView> mutableViews;
	std::vector<VkFramebuffer> mutableTargets;
	if (mutableFormat)
	{
		CHECK(chain.format == VK_FORMAT_B8G8R8A8_SRGB);
		VkAttachmentDescription attachment{};
		attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
		attachment.samples = static_cast<VkSampleCountFlagBits>(chain.sampleCount);
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color;
		VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass.attachmentCount = 1;
		pass.pAttachments = &attachment;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		VK(vkCreateRenderPass(device, &pass, nullptr, &mutablePass));
		mutableViews.resize(images.size());
		mutableTargets.resize(images.size());
		for (std::size_t image = 0; image < images.size(); ++image)
		{
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = images[image].image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = VK_FORMAT_B8G8R8A8_UNORM;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			VK(vkCreateImageView(device, &view, nullptr, &mutableViews[image]));
			VkFramebufferCreateInfo target{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			target.renderPass = mutablePass;
			target.attachmentCount = 1;
			target.pAttachments = &mutableViews[image];
			target.width = chain.width;
			target.height = chain.height;
			target.layers = 1;
			VK(vkCreateFramebuffer(device, &target, nullptr, &mutableTargets[image]));
		}
	}
	// Also exercise depth swapchain allocation and exact acquire/wait/release ordering.
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
	if (const auto* selected = std::getenv("SIMXR_PROBE_DEPTH_FORMAT"))
	{
		if (std::strcmp(selected, "d16") == 0)
			depthFormat = VK_FORMAT_D16_UNORM;
		else if (std::strcmp(selected, "d24s8") == 0)
			depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
		else if (std::strcmp(selected, "d32s8") == 0)
			depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
		else
			CHECK(std::strcmp(selected, "d32") == 0);
	}
	const VkImageAspectFlags depthAspect =
		VK_IMAGE_ASPECT_DEPTH_BIT |
		((depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
			 ? VK_IMAGE_ASPECT_STENCIL_BIT
			 : 0);
	chain.format = depthFormat;
	const bool multisampledDepth = depthMode && EnvOn("SIMXR_PROBE_DEPTH_MSAA");
	chain.sampleCount = multisampledDepth ? 4 : 1;
	chain.mipCount = 1;
	chain.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	XrSwapchain depth;
	XR(xrCreateSwapchain(session, &chain, &depth));
	std::vector<XrSwapchainImageVulkanKHR> depthImages;
	VkBuffer depthUpload = VK_NULL_HANDLE;
	VkDeviceMemory depthMemory = VK_NULL_HANDLE;
	void* depthPixels = nullptr;
	if (depthMode)
	{
		XR(xrEnumerateSwapchainImages(depth, 0, &count, nullptr));
		depthImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		XR(xrEnumerateSwapchainImages(depth, count, &count,
									  reinterpret_cast<XrSwapchainImageBaseHeader*>(depthImages.data())));
		VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer.size = 64 * 48 * activeViewCount * sizeof(float);
		buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VK(vkCreateBuffer(device, &buffer, nullptr, &depthUpload));
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(device, depthUpload, &requirements);
		VkPhysicalDeviceMemoryProperties memory;
		vkGetPhysicalDeviceMemoryProperties(physical, &memory);
		uint32_t type = 0;
		constexpr auto properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		while (type < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1U << type)) ||
												 (memory.memoryTypes[type].propertyFlags & properties) != properties))
			++type;
		CHECK(type < memory.memoryTypeCount);
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = type;
		VK(vkAllocateMemory(device, &allocation, nullptr, &depthMemory));
		VK(vkBindBufferMemory(device, depthUpload, depthMemory, 0));
		void* mapped = nullptr;
		VK(vkMapMemory(device, depthMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
		depthPixels = mapped;
	}
	else
		XR(xrDestroySwapchain(depth));
	XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	CHECK(xrReleaseSwapchainImage(swapchain, &release) == XR_ERROR_CALL_ORDER_INVALID);
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex = family;
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool;
	VK(vkCreateCommandPool(device, &pci, nullptr, &pool));
	VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = pool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VK(vkAllocateCommandBuffers(device, &alloc, &cmd));
	VkSemaphoreTypeCreateInfo semaphoreType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	semaphoreInfo.pNext = &semaphoreType;
	VkSemaphore deferredSemaphore;
	VK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &deferredSemaphore));
	auto releaseDependency = [&] {
		VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		signal.semaphore = deferredSemaphore;
		signal.value = 1;
		return vkSignalSemaphore(device, &signal);
	};
	XrFrameState loadingState{XR_TYPE_FRAME_STATE};
	XrFrameWaitInfo loadingWait{XR_TYPE_FRAME_WAIT_INFO};
	XR(xrWaitFrame(session, &loadingWait, &loadingState));
	auto overlappingWait = std::async(std::launch::async, [session] {
		XrFrameState state{XR_TYPE_FRAME_STATE};
		XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
		auto result = xrWaitFrame(session, &wait, &state);
		return std::make_pair(result, state);
	});
	CHECK(overlappingWait.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
	XrFrameBeginInfo loadingBegin{XR_TYPE_FRAME_BEGIN_INFO};
	XR(xrBeginFrame(session, &loadingBegin));
	CHECK(overlappingWait.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	auto nextLoading = overlappingWait.get();
	XR(nextLoading.first);
	CHECK(nextLoading.second.predictedDisplayTime > loadingState.predictedDisplayTime);
	XrFrameEndInfo loadingEnd{XR_TYPE_FRAME_END_INFO};
	loadingEnd.displayTime = loadingState.predictedDisplayTime;
	loadingEnd.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	XR(xrEndFrame(session, &loadingEnd));
	XR(xrBeginFrame(session, &loadingBegin));
	loadingEnd.displayTime = nextLoading.second.predictedDisplayTime;
	XR(xrEndFrame(session, &loadingEnd));
	namespace wire = control;
	namespace tag = control::tag;
	const auto* selectedEndpoint = std::getenv("SIMXR_CONTROL_SOCKET");
	const auto endpoint = selectedEndpoint && *selectedEndpoint
							  ? std::string(selectedEndpoint)
							  : (data / ("control." + std::to_string(getpid()) + ".sock")).string();
	wire::LocalControlClient controls(endpoint);
	const auto exchange = [&](wire::OpCode operation, std::vector<wire::Field> fields = {}) {
		wire::ControlPacket request;
		request.message.opcode = static_cast<std::uint16_t>(operation);
		request.message.fields = std::move(fields);
		auto response = controls.Request(std::move(request)).message;
		const auto* error = response.Find(tag::Error);
		CHECK(error && error->AsU32() == 0);
		return response;
	};
	exchange(wire::OpCode::Hello, {wire::Field::Text(tag::ClientName, "XR probe"),
								   wire::Field::U64(tag::RequiredCapabilities,
													static_cast<std::uint64_t>(wire::Capability::SimulationCommands))});
	const auto sessionScope = [&] {
		const auto sessions = exchange(wire::OpCode::Sessions);
		CHECK(sessions.Find(tag::Sessions));
		const auto entries = wire::DecodeFields(sessions.Find(tag::Sessions)->value);
		CHECK(entries.error == wire::DecodeError::None && entries.fields.size() == 1);
		auto identity = wire::DecodeFields(entries.fields.front().value);
		CHECK(identity.error == wire::DecodeError::None);
		wire::Message scope;
		scope.fields = std::move(identity.fields);
		CHECK(scope.Find(tag::Session) && scope.Find(tag::Generation));
		const auto session = scope.Find(tag::Session)->AsU64();
		const auto generation = scope.Find(tag::Generation)->AsU64();
		CHECK(session && *session && generation && *generation);
		return std::pair{*session, *generation};
	};
	const auto [sessionId, generation] = sessionScope();
	const auto readPreview = [&](std::uint64_t frame) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		wire::Message retained;
		while (true)
		{
			wire::ControlPacket request;
			request.message.opcode = static_cast<std::uint16_t>(wire::OpCode::RetainPreview);
			request.message.fields = {wire::Field::U64(tag::Session, sessionId),
									  wire::Field::U64(tag::Generation, generation),
									  wire::Field::U64(tag::FrameId, frame)};
			retained = controls.Request(std::move(request)).message;
			CHECK(retained.Find(tag::Error));
			const auto error = retained.Find(tag::Error)->AsU32();
			if (error == 0)
				break;
			CHECK(error == static_cast<std::uint32_t>(wire::ErrorCode::Busy) ||
				  error == static_cast<std::uint32_t>(wire::ErrorCode::NotFound));
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		std::array<std::vector<std::byte>, 2> eyes;
		CHECK(retained.Find(tag::ImageAvailable));
		if (retained.Find(tag::ImageAvailable)->AsBool() == false)
			return eyes;
		CHECK(retained.Find(tag::ImageReaderId));
		const auto reader = retained.Find(tag::ImageReaderId)->AsU64();
		CHECK(reader);
		const auto info = exchange(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, *reader)});
		CHECK(info.Find(tag::ImageDescriptor));
		const auto image = wire::DecodeImageDescription(*info.Find(tag::ImageDescriptor));
		CHECK(image.format == VK_FORMAT_R8G8B8A8_SRGB && image.arrayLayers == 2);
		const auto region = wire::Field::Bytes(
			tag::ImageRegion,
			wire::EncodeFields(std::array{wire::Field::U32(1, 0), wire::Field::U32(2, 0),
										  wire::Field::U32(3, image.width), wire::Field::U32(4, image.height),
										  wire::Field::U32(5, 64), wire::Field::U32(6, 48)}));
		for (std::uint32_t eye = 0; eye < eyes.size(); ++eye)
		{
			const auto started = exchange(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, *reader),
																	  wire::Field::U32(tag::Layer, eye), region});
			CHECK(started.Find(tag::ExportId) && started.Find(tag::TotalBytes));
			const auto ticket = started.Find(tag::ExportId)->AsU64();
			CHECK(ticket && started.Find(tag::TotalBytes)->AsU64() == 64 * 48 * 4);
			while (true)
			{
				const auto status = exchange(wire::OpCode::ExportInfo, {wire::Field::U64(tag::ExportId, *ticket)});
				CHECK(status.Find(tag::Phase));
				const auto phase = status.Find(tag::Phase)->AsText();
				if (phase == "complete")
					break;
				CHECK(phase == "pending" && std::chrono::steady_clock::now() < deadline);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			auto pixels = exchange(wire::OpCode::ReadExport,
								   {wire::Field::U64(tag::ExportId, *ticket), wire::Field::U64(tag::Offset, 0),
									wire::Field::U64(tag::Length, 64 * 48 * 4)});
			CHECK(pixels.Find(tag::Bytes) && pixels.Find(tag::Bytes)->value.size() == 64 * 48 * 4);
			eyes[eye] = std::move(pixels.Find(tag::Bytes)->value);
			exchange(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, *ticket)});
		}
		exchange(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, *reader)});
		return eyes;
	};
	CHECK(readPreview(2)[0].empty());
	const auto emptyProjectionHistory = [&](std::uint64_t session, std::uint64_t generation) {
		const auto response =
			exchange(wire::OpCode::ReadProjectionHistory,
					 {wire::Field::U64(tag::Session, session), wire::Field::U64(tag::Generation, generation)});
		CHECK(response.Find(tag::Document));
		auto decoded = wire::DecodeFields(response.Find(tag::Document)->value);
		CHECK(decoded.error == wire::DecodeError::None);
		wire::Message document;
		document.fields = std::move(decoded.fields);
		CHECK(document.Find(3));
		const auto members = wire::DecodeFields(document.Find(3)->value);
		CHECK(members.error == wire::DecodeError::None);
		const auto empty = wire::InspectionValue::Array({}).Encode("entries");
		return std::ranges::any_of(members.fields, [&](const auto& member) { return member.value == empty; });
	};
	for (std::uint64_t frame = 0; frame < (interactive ? 5u : 4u); ++frame)
	{
		std::vector<wire::Field> commands;
		const auto add = [&](const SimulationCommand& command) {
			commands.push_back(wire::EncodeSimulationCommand(command, static_cast<std::uint16_t>(commands.size() + 1)));
		};
		if (!frame)
		{
			add(TakeControl{ControlChannel::RightControls});
			ControllerControls input;
			input.trigger = .75f;
			input.stick.x = -1;
			add(SetControllerControls{1, input});
		}
		if (frame == 4)
		{
			add(ReleaseControl{ControlChannel::RightControls});
			if (!EnvOn("SIMXR_PROBE_CANONICAL_SEQUENCE"))
				for (const auto channel : {ControlChannel::Head, ControlChannel::LeftPose, ControlChannel::RightPose})
					add(ReleaseControl{channel});
		}
		else if (!EnvOn("SIMXR_PROBE_CANONICAL_SEQUENCE"))
		{
			if (!frame)
				add(TakeControl{ControlChannel::Head});
			PoseSample pose;
			pose.pose = {EulerAngles(.2f, -.1f, .15f), {.3f + .1f * static_cast<float>(frame), 1.8f, -.4f}};
			pose.flags = TrackedPoseFlags;
			add(SetHeadPose{pose});
			for (std::uint32_t hand = 0; hand < 2; ++hand)
			{
				if (!frame)
					add(TakeControl{hand ? ControlChannel::RightPose : ControlChannel::LeftPose});
				ControllerPose controller;
				controller.pose = pose;
				controller.pose.pose = Compose(pose.pose, {{0, 0, 0, 1}, {hand ? .2f : -.2f, -.3f, -.4f}});
				add(SetControllerPose{hand, controller});
			}
		}
		if (!commands.empty())
			exchange(wire::OpCode::SubmitSimulation,
					 {wire::Field::U64(tag::Session, sessionId), wire::Field::U64(tag::Generation, generation),
					  wire::Field::U64(tag::TransactionId, frame + 1), wire::Field::U64(tag::ApplyFrame, frame + 3),
					  wire::Field::U32(tag::SourceKind, 0), wire::Field::U32(tag::LatePolicy, 0),
					  wire::Field::Bytes(tag::Commands, wire::EncodeFields(commands))});
	}
	XrTime firstTime = 0;
	XrTime prior = 0;
	std::future<std::pair<XrResult, XrFrameState>> nextWait;
	const bool threaded = EnvOn("SIMXR_PROBE_THREADED");
	// Every Nth frame renders 40 ms late, so frame timing views have misses to show.
	if (EnvOn("SIMXR_PROBE_CONTROL"))
	{
		WriteText(data / "control.ready", "ready\n");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (!std::filesystem::exists(data / "control.continue"))
		{
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	for (int frame = 0; frame < frames; ++frame)
	{
		if (interactive)
		{
			PollXrEvents(instance, [&](const XrEventDataBuffer& event) {
				if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
					std::printf("Interactive session state: %d\n",
								reinterpret_cast<const XrEventDataSessionStateChanged*>(&event)->state);
				if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
					std::puts("Interactive reference space changed");
			});
		}
		XrFrameState fs{XR_TYPE_FRAME_STATE};
		XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
		if (nextWait.valid())
		{
			auto completedWait = nextWait.get();
			XR(completedWait.first);
			fs = completedWait.second;
		}
		else
			XR(xrWaitFrame(session, &wait, &fs));
		CHECK((interactive || fs.shouldRender) && fs.predictedDisplayTime > prior);
		auto previousDisplay = prior;
		prior = fs.predictedDisplayTime;
		if (frame == 0)
			firstTime = prior;
		XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
		XR(xrBeginFrame(session, &bi));
		if (slowEvery && frame % slowEvery == slowEvery - 1)
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
		if (EnvOn("SIMXR_PROBE_RENDER_GATE"))
		{
			const auto gate = "rendering." + std::to_string(frame + 3);
			WriteText(data / (gate + ".ready"),
					  std::to_string(fs.predictedDisplayPeriod) + " " + std::to_string(fs.predictedDisplayTime));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!std::filesystem::exists(data / (gate + ".continue")))
			{
				CHECK(std::chrono::steady_clock::now() < deadline);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		XrActiveActionSet active{set, XR_NULL_PATH};
		XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		const auto syncResult = xrSyncActions(session, &sync);
		CHECK(syncResult == XR_SUCCESS || (interactive && syncResult == XR_SESSION_NOT_FOCUSED));
		if (frame == 0)
		{
			XrHapticActionInfo target{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, haptic, right};
			XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_MIN_HAPTIC_DURATION, 0, 0.5f};
			XR(xrApplyHapticFeedback(session, &target, reinterpret_cast<const XrHapticBaseHeader*>(&pulse)));
			XR(xrStopHapticFeedback(session, &target));
			target.action = trigger;
			CHECK(xrApplyHapticFeedback(session, &target, reinterpret_cast<const XrHapticBaseHeader*>(&pulse)) ==
				  XR_ERROR_ACTION_TYPE_MISMATCH);
		}
		XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
		get.action = trigger;
		get.subactionPath = right;
		XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
		XR(xrGetActionStateFloat(session, &get, &value));
		if (!interactive || frame < 4)
		{
			CHECK(value.isActive);
			CHECK(std::fabs(value.currentState - .75f) < .0001f);
			CHECK(!value.changedSinceLastSync);
		}
		XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
		loc.next = &velocity;
		XR(xrLocateSpace(head, stage, fs.predictedDisplayTime, &loc));
		auto locatedHead = loc.pose;
		if (!interactive || frame < 4)
			CHECK(std::fabs(loc.pose.position.x - (.3f + .1f * std::min(frame, 3))) < .0001f &&
				  std::fabs(loc.pose.position.y - 1.8f) < .0001f);
		if (foveatedMode && (frame == 3 || frame == 4))
			CHECK(!(velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT));
		else if (frame > 0 && frame < 4)
		{
			CHECK((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0);
			CHECK(std::fabs(velocity.linearVelocity.x - .1f * 1.e9f / static_cast<float>(prior - previousDisplay)) <
				  .001f);
		}
		if (frame > 0 && frame < 120)
		{
			XR(xrLocateSpace(head, stage, firstTime, &loc));
			CHECK(std::fabs(loc.pose.position.x - .3f) < .0001f);
		}
		if (devicesMode && frame < 4)
		{
			if (!frame)
			{
				XrInteractionProfileState current{XR_TYPE_INTERACTION_PROFILE_STATE};
				XR(xrGetCurrentInteractionProfile(session, eyes, &current));
				CHECK(current.interactionProfile == gazeProfile);
				XR(xrGetCurrentInteractionProfile(session, trackerPersistent, &current));
				CHECK(current.interactionProfile == trackerProfile);
				bool profileChanged = false;
				PollXrEvents(instance, [&](const XrEventDataBuffer& event) {
					if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
						profileChanged |=
							reinterpret_cast<const XrEventDataInteractionProfileChanged*>(&event)->session == session;
				});
				CHECK(profileChanged);
			}
			XrActionStateGetInfo deviceGet{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, gazeAction, eyes};
			XrActionStatePose activePose{XR_TYPE_ACTION_STATE_POSE};
			XR(xrGetActionStatePose(session, &deviceGet, &activePose));
			CHECK(activePose.isActive);
			deviceGet.action = trackerPose;
			deviceGet.subactionPath = trackerPersistent;
			XR(xrGetActionStatePose(session, &deviceGet, &activePose));
			CHECK(activePose.isActive);
			XrEyeGazeSampleTimeEXT sampleTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT, nullptr, -1};
			XrSpaceVelocity gazeVelocity{XR_TYPE_SPACE_VELOCITY, &sampleTime};
			XrSpaceLocation gazeLocation{XR_TYPE_SPACE_LOCATION, &gazeVelocity};
			XR(xrLocateSpace(gazeSpace, stage, fs.predictedDisplayTime, &gazeLocation));
			CHECK(sampleTime.time == fs.predictedDisplayTime && gazeLocation.next == &gazeVelocity &&
				  gazeVelocity.next == &sampleTime);
			CHECK((gazeLocation.locationFlags &
				   (XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) ==
				  (XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT));
			if (!frame)
				firstGaze = gazeLocation.pose;
			XR(xrLocateSpace(gazeSpace, stage, fs.predictedDisplayTime + fs.predictedDisplayPeriod, &gazeLocation));
			CHECK(sampleTime.time == fs.predictedDisplayTime);
			if (frame)
			{
				XR(xrLocateSpace(gazeSpace, stage, firstTime, &gazeLocation));
				CHECK(sampleTime.time == firstTime &&
					  std::fabs(gazeLocation.pose.position.x - firstGaze.position.x) < .0001f);
			}
			sampleTime.time = -1;
			XR(xrLocateSpace(head, stage, fs.predictedDisplayTime, &gazeLocation));
			CHECK(sampleTime.time == 0);
			XrSpaceLocation roleLocation{XR_TYPE_SPACE_LOCATION}, persistentLocation{XR_TYPE_SPACE_LOCATION};
			XR(xrLocateSpace(trackerSpace, stage, fs.predictedDisplayTime, &roleLocation));
			XR(xrLocateSpace(persistentSpace, stage, fs.predictedDisplayTime, &persistentLocation));
			CHECK((roleLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
				  std::fabs(roleLocation.pose.position.x - persistentLocation.pose.position.x) < .0001f &&
				  std::fabs(roleLocation.pose.position.y - persistentLocation.pose.position.y) < .0001f);
			if (!frame)
			{
				XrHapticActionInfo target{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, trackerHaptic, trackerRole};
				XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_INFINITE_DURATION,
										XR_FREQUENCY_UNSPECIFIED, 0.6f};
				XR(xrApplyHapticFeedback(session, &target, reinterpret_cast<const XrHapticBaseHeader*>(&pulse)));
				target.subactionPath = trackerPersistent;
				XR(xrStopHapticFeedback(session, &target));
				target.action = gazeAction;
				target.subactionPath = eyes;
				CHECK(xrApplyHapticFeedback(session, &target, reinterpret_cast<const XrHapticBaseHeader*>(&pulse)) ==
					  XR_ERROR_ACTION_TYPE_MISMATCH);
			}
		}
		if (bodyTracking && frame < 4)
		{
			XrBodyJointsLocateInfoFB bodyInfo{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB};
			bodyInfo.baseSpace = stage;
			bodyInfo.time = fs.predictedDisplayTime;
			std::vector<XrBodyJointLocationFB> joints(XR_FULL_BODY_JOINT_COUNT_META);
			XrBodyTrackingFidelityStatusMETA fidelity{XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META};
			XrBodyTrackingCalibrationStatusMETA calibration{XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META};
			fidelity.next = &calibration;
			XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB};
			locations.next = &fidelity;
			locations.jointCount = static_cast<uint32_t>(joints.size());
			locations.jointLocations = joints.data();
			XR(locateBody(body, &bodyInfo, &locations));
			CHECK(locations.isActive && locations.time == fs.predictedDisplayTime &&
				  locations.skeletonChangedCount > 0);
			CHECK(fidelity.fidelity == XR_BODY_TRACKING_FIDELITY_HIGH_META &&
				  calibration.status == XR_BODY_TRACKING_CALIBRATION_STATE_VALID_META);
			CHECK(std::fabs(joints[XR_FULL_BODY_JOINT_HEAD_META].pose.position.x - locatedHead.position.x) < .0001f);
			std::vector<XrBodySkeletonJointFB> skeleton(XR_FULL_BODY_JOINT_COUNT_META);
			XrBodySkeletonFB skeletonInfo{XR_TYPE_BODY_SKELETON_FB};
			skeletonInfo.jointCount = static_cast<uint32_t>(skeleton.size());
			skeletonInfo.joints = skeleton.data();
			XR(skeletonBody(body, &skeletonInfo));
			CHECK(skeleton[XR_FULL_BODY_JOINT_HEAD_META].joint == XR_FULL_BODY_JOINT_HEAD_META);
			locations.jointCount = XR_BODY_JOINT_COUNT_FB;
			XR(locateBody(upperBody, &bodyInfo, &locations));
			CHECK(locations.isActive);
			skeletonInfo.jointCount = XR_BODY_JOINT_COUNT_FB;
			XR(skeletonBody(upperBody, &skeletonInfo));
			CHECK(skeleton[XR_BODY_JOINT_HEAD_FB].joint == XR_BODY_JOINT_HEAD_FB);
			std::vector<XrBodyJointLocationBD> bdJoints(XR_BODY_JOINT_COUNT_BD);
			XrBodyJointLocationsBD bdLocations{XR_TYPE_BODY_JOINT_LOCATIONS_BD};
			bdLocations.jointLocationCount = static_cast<uint32_t>(bdJoints.size());
			bdLocations.jointLocations = bdJoints.data();
			XrBodyJointsLocateInfoBD bdInfo{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD};
			bdInfo.baseSpace = stage;
			bdInfo.time = fs.predictedDisplayTime;
			XR(locateBodyBD(bodyBD, &bdInfo, &bdLocations));
			CHECK((bdJoints[XR_BODY_JOINT_HEAD_BD].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
			CHECK(std::fabs(bdJoints[XR_BODY_JOINT_HEAD_BD].pose.position.x - locatedHead.position.x) < .0001f);
			if (frame > 0)
			{
				bodyInfo.time = firstTime;
				locations.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
				XR(locateBody(body, &bodyInfo, &locations));
				CHECK(std::fabs(joints[XR_FULL_BODY_JOINT_HEAD_META].pose.position.x - .3f) < .0001f);
			}
		}
		XR(xrLocateSpace(hand, head, fs.predictedDisplayTime, &loc));
		if (!interactive || frame < 4)
			CHECK(std::fabs(loc.pose.position.x - .2f) < .0001f && std::fabs(loc.pose.position.z + .4f) < .0001f);
		XrViewLocateFoveatedRenderingVARJO foveated{XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO, nullptr,
													static_cast<XrBool32>(frame == 1 ? XR_FALSE : XR_TRUE)};
		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
		if (foveatedMode)
		{
			locate.next = &foveated;
			XrSpaceLocation renderingGaze{XR_TYPE_SPACE_LOCATION};
			XR(xrLocateSpace(renderGaze, head, fs.predictedDisplayTime, &renderingGaze));
			CHECK((renderingGaze.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) ==
				  (frame == 3 ? 0 : XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT));
		}
		locate.viewConfigurationType = activeViewType;
		locate.displayTime = fs.predictedDisplayTime;
		locate.space = head;
		XrViewState vs{XR_TYPE_VIEW_STATE};
		std::vector<XrView> views(activeViewCount, {XR_TYPE_VIEW});
		XR(xrLocateViews(session, &locate, &vs, 0, &count, nullptr));
		CHECK(count == activeViewCount);
		CHECK(xrLocateViews(session, &locate, &vs, activeViewCount - 1, &count, views.data()) ==
			  XR_ERROR_SIZE_INSUFFICIENT);
		XR(xrLocateViews(session, &locate, &vs, activeViewCount, &count, views.data()));
		CHECK(count == activeViewCount);
		if (!interactive)
			CHECK(std::fabs(views[1].pose.position.x - views[0].pose.position.x - .064f) < .0001f);
		else
		{
			std::ostringstream state;
			state << "{\"frame\":" << frame << ",\"headX\":" << locatedHead.position.x
				  << ",\"trigger\":" << value.currentState << ",\"handX\":" << loc.pose.position.x
				  << ",\"viewSpan\":" << views[1].pose.position.x - views[0].pose.position.x
				  << ",\"leftFovRight\":" << views[0].fov.angleRight << '}';
			WriteText(data / "probe_state.json.tmp", state.str());
			std::filesystem::rename(data / "probe_state.json.tmp", data / "probe_state.json");
		}
		if (quadMode)
		{
			for (uint32_t eye = 0; eye < 2; ++eye)
			{
				CHECK(std::memcmp(&views[eye].pose, &views[eye + 2].pose, sizeof(XrPosef)) == 0);
				CHECK(views[eye + 2].fov.angleLeft >= views[eye].fov.angleLeft &&
					  views[eye + 2].fov.angleRight <= views[eye].fov.angleRight &&
					  views[eye + 2].fov.angleDown >= views[eye].fov.angleDown &&
					  views[eye + 2].fov.angleUp <= views[eye].fov.angleUp);
			}
			auto inactive = locate;
			inactive.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			CHECK(xrLocateViews(session, &inactive, &vs, 0, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE);
		}
		uint32_t index;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		XR(xrAcquireSwapchainImage(swapchain, &acquire, &index));
		XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wi.timeout = XR_INFINITE_DURATION;
		XR(xrWaitSwapchainImage(swapchain, &wi));
		uint32_t depthIndex = 0;
		if (depthMode)
		{
			XR(xrAcquireSwapchainImage(depth, &acquire, &depthIndex));
			XR(xrWaitSwapchainImage(depth, &wi));
			for (uint32_t eye = 0; eye < activeViewCount; ++eye)
				for (uint32_t y = 0; y < 48; ++y)
					for (uint32_t x = 0; x < 64; ++x)
					{
						const auto pixel = (eye * 48 + y) * 64 + x;
						const auto value = .1f * (frame % 4 + 1) + .15f * eye + .05f * (x / 16) + .025f * (y / 12);
						if (depthFormat == VK_FORMAT_D16_UNORM)
							static_cast<uint16_t*>(depthPixels)[pixel] =
								static_cast<uint16_t>(std::lround(value * 65535));
						else if (depthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
							static_cast<uint32_t*>(depthPixels)[pixel] =
								static_cast<uint32_t>(std::lround(value * 16777215));
						else
							static_cast<float*>(depthPixels)[pixel] = value;
					}
		}
		VK(vkResetCommandPool(device, pool, 0));
		VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		VK(vkBeginCommandBuffer(cmd, &cb));
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.image = images[index].image;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, activeViewCount};
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
							 nullptr, 1, &barrier);
		VkClearColorValue colors[4] = {{{1, 0, 0, 1}}, {{0, 0, 1, 1}}, {{0, 1, 0, 1}}, {{1, 1, 0, 1}}};
		if (EnvOn("SIMXR_PROBE_MIDTONE"))
			colors[0] = {{0.25f, 0.5f, 0.75f, 1.0f}};

		if (EnvOn("SIMXR_PROBE_DYNAMIC_COLOR") && frame % 2)
			colors[0] = colors[2];
		for (uint32_t eye = 0; eye < activeViewCount; ++eye)
		{
			const bool minify = mipLevels > 1 && EnvOn("SIMXR_PROBE_MIP_MINIFY");
			VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, minify ? 1U : mipLevels, eye, 1};
			vkCmdClearColorImage(cmd, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colors[eye], 1,
								 &range);
			if (minify)
			{
				range.baseMipLevel = 1;
				range.levelCount = mipLevels - 1;
				const VkClearColorValue minified = eye == 0 ? colors[2] : colors[3];
				vkCmdClearColorImage(cmd, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &minified, 1,
									 &range);
			}
		}
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
							 nullptr, 0, nullptr, 1, &barrier);
		if (mutableFormat)
		{
			VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			begin.renderPass = mutablePass;
			begin.framebuffer = mutableTargets[index];
			begin.renderArea.extent = {64, 48};
			vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
			VkClearAttachment clear{};
			clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			clear.clearValue.color = {{.25f, .5f, .75f, 1}};
			const VkClearRect rectangle{{{8, 20}, {8, 8}}, 0, 1};
			vkCmdClearAttachments(cmd, 1, &clear, 1, &rectangle);
			vkCmdEndRenderPass(cmd);
		}
		if (depthMode)
		{
			VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
			upload.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &upload, 0,
								 nullptr, 0, nullptr);
			auto depthBarrier = barrier;
			depthBarrier.image = depthImages[depthIndex].image;
			depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			depthBarrier.subresourceRange.aspectMask = depthAspect;
			depthBarrier.subresourceRange.levelCount = 1;
			depthBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			depthBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
								 0, nullptr, 1, &depthBarrier);
			VkBufferImageCopy copy{};
			copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, activeViewCount};
			copy.imageExtent = {64, 48, 1};
			if (multisampledDepth)
			{
				for (uint32_t eye = 0; eye < activeViewCount; ++eye)
				{
					const VkClearDepthStencilValue value{.1f * (frame % 4 + 1) + .15f * eye, 0};
					const VkImageSubresourceRange range{depthAspect, 0, 1, eye, 1};
					vkCmdClearDepthStencilImage(cmd, depthImages[depthIndex].image,
												VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
				}
			}
			else
				vkCmdCopyBufferToImage(cmd, depthUpload, depthImages[depthIndex].image,
									   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			depthBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			depthBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
								 0, nullptr, 1, &depthBarrier);
		}
		VK(vkEndCommandBuffer(cmd));
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		// Model hosts releasing an earlier GPU timeline wait after xrEndFrame.
		uint64_t dependencyValue = 1;
		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkTimelineSemaphoreSubmitInfo timelineSubmit{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		if (frame == 2)
		{
			timelineSubmit.waitSemaphoreValueCount = 1;
			timelineSubmit.pWaitSemaphoreValues = &dependencyValue;
			submit.pNext = &timelineSubmit;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &deferredSemaphore;
			submit.pWaitDstStageMask = &waitStage;
		}
		VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
		if (frame != 2)
			VK(vkQueueWaitIdle(queue));
		XR(xrReleaseSwapchainImage(swapchain, &release));
		if (depthMode)
			XR(xrReleaseSwapchainImage(depth, &release));
		if (EnvOn("SIMXR_PROBE_WORLD_PROJECTION"))
		{
			locate.space = stage;
			XR(xrLocateViews(session, &locate, &vs, activeViewCount, &count, views.data()));
		}
		std::vector<XrCompositionLayerDepthInfoKHR> depths(activeViewCount, {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
		std::vector<XrCompositionLayerProjectionView> pv(activeViewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
		for (uint32_t e = 0; e < activeViewCount; ++e)
		{
			pv[e].pose = views[e].pose;
			pv[e].fov = views[e].fov;
			pv[e].subImage = {swapchain, {{0, 0}, {64, 48}}, e};
			if (depthMode)
			{
				depths[e].subImage = {depth, {{1, 1}, {62, 46}}, e};
				depths[e].minDepth = 0;
				depths[e].maxDepth = 1;
				depths[e].nearZ = frame == 2 ? 10.f : .1f;
				depths[e].farZ = frame >= 3 ? std::numeric_limits<float>::infinity() : (frame == 2 ? .1f : 10.f);
				pv[e].next = &depths[e];
			}
		}
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		projection.space = EnvOn("SIMXR_PROBE_WORLD_PROJECTION") ? stage : head;
		projection.viewCount = activeViewCount;
		projection.views = pv.data();
		XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
		quad.space = head;
		quad.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
		quad.pose.orientation.w = 1;
		quad.pose.position.z = -2;
		if (EnvOn("SIMXR_PROBE_MOVING_QUAD"))
			quad.pose.position.x = frame % 2 ? 0.25f : -0.25f;
		quad.size = {1, 1};
		quad.subImage = {swapchain, {{16, 12}, {32, 24}}, 0};
		const XrCompositionLayerBaseHeader* submitted[] = {(const XrCompositionLayerBaseHeader*)&projection,
														   (const XrCompositionLayerBaseHeader*)&quad};
		XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
		end.displayTime = fs.predictedDisplayTime;
		end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end.layerCount = quadMode ? 1 : 2;
		end.layers = submitted;
		if (clearFrame == frame + 3)
			end.layerCount = 0;
		std::atomic<bool> dependencyReleased{false};
		VkResult dependencyResult = VK_SUCCESS;
		std::jthread watchdog;
		if (frame == 2)
			watchdog = std::jthread([&] {
				for (int i = 0; i < 1000 && !dependencyReleased.load(); ++i)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				if (!dependencyReleased.exchange(true))
					dependencyResult = releaseDependency();
			});
		if (threaded && frame >= 4 && frame + 1 < frames)
			nextWait = std::async(std::launch::async, [session] {
				XrFrameState state{XR_TYPE_FRAME_STATE};
				XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
				auto result = xrWaitFrame(session, &wait, &state);
				return std::make_pair(result, state);
			});
		if (quadMode && frame == 2)
		{
			projection.viewCount = 2;
			CHECK(xrEndFrame(session, &end) == XR_ERROR_LAYER_INVALID);
			projection.viewCount = activeViewCount;
		}
		auto endStart = std::chrono::steady_clock::now();
		auto ended = frame == 0 && EnvOn("SIMXR_PROBE_NATIVE_ADMISSION") ? CheckNativeAdmission(instance, session, end)
																		 : xrEndFrame(session, &end);
		auto endElapsed = std::chrono::steady_clock::now() - endStart;
		if (frame == 2)
		{
			if (!dependencyReleased.exchange(true))
				dependencyResult = releaseDependency();
			watchdog.join();
			VK(dependencyResult);
			CHECK(endElapsed < std::chrono::milliseconds(500));
			VK(vkQueueWaitIdle(queue));
		}
		XR(ended);
		CHECK(xrEndFrame(session, &end) == XR_ERROR_CALL_ORDER_INVALID);
		if (verifyCapture && !quadMode && threaded && frame >= 4 && frame % 7 == 0)
			CHECK(!readPreview(frame + 3)[0].empty());
		if (EnvOn("SIMXR_PROBE_RECORDING"))
		{
			const auto gate = "recording." + std::to_string(frame + 3);
			WriteText(data / (gate + ".ready"), "1");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!std::filesystem::exists(data / (gate + ".continue")))
			{
				CHECK(std::chrono::steady_clock::now() < deadline);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	}
	if (!interactive && verifyCapture)
	{
		const auto eyes = readPreview(frames + 2);
		CHECK(eyes[0].size() == 64 * 48 * 4 && eyes[1].size() == eyes[0].size());
		CHECK(eyes[0][0] == std::byte{255} && eyes[0][2] == std::byte{0});
		CHECK(eyes[1][0] == std::byte{0} && eyes[1][2] == std::byte{255});
		if (mutableFormat)
		{
			const auto pixel = (24 * 64 + 12) * 4;
			CHECK(std::abs(int(eyes[0][pixel]) - 64) <= 1 && std::abs(int(eyes[0][pixel + 1]) - 128) <= 1 &&
				  std::abs(int(eyes[0][pixel + 2]) - 191) <= 1 && eyes[0][pixel + 3] == std::byte{255});
			std::puts(
				"PASS: mutable SRGB swapchain, alternate UNORM attachment clear, canonical SRGB composition pixels");
		}
		const auto center = (24 * 64 + 32) * 4;
		if (quadMode)
		{
			CHECK(eyes[0][center] == std::byte{0} && eyes[0][center + 1] == std::byte{255} &&
				  eyes[0][center + 2] == std::byte{0});
			CHECK(eyes[1][center] == std::byte{255} && eyes[1][center + 1] == std::byte{255} &&
				  eyes[1][center + 2] == std::byte{0});
		}
		else
		{
			CHECK(eyes[0][center] == std::byte{255} && eyes[0][center + 2] == std::byte{0});
			CHECK(eyes[1][center] == std::byte{255} && eyes[1][center + 2] == std::byte{0});
		}
	}
	VK(vkQueueWaitIdle(queue));
	XrFrameState drainState{XR_TYPE_FRAME_STATE};
	XrFrameWaitInfo drainWait{XR_TYPE_FRAME_WAIT_INFO};
	XR(xrWaitFrame(session, &drainWait, &drainState));
	XrFrameBeginInfo drainBegin{XR_TYPE_FRAME_BEGIN_INFO};
	XR(xrBeginFrame(session, &drainBegin));
	XrFrameEndInfo drainEnd{XR_TYPE_FRAME_END_INFO};
	drainEnd.displayTime = drainState.predictedDisplayTime;
	drainEnd.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	XR(xrEndFrame(session, &drainEnd));
	if (devicesMode)
	{
		XrEyeGazeSampleTimeEXT retainedTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
		XrSpaceLocation retainedLocation{XR_TYPE_SPACE_LOCATION, &retainedTime};
		XR(xrLocateSpace(gazeSpace, stage, prior, &retainedLocation));
		const auto expectedRetiredPose = retainedLocation.pose;
		XR(xrDestroyAction(gazeAction));
		XrActionStateGetInfo deadGet{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, gazeAction, eyes};
		XrActionStatePose deadState{XR_TYPE_ACTION_STATE_POSE};
		CHECK(xrGetActionStatePose(session, &deadGet, &deadState) == XR_ERROR_HANDLE_INVALID);
		XrActionSpaceCreateInfo deadSpace{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		deadSpace.action = gazeAction;
		deadSpace.subactionPath = eyes;
		deadSpace.poseInActionSpace.orientation.w = 1;
		XrSpace rejectedSpace;
		CHECK(xrCreateActionSpace(session, &deadSpace, &rejectedSpace) == XR_ERROR_HANDLE_INVALID);
		XrActiveActionSet active{set, XR_NULL_PATH};
		XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
		XR(xrSyncActions(session, &sync));
		XR(xrLocateSpace(gazeSpace, stage, prior, &retainedLocation));
		CHECK(retainedTime.time == prior && (retainedLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
			  std::fabs(retainedLocation.pose.position.x - expectedRetiredPose.position.x) < .0001f);
	}
	{
		XrSwapchain cancelled;
		XR(xrCreateSwapchain(session, &chain, &cancelled));
		uint64_t heldValue = 2;
		VkTimelineSemaphoreSubmitInfo holdTimeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		holdTimeline.waitSemaphoreValueCount = 1;
		holdTimeline.pWaitSemaphoreValues = &heldValue;
		VkPipelineStageFlags holdStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo held{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		held.pNext = &holdTimeline;
		held.waitSemaphoreCount = 1;
		held.pWaitSemaphores = &deferredSemaphore;
		held.pWaitDstStageMask = &holdStage;
		VK(vkQueueSubmit(queue, 1, &held, VK_NULL_HANDLE));
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		uint32_t cancelledIndex;
		XR(xrAcquireSwapchainImage(cancelled, &acquire, &cancelledIndex));
		XrSwapchainImageWaitInfo poll{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		CHECK(xrWaitSwapchainImage(cancelled, &poll) == XR_TIMEOUT_EXPIRED);
		auto waiter = std::async(std::launch::async, [cancelled] {
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = XR_INFINITE_DURATION;
			return xrWaitSwapchainImage(cancelled, &wait);
		});
		CHECK(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
		const auto destroyed = xrDestroySwapchain(cancelled);
		VkSemaphoreSignalInfo unblock{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		unblock.semaphore = deferredSemaphore;
		unblock.value = heldValue;
		VK(vkSignalSemaphore(device, &unblock));
		XR(destroyed);
		CHECK(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		const auto cancelledWait = waiter.get();
		CHECK(cancelledWait == XR_ERROR_SESSION_LOST || cancelledWait == XR_ERROR_HANDLE_INVALID);
		VK(vkQueueWaitIdle(queue));
	}
	XR(xrRequestExitSession(session));
	XR(xrEndSession(session));
	XrHapticActionInfo inactiveHaptic{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, haptic, right};
	CHECK(xrStopHapticFeedback(session, &inactiveHaptic) == XR_SESSION_NOT_FOCUSED);
	for (auto target : mutableTargets)
		vkDestroyFramebuffer(device, target, nullptr);
	for (auto view : mutableViews)
		vkDestroyImageView(device, view, nullptr);
	if (mutablePass)
		vkDestroyRenderPass(device, mutablePass, nullptr);
	XR(xrDestroySwapchain(swapchain));
	if (depthMode)
	{
		XR(xrDestroySwapchain(depth));
		vkUnmapMemory(device, depthMemory);
		vkDestroyBuffer(device, depthUpload, nullptr);
		vkFreeMemory(device, depthMemory, nullptr);
	}
	if (devicesMode)
	{
		XR(xrDestroySpace(gazeSpace));
		XR(xrDestroySpace(trackerSpace));
	}
	if (renderGaze)
		XR(xrDestroySpace(renderGaze));
	XR(xrDestroySpace(hand));
	XR(xrDestroySpace(head));
	XR(xrDestroySpace(stage));
	XrSwapchain orphanedSwapchain;
	XR(xrCreateSwapchain(session, &chain, &orphanedSwapchain));
	XrReferenceSpaceCreateInfo orphanedSpaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	orphanedSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	orphanedSpaceInfo.poseInReferenceSpace.orientation.w = 1;
	XrSpace orphanedSpace;
	XR(xrCreateReferenceSpace(session, &orphanedSpaceInfo, &orphanedSpace));
	if (bodyTracking)
		XR(destroyBody(upperBody));
	if (EnvOn("SIMXR_PROBE_CLOSE_RETRY"))
	{
		const std::uint64_t heldValue = 3;
		VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		timeline.waitSemaphoreValueCount = 1;
		timeline.pWaitSemaphoreValues = &heldValue;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo held{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		held.pNext = &timeline;
		held.waitSemaphoreCount = 1;
		held.pWaitSemaphores = &deferredSemaphore;
		held.pWaitDstStageMask = &stage;
		VK(vkQueueSubmit(queue, 1, &held, VK_NULL_HANDLE));
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		std::uint32_t pendingIndex;
		XR(xrAcquireSwapchainImage(orphanedSwapchain, &acquire, &pendingIndex));
		XrSwapchainImageWaitInfo poll{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		CHECK(xrWaitSwapchainImage(orphanedSwapchain, &poll) == XR_TIMEOUT_EXPIRED);
		const auto result = xrDestroySession(session);
		VkSemaphoreSignalInfo unblock{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		unblock.semaphore = deferredSemaphore;
		unblock.value = heldValue;
		VK(vkSignalSemaphore(device, &unblock));
		CHECK(result == XR_ERROR_RUNTIME_FAILURE);
		CHECK(xrAcquireSwapchainImage(orphanedSwapchain, &acquire, &pendingIndex) == XR_ERROR_SESSION_LOST);
		XrSession untouched = XR_NULL_HANDLE;
		CHECK(xrCreateSession(instance, &sci, &untouched) == XR_ERROR_LIMIT_REACHED && untouched == XR_NULL_HANDLE);
		VK(vkQueueWaitIdle(queue));
	}
	CHECK(!emptyProjectionHistory(sessionId, generation));
	XR(xrDestroySession(session));
	XrSession replacement;
	XR(xrCreateSession(instance, &sci, &replacement));
	CHECK(replacement != session);
	if (bodyTracking)
	{
		CHECK(destroyBody(body) == XR_ERROR_HANDLE_INVALID);
		CHECK(destroyBodyBD(bodyBD) == XR_ERROR_HANDLE_INVALID);
	}
	CHECK(xrBeginFrame(session, &drainBegin) == XR_ERROR_HANDLE_INVALID);
	CHECK(xrDestroySwapchain(orphanedSwapchain) == XR_ERROR_HANDLE_INVALID);
	CHECK(xrDestroySpace(orphanedSpace) == XR_ERROR_HANDLE_INVALID);
	if (devicesMode)
	{
		CHECK(xrDestroySpace(persistentSpace) == XR_ERROR_HANDLE_INVALID);
		XR(enumerateTrackers(instance, 0, &count, nullptr));
		CHECK(count == 4);
	}
	XR(xrBeginSession(replacement, &begin));
	XrFrameState replacementFrame{XR_TYPE_FRAME_STATE};
	XR(xrWaitFrame(replacement, &drainWait, &replacementFrame));
	XR(xrBeginFrame(replacement, &drainBegin));
	XrFrameEndInfo replacementEnd{XR_TYPE_FRAME_END_INFO};
	replacementEnd.displayTime = replacementFrame.predictedDisplayTime;
	replacementEnd.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	XR(xrEndFrame(replacement, &replacementEnd));
	const auto [replacementId, replacementGeneration] = sessionScope();
	CHECK(replacementId != sessionId);
	CHECK(emptyProjectionHistory(replacementId, replacementGeneration));
	XR(xrRequestExitSession(replacement));
	XR(xrEndSession(replacement));
	XR(xrDestroySession(replacement));
	XR(xrDestroyActionSet(set));
	XR(xrDestroyInstance(instance));
	if (creationRollback)
	{
		XrInstance recreated = XR_NULL_HANDLE;
		XR(xrCreateInstance(&ci, &recreated));
		XrInstanceProperties recreatedProperties{XR_TYPE_INSTANCE_PROPERTIES};
		XR(xrGetInstanceProperties(recreated, &recreatedProperties));
		XR(xrDestroyInstance(recreated));
	}
	vkDestroySemaphore(device, deferredSemaphore, nullptr);
	vkDestroyCommandPool(device, pool, nullptr);
	vkDestroyDevice(device, nullptr);
	if (messenger)
	{
		const auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
		CHECK(destroyDebug);
		destroyDebug(vkInstance, messenger, nullptr);
	}
	vkDestroyInstance(vkInstance, nullptr);
	CHECK(s_validationErrors == 0);
	if (EnvOn("SIMXR_PROBE_RETAIN_CONTROL"))
	{
		WriteText(data / "control.destroyed", "application XR/Vulkan objects destroyed\n");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!std::filesystem::exists(data / "control.retained.continue"))
		{
			CHECK(std::chrono::steady_clock::now() < deadline);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	std::printf("PASS: %llu checks; loader lifecycle, %s Vulkan, array/depth swapchains, frame order, "
				"transformed poses, held input, %s\n",
				static_cast<unsigned long long>(s_checks), v1 ? "v1" : "v2",
				quadMode ? "four-view projection/inset GPU submission" : "stereo capture pixels");
	return 0;
}

} // namespace simxr::probe

int main(int argc, char** argv)
try
{
	if (!simxr::InstallationReady())
	{
		std::fputs("OpenXR Simulator installation is being updated or is no longer available. Try again.\n", stderr);
		return 1;
	}
	using namespace simxr::probe;
	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--quad") != 0)
			return 2;
		CHECK(setenv("SIMXR_PROBE_QUAD", "1", 1) == 0);
	}
	const int cycles = std::max(1, EnvironmentCount("SIMXR_PROBE_CYCLES", 1, 0));
	for (int i = 0; i < cycles; ++i)
	{
		if (int result = RunProbe())
			return result;
	}
	return 0;
}
catch (const std::exception& error)
{
	std::fprintf(stderr, "FAIL: %s\n", error.what());
	return 1;
}
