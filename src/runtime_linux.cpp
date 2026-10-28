#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#include "Pch.h"
#include "control/legacy/LegacyFileChannel.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "graphics/backends/vulkan/VulkanSessionGraphics.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
#include "platform/linux/LinuxRuntime.h"
#include "platform/linux/InstallUse.h"
#include "presentation/FrameScheduler.h"
#include "runtime/DispatchTable.h"
#include "runtime/EntryPoints.h"
#include "runtime/EnumNames.h"
#include "runtime/EventQueue.h"
#include "runtime/Instance.h"
#include "runtime/Runtime.h"
#include "runtime/Session.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"
#include "runtime/validation/StructureChain.h"
#include "simulation/HapticState.h"
#include "simxr/Version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <loader_interfaces.h>
#include <mutex>
#include <openxr/openxr_platform.h>
#include <vulkan/vulkan.h>

static simxr::LinuxRuntime& Host()
{
	// The delivery observer outlives runtime/control workers and native bindings.
	simxr::InitializeVulkanImageDelivery();
	static simxr::LinuxRuntime s_runtime;
	return s_runtime;
}
static bool Valid(XrInstance handle)
{
	return static_cast<bool>(Host().FindInstance(handle));
}
static bool Valid(XrSession handle)
{
	return static_cast<bool>(Host().FindSession(handle));
}
template <typename T>
static XrResult Enumerate(std::uint32_t cap, std::uint32_t* count, T* out, const std::vector<T>& items)
{
	if (!count || (cap && !out))
		return XR_ERROR_VALIDATION_FAILURE;
	if (items.size() > std::numeric_limits<std::uint32_t>::max())
		return XR_ERROR_LIMIT_REACHED;
	*count = static_cast<std::uint32_t>(items.size());
	if (!cap)
		return XR_SUCCESS;
	if (cap < items.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(items.begin(), items.end(), out);
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrEnumerateInstanceExtensionProperties(const char* layer, std::uint32_t cap,
																	std::uint32_t* n, XrExtensionProperties* p)
{
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (layer)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	return simxr::LinuxRuntime::Extensions().Enumerate(cap, n, p);
}
static XrResult XRAPI_PTR RunXrCreateInstance(const XrInstanceCreateInfo* info, XrInstance* out)
{
	return Host().CreateInstance(info, out);
}
static XrResult XRAPI_PTR RunXrGetInstanceProperties(XrInstance iHandle, XrInstanceProperties* p)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	if (!p || p->type != XR_TYPE_INSTANCE_PROPERTIES)
		return XR_ERROR_VALIDATION_FAILURE;
	p->runtimeVersion = simxr::RuntimeVersion;
	std::strcpy(p->runtimeName, "OpenXR Simulator Linux Vulkan");
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrGetSystem(XrInstance iHandle, const XrSystemGetInfo* info, XrSystemId* id)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !id || info->type != XR_TYPE_SYSTEM_GET_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	*id = 1;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrGetSystemProperties(XrInstance iHandle, XrSystemId id, XrSystemProperties* p)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	if (id != 1)
		return XR_ERROR_SYSTEM_INVALID;
	auto validation = simxr::FillSimulationProperties(i->core->Extensions(), p);
	if (XR_FAILED(validation))
		return validation;
	p->systemId = 1;
	p->vendorId = 0;
	std::strcpy(p->systemName, "Simulated Linux HMD");
	p->graphicsProperties = {8192, 8192, 16};
	p->trackingProperties = {XR_TRUE, XR_TRUE};
	return XR_SUCCESS;
}
static XrResult ValidateViewConfiguration(simxr::LinuxRuntime::InstanceRef instance, XrSystemId system,
										  XrViewConfigurationType type)
{
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ||
		(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
		 instance->core->Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)))
		return XR_SUCCESS;
	return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
}
static XrResult XRAPI_PTR RunXrEnumerateViewConfigurations(XrInstance handle, XrSystemId system, std::uint32_t c,
														   std::uint32_t* n, XrViewConfigurationType* p)
{
	auto instance = Host().FindInstance(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	const auto validation = ValidateViewConfiguration(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	if (XR_FAILED(validation))
		return validation;
	std::vector<XrViewConfigurationType> types{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	if (instance->core->Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME))
		types.push_back(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
	return Enumerate(c, n, p, types);
}
static XrResult XRAPI_PTR RunXrGetViewConfigurationProperties(XrInstance handle, XrSystemId system,
															  XrViewConfigurationType type,
															  XrViewConfigurationProperties* p)
{
	auto instance = Host().FindInstance(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	const auto validation = ValidateViewConfiguration(instance, system, type);
	if (XR_FAILED(validation))
		return validation;
	if (!p || p->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES)
		return XR_ERROR_VALIDATION_FAILURE;
	if (XR_FAILED(simxr::StructureChain(p->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	p->viewConfigurationType = type;
	p->fovMutable = XR_TRUE;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrEnumerateViewConfigurationViews(XrInstance handle, XrSystemId system,
															   XrViewConfigurationType type, std::uint32_t c,
															   std::uint32_t* n, XrViewConfigurationView* p)
{
	auto instance = Host().FindInstance(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	const auto validation = ValidateViewConfiguration(instance, system, type);
	if (XR_FAILED(validation))
		return validation;
	if (!n || (c && !p))
		return XR_ERROR_VALIDATION_FAILURE;
	const std::uint32_t count = type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ? 4 : 2;
	*n = count;
	if (!c)
		return XR_SUCCESS;
	if (c < count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	const bool foveationEnabled = instance->core->Extensions().Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
	const std::array known{XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO};
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const simxr::StructureChain chain(p[index].next);
		if (p[index].type != XR_TYPE_VIEW_CONFIGURATION_VIEW ||
			XR_FAILED(chain.Validate(std::span(known).first(foveationEnabled ? 1 : 0))))
			return XR_ERROR_VALIDATION_FAILURE;
		if (foveationEnabled)
			if (const auto request = chain.Find<XrFoveatedViewConfigurationViewVARJO>(known[0]);
				request && request->foveatedRenderingActive > XR_TRUE)
				return XR_ERROR_VALIDATION_FAILURE;
	}
	std::uint32_t maxSamples = 1;
	const auto samplesResult = instance->graphics.MaxSampleCount(maxSamples);
	if (XR_FAILED(samplesResult))
		return samplesResult;
	// Fixed and gaze-steered insets use the same qualified texture dimensions.
	const auto& settings = instance->Input().Views();
	for (std::uint32_t index = 0; index < count; ++index)
	{
		p[index].recommendedImageRectWidth = index < 2 ? settings.outerWidth : settings.insetWidth;
		p[index].recommendedImageRectHeight = index < 2 ? settings.outerHeight : settings.insetHeight;
		p[index].maxImageRectWidth = p[index].maxImageRectHeight = 8192;
		p[index].recommendedSwapchainSampleCount = 1;
		p[index].maxSwapchainSampleCount = maxSamples;
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrEnumerateEnvironmentBlendModes(XrInstance handle, XrSystemId system,
															  XrViewConfigurationType type, std::uint32_t c,
															  std::uint32_t* n, XrEnvironmentBlendMode* p)
{
	auto instance = Host().FindInstance(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	const auto validation = ValidateViewConfiguration(instance, system, type);
	if (XR_FAILED(validation))
		return validation;
	return Enumerate(c, n, p, std::vector<XrEnvironmentBlendMode>{XR_ENVIRONMENT_BLEND_MODE_OPAQUE});
}
static XrResult XRAPI_PTR RunXrGetVulkanGraphicsRequirementsKHR(XrInstance handle, XrSystemId system,
																XrGraphicsRequirementsVulkanKHR* output)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.Requirements(system, output);
}
static XrResult XRAPI_PTR RunXrGetVulkanGraphicsRequirements2KHR(XrInstance handle, XrSystemId system,
																 XrGraphicsRequirementsVulkanKHR* output)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.Requirements(system, output);
}
static XrResult XRAPI_PTR RunXrGetVulkanInstanceExtensionsKHR(XrInstance handle, XrSystemId system,
															  std::uint32_t capacity, std::uint32_t* count,
															  char* output)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.InstanceExtensions(system, capacity, count, output);
}
static XrResult XRAPI_PTR RunXrGetVulkanDeviceExtensionsKHR(XrInstance handle, XrSystemId system,
															std::uint32_t capacity, std::uint32_t* count, char* output)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.DeviceExtensions(system, capacity, count, output);
}
static XrResult XRAPI_PTR RunXrCreateVulkanInstanceKHR(XrInstance handle, const XrVulkanInstanceCreateInfoKHR* info,
													   VkInstance* output, VkResult* result)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.CreateInstance(info, output, result);
}
static XrResult XRAPI_PTR RunXrCreateVulkanDeviceKHR(XrInstance handle, const XrVulkanDeviceCreateInfoKHR* info,
													 VkDevice* output, VkResult* result)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.CreateDevice(info, output, result);
}
static XrResult XRAPI_PTR RunXrGetVulkanGraphicsDeviceKHR(XrInstance handle, XrSystemId system,
														  VkInstance graphicsInstance, VkPhysicalDevice* output)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.PhysicalDevice(system, graphicsInstance, output);
}
static XrResult XRAPI_PTR RunXrGetVulkanGraphicsDevice2KHR(XrInstance handle,
														   const XrVulkanGraphicsDeviceGetInfoKHR* info,
														   VkPhysicalDevice* output)
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!info || info->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	if (XR_FAILED(simxr::StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	return instance->graphics.PhysicalDevice(info->systemId, info->vulkanInstance, output);
}
static XrResult XRAPI_PTR RunXrCreateSession(XrInstance handle, const XrSessionCreateInfo* info, XrSession* output)
{
	const auto result = Host().CreateSession(handle, info, output);
	if (XR_FAILED(result))
		return result;
	// Driver/validation DSOs register their exit handlers during native creation.
	// Close retained control readers and the retirement worker before those handlers.
	// Rearm after each creation because drivers may have been unloaded and reloaded.
	if (std::atexit(
			[]
			{
				Host().Shutdown();
				simxr::ShutdownVulkanImageDelivery();
			}) != 0)
	{
		Host().DestroySession(*output);
		*output = XR_NULL_HANDLE;
		return XR_ERROR_OUT_OF_MEMORY;
	}
	return result;
}
static XrResult XRAPI_PTR RunXrBeginSession(XrSession handle, const XrSessionBeginInfo* info)
{
	return Host().BeginSession(handle, info);
}
static XrResult XRAPI_PTR RunXrRequestExitSession(XrSession handle)
{
	auto session = Host().FindSession(handle);
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	auto result = session->core->RequestExit(simxr::LinuxRuntime::SessionTime(*session.Shared()));
	session->owner->PublishStatus(session->core);
	return result;
}
static XrResult XRAPI_PTR RunXrEndSession(XrSession handle)
{
	return Host().EndSession(handle);
}
static XrResult XRAPI_PTR RunXrPollEvent(XrInstance iHandle, XrEventDataBuffer* p)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	if (!p)
		return XR_ERROR_VALIDATION_FAILURE;
	return i->core->Events().Poll(*p);
}
static XrResult XRAPI_PTR RunXrWaitFrame(XrSession sHandle, const XrFrameWaitInfo* info, XrFrameState* p)
{
	auto s = Host().FindSession(sHandle);
	std::unique_lock<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(s))
		return XR_ERROR_HANDLE_INVALID;
	if (!p || p->type != XR_TYPE_FRAME_STATE || (info && info->type != XR_TYPE_FRAME_WAIT_INFO))
		return XR_ERROR_VALIDATION_FAILURE;
	auto core = s->core.Shared();
	apiLock.unlock();
	simxr::FramePrediction prediction;
	const auto publishNative = [&](const simxr::FramePrediction& admitted) -> XrResult
	{
		std::lock_guard nativeLock(s->graphicsMutex);
		if (!Valid(s) || s->core.Shared() != core || core->Snapshot().closed)
			return XR_ERROR_HANDLE_INVALID;
		s->graphics->PollCaptures();
		std::lock_guard stateLock(Host().Mutex());
		if (s->owner->PollCommands(s->core->Snapshot().lastSubmitted))
			s->graphics->RequestCapture({"both", "all"}, s->core->Snapshot().lastSubmitted);
		const auto& input = s->owner->Input();
		const auto tracking = input.Tracking(s->core->Snapshot().state == XR_SESSION_STATE_FOCUSED);
		s->core->PublishTracking(admitted.displayTime, tracking, input.Views(), admitted.id);
		p->predictedDisplayTime = s->core->Snapshot().lastIssued.displayTime.value.count();
		p->predictedDisplayPeriod = admitted.displayPeriod.count();
		p->shouldRender = s->core->Snapshot().state == XR_SESSION_STATE_VISIBLE ||
						  s->core->Snapshot().state == XR_SESSION_STATE_FOCUSED;
		return XR_SUCCESS;
	};
	return core->WaitFrame(prediction, publishNative);
}
static XrResult XRAPI_PTR RunXrBeginFrame(XrSession sHandle, const XrFrameBeginInfo* info)
{
	auto s = Host().FindSession(sHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(s))
		return XR_ERROR_HANDLE_INVALID;
	if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return s->core->BeginFrame();
}
static XrResult XRAPI_PTR RunXrLocateViews(XrSession handle, const XrViewLocateInfo* info, XrViewState* state,
										   std::uint32_t cap, std::uint32_t* count, XrView* views)
{
	auto session = Host().FindSession(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->LocateViews(*info, state, cap, count, views);
}
static XrResult XRAPI_PTR RunXrEnumerateSwapchainFormats(XrSession handle, std::uint32_t capacity, std::uint32_t* count,
														 std::int64_t* formats)
{
	auto session = Host().FindSession(handle);
	std::lock_guard lock(Host().Mutex());
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!count || (capacity && !formats))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto supported = simxr::VulkanSwapchain::Formats();
	*count = static_cast<std::uint32_t>(supported.size());
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < supported.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(supported.begin(), supported.end(), formats);
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrCreateSwapchain(XrSession handle, const XrSwapchainCreateInfo* info, XrSwapchain* output)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (session->core->Snapshot().closed)
		return XR_ERROR_SESSION_LOST;
	if (!info || !output)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->graphics->CreateSwapchain(*info, *output);
}
static XrResult XRAPI_PTR RunXrEnumerateSwapchainImages(XrSwapchain handle, std::uint32_t capacity,
														std::uint32_t* count, XrSwapchainImageBaseHeader* images)
{
	const auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	const auto chain = session->graphics->FindSwapchain(handle);
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	return chain->EnumerateImages(capacity, count, images);
}
static XrResult XRAPI_PTR RunXrAcquireSwapchainImage(XrSwapchain handle, const XrSwapchainImageAcquireInfo* info,
													 std::uint32_t* index)
{
	const auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	const auto chain = session->graphics->FindSwapchain(handle);
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (session->core->Snapshot().closed)
		return XR_ERROR_SESSION_LOST;
	if (!index || (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO))
		return XR_ERROR_VALIDATION_FAILURE;
	return session->graphics->Acquire(*chain.Shared(), *index);
}
static XrResult XRAPI_PTR RunXrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo* info)
{
	const auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::unique_lock nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	const auto chain = session->graphics->FindSwapchain(handle);
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO || info->timeout < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	auto registered = session->graphics->EnterWait();
	if (!registered)
		return XR_ERROR_SESSION_LOST;
	nativeLock.unlock();
	return chain->Wait(info->timeout);
}
static XrResult XRAPI_PTR RunXrReleaseSwapchainImage(XrSwapchain handle, const XrSwapchainImageReleaseInfo* info)
{
	const auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	const auto chain = session->graphics->FindSwapchain(handle);
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return chain->Release();
}
static XrResult XRAPI_PTR RunXrDestroySwapchain(XrSwapchain handle)
{
	const auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	const auto chain = session->graphics->FindSwapchain(handle);
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	session->graphics->PollCaptures();
	session->graphics->DestroySwapchain(chain);
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrEndFrame(XrSession handle, const XrFrameEndInfo* info)
{
	const auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!session->core->Snapshot().running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	const auto begun = session->core->Scheduler().Snapshot().begun;
	if (!begun)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (!info || info->type != XR_TYPE_FRAME_END_INFO || (info->layerCount && !info->layers))
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->displayTime <= 0)
		return XR_ERROR_TIME_INVALID;
	if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
		return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
	const auto result = session->graphics->EndFrame(*info);
	if (XR_FAILED(result))
		return result;
	try
	{
		std::uint32_t projectionCount = 0;
		for (std::uint32_t layer = 0; layer < info->layerCount; ++layer)
			if (info->layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
				++projectionCount;
		std::lock_guard lock(Host().Mutex());
		session->owner->PublishStatus(session->core,
									  simxr::LinuxRuntimeStatus::FrameCounts{info->layerCount, projectionCount});
	}
	catch (const std::exception& error)
	{
		try
		{
			session->owner->Log(std::string("submitted frame observer failed: ") + error.what());
		}
		catch (...)
		{
		}
	}
	catch (...)
	{
		try
		{
			session->owner->Log("submitted frame observer failed");
		}
		catch (...)
		{
		}
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrEnumerateReferenceSpaces(XrSession sHandle, std::uint32_t c, std::uint32_t* n,
														XrReferenceSpaceType* p)
{
	auto s = Host().FindSession(sHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(s))
		return XR_ERROR_HANDLE_INVALID;
	std::vector<XrReferenceSpaceType> types{XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
											XR_REFERENCE_SPACE_TYPE_STAGE};
	if (s->core->Extensions().Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME))
		types.push_back(XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO);
	return Enumerate(c, n, p, types);
}
static XrResult XRAPI_PTR RunXrCreateReferenceSpace(XrSession handle, const XrReferenceSpaceCreateInfo* info,
													XrSpace* out)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !out || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Spaces().CreateReferenceSpace(info->referenceSpaceType, info->poseInReferenceSpace, *out);
}
static XrResult XRAPI_PTR RunXrGetReferenceSpaceBoundsRect(XrSession handle, XrReferenceSpaceType type,
														   XrExtent2Df* bounds)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!bounds)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Spaces().Bounds(type, *bounds);
}
static XrResult XRAPI_PTR RunXrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location)
{
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Host().CurrentSession())
		return XR_ERROR_HANDLE_INVALID;
	return simxr::LocateSpace(Host().CurrentSession()->core->Spaces(), space, base, time, location,
							  &Host().CurrentSession()->core->Extensions());
}
static XrResult XRAPI_PTR RunXrDestroySpace(XrSpace space)
{
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Host().CurrentSession())
		return XR_ERROR_HANDLE_INVALID;
	return Host().CurrentSession()->core->Spaces().Destroy(space);
}
static XrResult XRAPI_PTR RunXrStringToPath(XrInstance handle, const char* name, XrPath* path)
{
	auto instance = Host().FindInstance(handle);
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	if (!name || !path)
		return XR_ERROR_VALIDATION_FAILURE;
	auto end = std::find(name, name + XR_MAX_PATH_LENGTH, '\0');
	if (end == name + XR_MAX_PATH_LENGTH)
		return XR_ERROR_PATH_FORMAT_INVALID;
	return instance->core->Actions().StringToPath(std::string_view(name, end - name), *path);
}
static XrResult XRAPI_PTR RunXrPathToString(XrInstance handle, XrPath path, std::uint32_t capacity,
											std::uint32_t* count, char* buffer)
{
	auto instance = Host().FindInstance(handle);
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	return instance->core->Actions().PathToString(path, capacity, count, buffer);
}
static XrResult XRAPI_PTR RunXrCreateActionSet(XrInstance handle, const XrActionSetCreateInfo* info, XrActionSet* set)
{
	auto instance = Host().FindInstance(handle);
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !set)
		return XR_ERROR_VALIDATION_FAILURE;
	return instance->core->Actions().CreateSet(*info, *set);
}
static XrResult XRAPI_PTR RunXrCreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	if (!Host().CurrentInstance())
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !action)
		return XR_ERROR_VALIDATION_FAILURE;
	return Host().CurrentInstance()->core->Actions().CreateAction(set, *info, *action);
}
static XrResult XRAPI_PTR RunXrSuggestInteractionProfileBindings(XrInstance handle,
																 const XrInteractionProfileSuggestedBinding* info)
{
	auto instance = Host().FindInstance(handle);
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return instance->core->Actions().SuggestBindings(*info);
}
static XrResult XRAPI_PTR RunXrAttachSessionActionSets(XrSession handle, const XrSessionActionSetsAttachInfo* info)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Attach(*info);
}
static XrResult XRAPI_PTR RunXrCreateActionSpace(XrSession handle, const XrActionSpaceCreateInfo* info, XrSpace* out)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !out || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Spaces().CreateActionSpace(info->action, info->subactionPath, info->poseInActionSpace, *out);
}
static XrResult XRAPI_PTR RunXrSyncActions(XrSession handle, const XrActionsSyncInfo* info)
{
	auto session = Host().FindSession(handle);
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	if (!Valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->SyncActions(*info, session->owner->Input().Controls());
}
static XrResult XRAPI_PTR RunXrGetActionStateBoolean(XrSession handle, const XrActionStateGetInfo* info,
													 XrActionStateBoolean* state)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Actions().GetBoolean(*info, *state);
}
static XrResult XRAPI_PTR RunXrGetActionStateFloat(XrSession handle, const XrActionStateGetInfo* info,
												   XrActionStateFloat* state)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Actions().GetFloat(*info, *state);
}
static XrResult XRAPI_PTR RunXrGetActionStateVector2f(XrSession handle, const XrActionStateGetInfo* info,
													  XrActionStateVector2f* state)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Actions().GetVector(*info, *state);
}
static XrResult XRAPI_PTR RunXrGetActionStatePose(XrSession handle, const XrActionStateGetInfo* info,
												  XrActionStatePose* state)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Actions().GetPose(*info, *state);
}
static XrResult XRAPI_PTR RunXrGetCurrentInteractionProfile(XrSession handle, XrPath user,
															XrInteractionProfileState* state)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!state || state->type != XR_TYPE_INTERACTION_PROFILE_STATE)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->core->Actions().CurrentProfile(user, state->interactionProfile);
}
static XrResult XRAPI_PTR RunXrEnumerateBoundSourcesForAction(XrSession handle,
															  const XrBoundSourcesForActionEnumerateInfo* info,
															  std::uint32_t capacity, std::uint32_t* count, XrPath* out)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	std::vector<XrPath> paths;
	auto result = session->core->Actions().BoundSources(info->action, paths);
	if (XR_FAILED(result))
		return result;
	return Enumerate(capacity, count, out, paths);
}
static XrResult XRAPI_PTR RunXrGetInputSourceLocalizedName(XrSession handle,
														   const XrInputSourceLocalizedNameGetInfo* info,
														   std::uint32_t capacity, std::uint32_t* count, char* out)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !count || (capacity && !out) || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	std::string name;
	const auto result = session->core->Actions().LocalizedName(info->sourcePath, info->whichComponents, name);
	if (XR_FAILED(result))
		return result;
	*count = static_cast<std::uint32_t>(name.size() + 1);
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::memcpy(out, name.c_str(), *count);
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrApplyHapticFeedback(XrSession handle, const XrHapticActionInfo* info,
												   const XrHapticBaseHeader* feedback)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->ApplyHaptic(info, feedback);
}
static XrResult XRAPI_PTR RunXrStopHapticFeedback(XrSession handle, const XrHapticActionInfo* info)
{
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->StopHaptic(info);
}
static XrResult XRAPI_PTR RunXrDestroyAction(XrAction action)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	if (!Host().CurrentInstance())
		return XR_ERROR_HANDLE_INVALID;
	if (Host().CurrentSession())
	{
		Host().CurrentSession()->core->Haptics().RemoveAction(action, Host().CurrentSession()->core->Snapshot().time);
	}
	return Host().CurrentInstance()->core->Actions().DestroyAction(action);
}
static XrResult XRAPI_PTR RunXrDestroyActionSet(XrActionSet set)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	if (!Host().CurrentInstance())
		return XR_ERROR_HANDLE_INVALID;
	for (const auto& action : Host().CurrentInstance()->core->Actions().Actions())
		if (action.set == set)
			RunXrDestroyAction(action.handle);
	return Host().CurrentInstance()->core->Actions().DestroySet(set);
}
static XrResult XRAPI_PTR RunXrDestroySession(XrSession handle)
{
	return Host().DestroySession(handle);
}
static XrResult XRAPI_PTR RunXrEnumerateViveTrackerPathsHTCX(XrInstance handle, std::uint32_t capacity,
															 std::uint32_t* count, XrViveTrackerPathsHTCX* paths)
{
	auto instance = Host().Core().FindInstance(handle);
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	return instance->Trackers().Enumerate(capacity, count, paths);
}
static XrResult XRAPI_PTR RunXrCreateBodyTrackerFB(XrSession handle, const XrBodyTrackerCreateInfoFB* info,
												   XrBodyTrackerFB* output)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().CreateFB(info, output);
}

static XrResult XRAPI_PTR RunXrCreateBodyTrackerBD(XrSession handle, const XrBodyTrackerCreateInfoBD* info,
												   XrBodyTrackerBD* output)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().CreateBD(info, output);
}

static XrResult XRAPI_PTR RunXrDestroyBodyTrackerFB(XrBodyTrackerFB tracker)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().DestroyFB(tracker);
}

static XrResult XRAPI_PTR RunXrDestroyBodyTrackerBD(XrBodyTrackerBD tracker)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().DestroyBD(tracker);
}

static XrResult XRAPI_PTR RunXrLocateBodyJointsFB(XrBodyTrackerFB tracker, const XrBodyJointsLocateInfoFB* info,
												  XrBodyJointLocationsFB* output)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().LocateFB(tracker, info, output);
}

static XrResult XRAPI_PTR RunXrLocateBodyJointsBD(XrBodyTrackerBD tracker, const XrBodyJointsLocateInfoBD* info,
												  XrBodyJointLocationsBD* output)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().LocateBD(tracker, info, output);
}

static XrResult XRAPI_PTR RunXrGetBodySkeletonFB(XrBodyTrackerFB tracker, XrBodySkeletonFB* output)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().SkeletonFB(tracker, output);
}

static XrResult XRAPI_PTR RunXrSuggestBodyTrackingCalibrationOverrideMETA(XrBodyTrackerFB tracker,
																		  const XrBodyTrackingCalibrationInfoMETA* info)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().SuggestCalibrationFB(tracker, info);
}

static XrResult XRAPI_PTR RunXrResetBodyTrackingCalibrationMETA(XrBodyTrackerFB tracker)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().ResetCalibrationFB(tracker);
}

static XrResult XRAPI_PTR RunXrRequestBodyTrackingFidelityMETA(XrBodyTrackerFB tracker,
															   XrBodyTrackingFidelityMETA fidelity)
{
	std::lock_guard<std::recursive_mutex> lock(Host().Mutex());
	auto session = Host().CurrentSession();
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	return session->core->Body().RequestFidelityFB(tracker, fidelity);
}
static XrResult XRAPI_PTR RunXrDestroyInstance(XrInstance iHandle)
{
	return Host().DestroyInstance(iHandle);
}
static XrResult XRAPI_PTR RunXrConvertTimespecTimeToTimeKHR(XrInstance iHandle, const timespec* t, XrTime* p)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	if (!i->core->Extensions().Contains(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!p || !t)
		return XR_ERROR_VALIDATION_FAILURE;
	if (t->tv_sec < 0 || t->tv_nsec < 0 || t->tv_nsec >= 1000000000 ||
		t->tv_sec > (std::numeric_limits<XrTime>::max() - t->tv_nsec) / 1000000000LL)
		return XR_ERROR_TIME_INVALID;
	*p = t->tv_sec * 1000000000LL + t->tv_nsec;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrConvertTimeToTimespecTimeKHR(XrInstance iHandle, XrTime t, timespec* p)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	if (!i->core->Extensions().Contains(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!p)
		return XR_ERROR_VALIDATION_FAILURE;
	if (t <= 0)
		return XR_ERROR_TIME_INVALID;
	const auto seconds = t / 1000000000;
	if (!std::in_range<decltype(p->tv_sec)>(seconds))
		return XR_ERROR_TIME_INVALID;
	p->tv_sec = static_cast<decltype(p->tv_sec)>(seconds);
	p->tv_nsec = static_cast<decltype(p->tv_nsec)>(t % 1000000000);
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR RunXrResultToString(XrInstance handle, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	return simxr::ResultToString(value, buffer);
}
static XrResult XRAPI_PTR RunXrStructureTypeToString(XrInstance handle, XrStructureType value,
													 char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
	const auto instance = Host().FindInstance(handle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!Valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	return simxr::StructureTypeToString(value, buffer);
}
#define ENTRY(name) {"xr" #name, reinterpret_cast<PFN_xrVoidFunction>(simxr::AbiBoundary<RunXr##name>::Invoke)}
static XrResult XRAPI_PTR RunXrGetInstanceProcAddr(XrInstance iHandle, const char* name, PFN_xrVoidFunction* p)
{
	auto i = Host().FindInstance(iHandle);
	std::lock_guard<std::recursive_mutex> apiLock(Host().Mutex());
	if (!name || !p)
		return XR_ERROR_VALIDATION_FAILURE;
	*p = nullptr;
	if (iHandle && !Valid(i))
		return XR_ERROR_HANDLE_INVALID;
	static const simxr::DispatchEntry s_entries[] = {
		{"xrGetInstanceProcAddr",
		 reinterpret_cast<PFN_xrVoidFunction>(simxr::AbiBoundary<RunXrGetInstanceProcAddr>::Invoke), true},
		{"xrEnumerateInstanceExtensionProperties",
		 reinterpret_cast<PFN_xrVoidFunction>(simxr::AbiBoundary<RunXrEnumerateInstanceExtensionProperties>::Invoke),
		 true},
		{"xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction>(simxr::AbiBoundary<RunXrCreateInstance>::Invoke),
		 true},
		ENTRY(DestroyInstance),
		ENTRY(GetInstanceProperties),
		ENTRY(GetSystem),
		ENTRY(GetSystemProperties),
		ENTRY(EnumerateViewConfigurations),
		ENTRY(GetViewConfigurationProperties),
		ENTRY(EnumerateViewConfigurationViews),
		ENTRY(EnumerateEnvironmentBlendModes),
		ENTRY(GetVulkanGraphicsRequirementsKHR),
		ENTRY(GetVulkanGraphicsRequirements2KHR),
		ENTRY(GetVulkanInstanceExtensionsKHR),
		ENTRY(GetVulkanDeviceExtensionsKHR),
		ENTRY(GetVulkanGraphicsDeviceKHR),
		ENTRY(GetVulkanGraphicsDevice2KHR),
		ENTRY(CreateVulkanInstanceKHR),
		ENTRY(CreateVulkanDeviceKHR),
		ENTRY(EnumerateViveTrackerPathsHTCX),
		ENTRY(CreateBodyTrackerFB),
		ENTRY(CreateBodyTrackerBD),
		ENTRY(DestroyBodyTrackerFB),
		ENTRY(DestroyBodyTrackerBD),
		ENTRY(LocateBodyJointsFB),
		ENTRY(LocateBodyJointsBD),
		ENTRY(GetBodySkeletonFB),
		ENTRY(SuggestBodyTrackingCalibrationOverrideMETA),
		ENTRY(ResetBodyTrackingCalibrationMETA),
		ENTRY(RequestBodyTrackingFidelityMETA),
		ENTRY(CreateSession),
		ENTRY(DestroySession),
		ENTRY(BeginSession),
		ENTRY(EndSession),
		ENTRY(RequestExitSession),
		ENTRY(PollEvent),
		ENTRY(WaitFrame),
		ENTRY(BeginFrame),
		ENTRY(EndFrame),
		ENTRY(LocateViews),
		ENTRY(EnumerateSwapchainFormats),
		ENTRY(CreateSwapchain),
		ENTRY(DestroySwapchain),
		ENTRY(EnumerateSwapchainImages),
		ENTRY(AcquireSwapchainImage),
		ENTRY(WaitSwapchainImage),
		ENTRY(ReleaseSwapchainImage),
		ENTRY(EnumerateReferenceSpaces),
		ENTRY(CreateReferenceSpace),
		ENTRY(GetReferenceSpaceBoundsRect),
		ENTRY(LocateSpace),
		ENTRY(DestroySpace),
		ENTRY(StringToPath),
		ENTRY(PathToString),
		ENTRY(CreateActionSet),
		ENTRY(DestroyActionSet),
		ENTRY(CreateAction),
		ENTRY(DestroyAction),
		ENTRY(SuggestInteractionProfileBindings),
		ENTRY(AttachSessionActionSets),
		ENTRY(CreateActionSpace),
		ENTRY(SyncActions),
		ENTRY(GetActionStateBoolean),
		ENTRY(GetActionStateFloat),
		ENTRY(GetActionStateVector2f),
		ENTRY(GetActionStatePose),
		ENTRY(GetCurrentInteractionProfile),
		ENTRY(EnumerateBoundSourcesForAction),
		ENTRY(GetInputSourceLocalizedName),
		ENTRY(ApplyHapticFeedback),
		ENTRY(StopHapticFeedback),
		ENTRY(ResultToString),
		ENTRY(StructureTypeToString),
		ENTRY(ConvertTimespecTimeToTimeKHR),
		ENTRY(ConvertTimeToTimespecTimeKHR)};
	static const simxr::DispatchTable s_table(s_entries, simxr::LinuxRuntime::Extensions());
	if (!s_table.CoversAdvertisedCommands())
		return XR_ERROR_RUNTIME_FAILURE;
	return s_table.Resolve(name, i ? &i->core->Extensions() : nullptr, *p);
}
#undef ENTRY

extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loader, XrNegotiateRuntimeRequest* runtime)
{
	if (!simxr::InstallationReady())
		return XR_ERROR_INITIALIZATION_FAILED;
	if (!loader || !runtime || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
		runtime->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
		runtime->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || runtime->structSize != sizeof(*runtime) ||
		loader->minInterfaceVersion > 1 || loader->maxInterfaceVersion < 1 ||
		loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0))
		return XR_ERROR_INITIALIZATION_FAILED;
	runtime->runtimeInterfaceVersion = 1;
	if (loader->minApiVersion > XR_MAKE_VERSION(1, 0, 27))
		return XR_ERROR_INITIALIZATION_FAILED;
	runtime->runtimeApiVersion = std::min<XrVersion>(loader->maxApiVersion, XR_MAKE_VERSION(1, 0, 27));
	runtime->getInstanceProcAddr = simxr::AbiBoundary<RunXrGetInstanceProcAddr>::Invoke;
	return XR_SUCCESS;
}

extern "C" __attribute__((visibility("default"))) int simxrGetGameBridgeApi(std::uint32_t version,
																			SimxrGameBridgeApi* api) noexcept
{
	try
	{
		if (!simxr::InstallationReady())
			return 0;
		if (!api || version != SIMXR_GAME_BRIDGE_VERSION || api->size < sizeof(*api))
			return 0;
		Host().StartControlEndpoint();
		if (!Host().Core().Game().FillApi(version, *api))
			return 0;
		api->shutdown = [](void*) -> int
		{
			try
			{
				return Host().Shutdown() ? 1 : 0;
			}
			catch (...)
			{
				return 0;
			}
		};
		return 1;
	}
	catch (...)
	{
		return 0;
	}
}
