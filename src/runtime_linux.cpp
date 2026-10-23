#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#include "Math.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "control/legacy/backends/vulkan/VulkanLegacyCapture.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "graphics/backends/vulkan/VulkanSessionGraphics.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
#include "platform/linux/LinuxRuntime.h"
#include "presentation/FrameScheduler.h"
#include "runtime/DispatchTable.h"
#include "runtime/EntryPoints.h"
#include "runtime/EventQueue.h"
#include "runtime/HandleRegistry.h"
#include "runtime/Instance.h"
#include "runtime/Runtime.h"
#include "runtime/Session.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"
#include "runtime/validation/StructureChain.h"
#include "simulation/HapticState.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <loader_interfaces.h>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

namespace sim
{
static simxr::LinuxRuntime& Host()
{
	// The delivery observer outlives runtime/control workers and native bindings.
	simxr::InitializeVulkanImageDelivery();
	static simxr::LinuxRuntime runtime;
	return runtime;
}
static bool valid(XrInstance handle)
{
	return static_cast<bool>(Host().FindInstance(handle));
}
static bool valid(XrSession handle)
{
	return static_cast<bool>(Host().FindSession(handle));
}
template <typename T> static XrResult enumerate(uint32_t cap, uint32_t *count, T *out, const std::vector<T> &items)
{
    if (!count || (cap && !out))
        return XR_ERROR_VALIDATION_FAILURE;
    *count = uint32_t(items.size());
    if (!cap)
        return XR_SUCCESS;
    if (cap < items.size())
        return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(items.begin(), items.end(), out);
    return XR_SUCCESS;
}
} // namespace sim
#define FN(name) static XrResult XRAPI_PTR run_##name
FN(xrEnumerateInstanceExtensionProperties)(const char *layer, uint32_t cap, uint32_t *n, XrExtensionProperties *p)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (layer)
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    return simxr::LinuxRuntime::Extensions().Enumerate(cap, n, p);
}
FN(xrCreateInstance)(const XrInstanceCreateInfo *info, XrInstance *out)
{
	return sim::Host().CreateInstance(info, out);
}
FN(xrGetInstanceProperties)(XrInstance iHandle, XrInstanceProperties *p)
{
    auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (!p || p->type != XR_TYPE_INSTANCE_PROPERTIES)
        return XR_ERROR_VALIDATION_FAILURE;
    p->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    std::strcpy(p->runtimeName, "OpenXR Simulator Linux Vulkan");
    return XR_SUCCESS;
}
FN(xrGetSystem)(XrInstance iHandle, const XrSystemGetInfo *info, XrSystemId *id)
{
    auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !id || info->type != XR_TYPE_SYSTEM_GET_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *id = 1;
    return XR_SUCCESS;
}
FN(xrGetSystemProperties)(XrInstance iHandle, XrSystemId id, XrSystemProperties *p)
{
    auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (id != 1)
        return XR_ERROR_SYSTEM_INVALID;
    auto validation = simxr::FillSimulationProperties(i->core->Extensions(), p);
    if (XR_FAILED(validation)) return validation;
    p->systemId = 1;
    p->vendorId = 0;
    std::strcpy(p->systemName, "Simulated Linux HMD");
    p->graphicsProperties = {8192, 8192, 16};
    p->trackingProperties = {XR_TRUE, XR_TRUE};
    return XR_SUCCESS;
}
static XrResult ValidateViewConfiguration(simxr::LinuxRuntime::InstanceRef instance, XrSystemId system, XrViewConfigurationType type)
{
    if (!sim::valid(instance)) return XR_ERROR_HANDLE_INVALID;
    if (system != 1) return XR_ERROR_SYSTEM_INVALID;
    if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ||
        (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
         instance->core->Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME))) return XR_SUCCESS;
    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
}
FN(xrEnumerateViewConfigurations)(XrInstance handle, XrSystemId system, uint32_t c, uint32_t *n, XrViewConfigurationType *p)
{
    auto instance = sim::Host().FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    const auto validation = ValidateViewConfiguration(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    if (XR_FAILED(validation)) return validation;
    std::vector<XrViewConfigurationType> types{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    if (instance->core->Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME))
        types.push_back(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
    return sim::enumerate(c, n, p, types);
}
FN(xrGetViewConfigurationProperties)(XrInstance handle, XrSystemId system, XrViewConfigurationType type,
                                     XrViewConfigurationProperties *p)
{
    auto instance = sim::Host().FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    const auto validation = ValidateViewConfiguration(instance, system, type);
    if (XR_FAILED(validation)) return validation;
    if (!p || p->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    if (XR_FAILED(simxr::StructureChain(p->next).Validate({}))) return XR_ERROR_VALIDATION_FAILURE;
    p->viewConfigurationType = type;
    p->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}
FN(xrEnumerateViewConfigurationViews)(XrInstance handle, XrSystemId system, XrViewConfigurationType type, uint32_t c, uint32_t *n,
                                      XrViewConfigurationView *p)
{
    auto instance = sim::Host().FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    const auto validation = ValidateViewConfiguration(instance, system, type);
    if (XR_FAILED(validation)) return validation;
    if (!n || (c && !p)) return XR_ERROR_VALIDATION_FAILURE;
    const uint32_t count = type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ? 4 : 2;
    *n = count;
    if (!c) return XR_SUCCESS;
    if (c < count) return XR_ERROR_SIZE_INSUFFICIENT;
    const bool foveationEnabled = instance->core->Extensions().Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
    const std::array known{XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO};
    for (uint32_t index = 0; index < count; ++index)
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
    uint32_t maxSamples = 1;
    const auto samplesResult = instance->graphics.MaxSampleCount(maxSamples);
    if (XR_FAILED(samplesResult)) return samplesResult;
    // Fixed and gaze-steered insets use the same qualified texture dimensions.
    const auto& settings = instance->Input().Views();
    for (uint32_t index = 0; index < count; ++index)
    {
        p[index].recommendedImageRectWidth = index < 2 ? settings.outerWidth : settings.insetWidth;
        p[index].recommendedImageRectHeight = index < 2 ? settings.outerHeight : settings.insetHeight;
        p[index].maxImageRectWidth = p[index].maxImageRectHeight = 8192;
        p[index].recommendedSwapchainSampleCount = 1;
        p[index].maxSwapchainSampleCount = maxSamples;
    }
    return XR_SUCCESS;
}
FN(xrEnumerateEnvironmentBlendModes)(XrInstance handle, XrSystemId system, XrViewConfigurationType type, uint32_t c, uint32_t *n,
                                     XrEnvironmentBlendMode *p)
{
    auto instance = sim::Host().FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    const auto validation = ValidateViewConfiguration(instance, system, type);
    if (XR_FAILED(validation)) return validation;
    return sim::enumerate(c, n, p, std::vector<XrEnvironmentBlendMode>{XR_ENVIRONMENT_BLEND_MODE_OPAQUE});
}
FN(xrGetVulkanGraphicsRequirementsKHR)(XrInstance handle, XrSystemId system, XrGraphicsRequirementsVulkanKHR* output)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.Requirements(system, output);
}
FN(xrGetVulkanGraphicsRequirements2KHR)(XrInstance handle, XrSystemId system, XrGraphicsRequirementsVulkanKHR* output)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.Requirements(system, output);
}
FN(xrGetVulkanInstanceExtensionsKHR)(XrInstance handle, XrSystemId system, std::uint32_t capacity,
									std::uint32_t* count, char* output)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.InstanceExtensions(system, capacity, count, output);
}
FN(xrGetVulkanDeviceExtensionsKHR)(XrInstance handle, XrSystemId system, std::uint32_t capacity,
								  std::uint32_t* count, char* output)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.DeviceExtensions(system, capacity, count, output);
}
FN(xrCreateVulkanInstanceKHR)(XrInstance handle, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* output,
							VkResult* result)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.CreateInstance(info, output, result);
}
FN(xrCreateVulkanDeviceKHR)(XrInstance handle, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* output, VkResult* result)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.CreateDevice(info, output, result);
}
FN(xrGetVulkanGraphicsDeviceKHR)(XrInstance handle, XrSystemId system, VkInstance graphicsInstance,
								VkPhysicalDevice* output)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.PhysicalDevice(system, graphicsInstance, output);
}
FN(xrGetVulkanGraphicsDevice2KHR)(XrInstance handle, const XrVulkanGraphicsDeviceGetInfoKHR* info,
								 VkPhysicalDevice* output)
{
	const auto instance = sim::Host().FindInstance(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!info || info->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	if (XR_FAILED(simxr::StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	return instance->graphics.PhysicalDevice(info->systemId, info->vulkanInstance, output);
}
FN(xrCreateSession)(XrInstance handle, const XrSessionCreateInfo* info, XrSession* output)
{
	const auto result = sim::Host().CreateSession(handle, info, output);
	if (XR_FAILED(result))
		return result;
	// Driver/validation DSOs register their exit handlers during native creation.
	// Close retained control readers and the retirement worker before those handlers.
	// Rearm after each creation because drivers may have been unloaded and reloaded.
	if (std::atexit(
			[]
			{
				sim::Host().Shutdown();
				simxr::ShutdownVulkanImageDelivery();
			}) != 0)
	{
		sim::Host().DestroySession(*output);
		*output = XR_NULL_HANDLE;
		return XR_ERROR_OUT_OF_MEMORY;
	}
	return result;
}
FN(xrBeginSession)(XrSession handle, const XrSessionBeginInfo* info)
{
	return sim::Host().BeginSession(handle, info);
}
FN(xrRequestExitSession)(XrSession handle)
{
	auto session = sim::Host().FindSession(handle);
	std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	auto result = session->core->RequestExit(simxr::LinuxRuntime::SessionTime(*session.Shared()));
	session->owner->PublishStatus(session->core);
	return result;
}
FN(xrEndSession)(XrSession handle)
{
	return sim::Host().EndSession(handle);
}
FN(xrPollEvent)(XrInstance iHandle, XrEventDataBuffer* p)
{
	auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (!p)
        return XR_ERROR_VALIDATION_FAILURE;
    return i->core->Events().Poll(*p);
}
FN(xrWaitFrame)(XrSession sHandle, const XrFrameWaitInfo *info, XrFrameState *p)
{
    auto s = sim::Host().FindSession(sHandle);
    std::unique_lock<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(s))
        return XR_ERROR_HANDLE_INVALID;
    if (!p || p->type != XR_TYPE_FRAME_STATE || (info && info->type != XR_TYPE_FRAME_WAIT_INFO))
        return XR_ERROR_VALIDATION_FAILURE;
    auto core = s->core.Shared();
    apiLock.unlock();
    simxr::FramePrediction prediction;
	const auto publishNative = [&](const simxr::FramePrediction& admitted) -> XrResult
	{
		std::lock_guard nativeLock(sim::Host().Mutex());
		if (!sim::valid(s) || s->core.Shared() != core)
			return XR_ERROR_HANDLE_INVALID;
		s->graphics->PollCaptures();
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
FN(xrBeginFrame)(XrSession sHandle, const XrFrameBeginInfo *info)
{
    auto s = sim::Host().FindSession(sHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(s))
        return XR_ERROR_HANDLE_INVALID;
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    return s->core->BeginFrame();
}
FN(xrLocateViews)(XrSession handle, const XrViewLocateInfo *info, XrViewState *state, uint32_t cap, uint32_t *count,
                  XrView *views)
{
    auto session = sim::Host().FindSession(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->LocateViews(*info, state, cap, count, views);
}
FN(xrEnumerateSwapchainFormats)(XrSession handle, uint32_t capacity, uint32_t* count, int64_t* formats)
{
	auto session = sim::Host().FindSession(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!count || (capacity && !formats))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto supported = simxr::VulkanSwapchain::Formats();
	*count = static_cast<uint32_t>(supported.size());
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < supported.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(supported.begin(), supported.end(), formats);
	return XR_SUCCESS;
}
FN(xrCreateSwapchain)(XrSession handle, const XrSwapchainCreateInfo* info, XrSwapchain* output)
{
	auto session = sim::Host().FindSession(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (session->core->Snapshot().closed)
		return XR_ERROR_SESSION_LOST;
	if (!info || !output)
		return XR_ERROR_VALIDATION_FAILURE;
	return session->graphics->CreateSwapchain(*info, *output);
}
FN(xrEnumerateSwapchainImages)(XrSwapchain handle, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
	std::lock_guard lock(sim::Host().Mutex());
	const auto session = sim::Host().CurrentSession();
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	return chain->EnumerateImages(capacity, count, images);
}
FN(xrAcquireSwapchainImage)(XrSwapchain handle, const XrSwapchainImageAcquireInfo* info, uint32_t* index)
{
	std::lock_guard lock(sim::Host().Mutex());
	const auto session = sim::Host().CurrentSession();
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (session->core->Snapshot().closed)
		return XR_ERROR_SESSION_LOST;
	if (!index || (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO))
		return XR_ERROR_VALIDATION_FAILURE;
	return session->graphics->Acquire(*chain.Shared(), *index);
}
FN(xrWaitSwapchainImage)(XrSwapchain handle, const XrSwapchainImageWaitInfo* info)
{
	std::unique_lock lock(sim::Host().Mutex());
	const auto session = sim::Host().CurrentSession();
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO || info->timeout < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	auto registered = session->graphics->EnterWait();
	if (!registered)
		return XR_ERROR_SESSION_LOST;
	lock.unlock();
	return chain->Wait(info->timeout);
}
FN(xrReleaseSwapchainImage)(XrSwapchain handle, const XrSwapchainImageReleaseInfo* info)
{
	std::lock_guard lock(sim::Host().Mutex());
	const auto session = sim::Host().CurrentSession();
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return chain->Release();
}
FN(xrDestroySwapchain)(XrSwapchain handle)
{
	std::lock_guard lock(sim::Host().Mutex());
	const auto session = sim::Host().CurrentSession();
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	session->graphics->PollCaptures();
	session->graphics->DestroySwapchain(chain);
	return XR_SUCCESS;
}
FN(xrEndFrame)(XrSession handle, const XrFrameEndInfo* info)
{
	const auto session = sim::Host().FindSession(handle);
	std::lock_guard lock(sim::Host().Mutex());
	if (!sim::valid(session))
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
FN(xrEnumerateReferenceSpaces)(XrSession sHandle, uint32_t c, uint32_t *n, XrReferenceSpaceType *p)
{
    auto s = sim::Host().FindSession(sHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(s))
        return XR_ERROR_HANDLE_INVALID;
    std::vector<XrReferenceSpaceType> types{XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                            XR_REFERENCE_SPACE_TYPE_STAGE};
    if (s->core->Extensions().Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME))
        types.push_back(XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO);
    return sim::enumerate(c, n, p, types);
}
FN(xrCreateReferenceSpace)(XrSession handle, const XrReferenceSpaceCreateInfo *info, XrSpace *out)
{
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Spaces().CreateReferenceSpace(info->referenceSpaceType, info->poseInReferenceSpace, *out);
}
FN(xrGetReferenceSpaceBoundsRect)(XrSession handle, XrReferenceSpaceType type, XrExtent2Df *bounds)
{
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Spaces().Bounds(type, *bounds);
}
FN(xrLocateSpace)(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::Host().CurrentSession()) return XR_ERROR_HANDLE_INVALID;
    return simxr::LocateSpace(sim::Host().CurrentSession()->core->Spaces(), space, base, time, location, &sim::Host().CurrentSession()->core->Extensions());
}
FN(xrDestroySpace)(XrSpace space)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::Host().CurrentSession()) return XR_ERROR_HANDLE_INVALID;
    return sim::Host().CurrentSession()->core->Spaces().Destroy(space);
}
FN(xrStringToPath)(XrInstance handle, const char *name, XrPath *path)
{
    auto instance = sim::Host().FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    if (!name || !path)
        return XR_ERROR_VALIDATION_FAILURE;
    auto end = std::find(name, name + XR_MAX_PATH_LENGTH, '\0');
    if (end == name + XR_MAX_PATH_LENGTH)
        return XR_ERROR_PATH_FORMAT_INVALID;
    return instance->core->Actions().StringToPath(std::string_view(name, end - name), *path);
}
FN(xrPathToString)(XrInstance handle, XrPath path, uint32_t capacity, uint32_t *count, char *buffer)
{
    auto instance = sim::Host().FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    return instance->core->Actions().PathToString(path, capacity, count, buffer);
}
FN(xrCreateActionSet)(XrInstance handle, const XrActionSetCreateInfo *info, XrActionSet *set)
{
    auto instance = sim::Host().FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !set)
        return XR_ERROR_VALIDATION_FAILURE;
    return instance->core->Actions().CreateSet(*info, *set);
}
FN(xrCreateAction)(XrActionSet set, const XrActionCreateInfo *info, XrAction *action)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    if (!sim::Host().CurrentInstance())
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !action)
        return XR_ERROR_VALIDATION_FAILURE;
    return sim::Host().CurrentInstance()->core->Actions().CreateAction(set, *info, *action);
}
FN(xrSuggestInteractionProfileBindings)(XrInstance handle, const XrInteractionProfileSuggestedBinding *info)
{
    auto instance = sim::Host().FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return instance->core->Actions().SuggestBindings(*info);
}
FN(xrAttachSessionActionSets)(XrSession handle, const XrSessionActionSetsAttachInfo *info)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Attach(*info);
}
FN(xrCreateActionSpace)(XrSession handle, const XrActionSpaceCreateInfo *info, XrSpace *out)
{
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Spaces().CreateActionSpace(info->action, info->subactionPath, info->poseInActionSpace, *out);
}
FN(xrSyncActions)(XrSession handle, const XrActionsSyncInfo *info)
{
    auto session = sim::Host().FindSession(handle);
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    if (!sim::valid(session))
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->SyncActions(*info, session->owner->Input().Controls());
}
FN(xrGetActionStateBoolean)(XrSession handle, const XrActionStateGetInfo *info, XrActionStateBoolean *state)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetBoolean(*info, *state);
}
FN(xrGetActionStateFloat)(XrSession handle, const XrActionStateGetInfo *info, XrActionStateFloat *state)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetFloat(*info, *state);
}
FN(xrGetActionStateVector2f)(XrSession handle, const XrActionStateGetInfo *info, XrActionStateVector2f *state)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetVector(*info, *state);
}
FN(xrGetActionStatePose)(XrSession handle, const XrActionStateGetInfo *info, XrActionStatePose *state)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetPose(*info, *state);
}
FN(xrGetCurrentInteractionProfile)(XrSession handle, XrPath user, XrInteractionProfileState *state)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!state || state->type != XR_TYPE_INTERACTION_PROFILE_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().CurrentProfile(user, state->interactionProfile);
}
FN(xrEnumerateBoundSourcesForAction)(XrSession handle, const XrBoundSourcesForActionEnumerateInfo *info,
                                     uint32_t capacity, uint32_t *count, XrPath *out)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    std::vector<XrPath> paths;
    auto result = session->core->Actions().BoundSources(info->action, paths);
    if (XR_FAILED(result))
        return result;
    return sim::enumerate(capacity, count, out, paths);
}
FN(xrGetInputSourceLocalizedName)(XrSession handle, const XrInputSourceLocalizedNameGetInfo *info,
                                  uint32_t capacity, uint32_t *count, char *out)
{
    auto session = sim::Host().FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !count || (capacity && !out) || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    std::string name;
    const auto result = session->core->Actions().LocalizedName(info->sourcePath, info->whichComponents, name);
    if (XR_FAILED(result))
        return result;
    *count = static_cast<uint32_t>(name.size() + 1);
    if (!capacity)
        return XR_SUCCESS;
    if (capacity < *count)
        return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(out, name.c_str(), *count);
    return XR_SUCCESS;
}
FN(xrApplyHapticFeedback)(XrSession handle, const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback)
{
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->ApplyHaptic(info, feedback);
}
FN(xrStopHapticFeedback)(XrSession handle, const XrHapticActionInfo* info)
{
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->StopHaptic(info);
}
FN(xrDestroyAction)(XrAction action)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    if (!sim::Host().CurrentInstance())
        return XR_ERROR_HANDLE_INVALID;
    if (sim::Host().CurrentSession())
    {
        sim::Host().CurrentSession()->core->Haptics().RemoveAction(action, sim::Host().CurrentSession()->core->Snapshot().time);
    }
    return sim::Host().CurrentInstance()->core->Actions().DestroyAction(action);
}
FN(xrDestroyActionSet)(XrActionSet set)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    if (!sim::Host().CurrentInstance())
        return XR_ERROR_HANDLE_INVALID;
    for (const auto& action : sim::Host().CurrentInstance()->core->Actions().Actions())
        if (action.set == set)
            run_xrDestroyAction(action.handle);
    return sim::Host().CurrentInstance()->core->Actions().DestroySet(set);
}
FN(xrDestroySession)(XrSession handle)
{
	return sim::Host().DestroySession(handle);
}
FN(xrEnumerateViveTrackerPathsHTCX)(XrInstance handle, uint32_t capacity, uint32_t* count, XrViveTrackerPathsHTCX* paths)
{
    auto instance = sim::Host().Core().FindInstance(handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    return instance->Trackers().Enumerate(capacity, count, paths);
}
FN(xrCreateBodyTrackerFB)(XrSession handle, const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().CreateFB(info, output);
}

FN(xrCreateBodyTrackerBD)(XrSession handle, const XrBodyTrackerCreateInfoBD* info, XrBodyTrackerBD* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().CreateBD(info, output);
}

FN(xrDestroyBodyTrackerFB)(XrBodyTrackerFB tracker)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().DestroyFB(tracker);
}

FN(xrDestroyBodyTrackerBD)(XrBodyTrackerBD tracker)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().DestroyBD(tracker);
}

FN(xrLocateBodyJointsFB)(XrBodyTrackerFB tracker, const XrBodyJointsLocateInfoFB* info, XrBodyJointLocationsFB* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().LocateFB(tracker, info, output);
}

FN(xrLocateBodyJointsBD)(XrBodyTrackerBD tracker, const XrBodyJointsLocateInfoBD* info, XrBodyJointLocationsBD* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().LocateBD(tracker, info, output);
}

FN(xrGetBodySkeletonFB)(XrBodyTrackerFB tracker, XrBodySkeletonFB* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().SkeletonFB(tracker, output);
}

FN(xrSuggestBodyTrackingCalibrationOverrideMETA)(XrBodyTrackerFB tracker, const XrBodyTrackingCalibrationInfoMETA* info)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().SuggestCalibrationFB(tracker, info);
}

FN(xrResetBodyTrackingCalibrationMETA)(XrBodyTrackerFB tracker)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().ResetCalibrationFB(tracker);
}

FN(xrRequestBodyTrackingFidelityMETA)(XrBodyTrackerFB tracker, XrBodyTrackingFidelityMETA fidelity)
{
    std::lock_guard<std::recursive_mutex> lock(sim::Host().Mutex());
    auto session = sim::Host().CurrentSession();
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().RequestFidelityFB(tracker, fidelity);
}
FN(xrDestroyInstance)(XrInstance iHandle)
{
	return sim::Host().DestroyInstance(iHandle);
}
FN(xrConvertTimespecTimeToTimeKHR)(XrInstance iHandle, const timespec *t, XrTime *p)
{
    auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(i))
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
FN(xrConvertTimeToTimespecTimeKHR)(XrInstance iHandle, XrTime t, timespec *p)
{
    auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (!i->core->Extensions().Contains(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME))
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!p)
        return XR_ERROR_VALIDATION_FAILURE;
    if (t <= 0)
        return XR_ERROR_TIME_INVALID;
    p->tv_sec = t / 1000000000;
    p->tv_nsec = t % 1000000000;
    return XR_SUCCESS;
}
FN(xrResultToString)(XrInstance, XrResult r, char *p)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!p)
        return XR_ERROR_VALIDATION_FAILURE;
    std::snprintf(p, XR_MAX_RESULT_STRING_SIZE, "XrResult(%d)", r);
    return XR_SUCCESS;
}
FN(xrStructureTypeToString)(XrInstance, XrStructureType t, char *p)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!p)
        return XR_ERROR_VALIDATION_FAILURE;
    std::snprintf(p, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType(%d)", t);
    return XR_SUCCESS;
}
#define ENTRY(name) {#name, (PFN_xrVoidFunction)simxr::AbiBoundary<run_##name>::Invoke}
FN(xrGetInstanceProcAddr)(XrInstance iHandle, const char *name, PFN_xrVoidFunction *p)
{
    auto i = sim::Host().FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::Host().Mutex());
    if (!name || !p)
        return XR_ERROR_VALIDATION_FAILURE;
    *p = nullptr;
    if (iHandle && !sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    static const simxr::DispatchEntry entries[] = {
                              {"xrGetInstanceProcAddr", (PFN_xrVoidFunction)simxr::AbiBoundary<run_xrGetInstanceProcAddr>::Invoke, true},
                              {"xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<run_xrEnumerateInstanceExtensionProperties>::Invoke, true},
                              {"xrCreateInstance", (PFN_xrVoidFunction)simxr::AbiBoundary<run_xrCreateInstance>::Invoke, true},
                              ENTRY(xrDestroyInstance),
                              ENTRY(xrGetInstanceProperties),
                              ENTRY(xrGetSystem),
                              ENTRY(xrGetSystemProperties),
                              ENTRY(xrEnumerateViewConfigurations),
                              ENTRY(xrGetViewConfigurationProperties),
                              ENTRY(xrEnumerateViewConfigurationViews),
                              ENTRY(xrEnumerateEnvironmentBlendModes),
                              ENTRY(xrGetVulkanGraphicsRequirementsKHR),
                              ENTRY(xrGetVulkanGraphicsRequirements2KHR),
                              ENTRY(xrGetVulkanInstanceExtensionsKHR),
                              ENTRY(xrGetVulkanDeviceExtensionsKHR),
                              ENTRY(xrGetVulkanGraphicsDeviceKHR),
                              ENTRY(xrGetVulkanGraphicsDevice2KHR),
                              ENTRY(xrCreateVulkanInstanceKHR),
                              ENTRY(xrCreateVulkanDeviceKHR),
                              ENTRY(xrEnumerateViveTrackerPathsHTCX),
                              ENTRY(xrCreateBodyTrackerFB),
                              ENTRY(xrCreateBodyTrackerBD),
                              ENTRY(xrDestroyBodyTrackerFB),
                              ENTRY(xrDestroyBodyTrackerBD),
                              ENTRY(xrLocateBodyJointsFB),
                              ENTRY(xrLocateBodyJointsBD),
                              ENTRY(xrGetBodySkeletonFB),
                              ENTRY(xrSuggestBodyTrackingCalibrationOverrideMETA),
                              ENTRY(xrResetBodyTrackingCalibrationMETA),
                              ENTRY(xrRequestBodyTrackingFidelityMETA),
                              ENTRY(xrCreateSession),
                              ENTRY(xrDestroySession),
                              ENTRY(xrBeginSession),
                              ENTRY(xrEndSession),
                              ENTRY(xrRequestExitSession),
                              ENTRY(xrPollEvent),
                              ENTRY(xrWaitFrame),
                              ENTRY(xrBeginFrame),
                              ENTRY(xrEndFrame),
                              ENTRY(xrLocateViews),
                              ENTRY(xrEnumerateSwapchainFormats),
                              ENTRY(xrCreateSwapchain),
                              ENTRY(xrDestroySwapchain),
                              ENTRY(xrEnumerateSwapchainImages),
                              ENTRY(xrAcquireSwapchainImage),
                              ENTRY(xrWaitSwapchainImage),
                              ENTRY(xrReleaseSwapchainImage),
                              ENTRY(xrEnumerateReferenceSpaces),
                              ENTRY(xrCreateReferenceSpace),
                              ENTRY(xrGetReferenceSpaceBoundsRect),
                              ENTRY(xrLocateSpace),
                              ENTRY(xrDestroySpace),
                              ENTRY(xrStringToPath),
                              ENTRY(xrPathToString),
                              ENTRY(xrCreateActionSet),
                              ENTRY(xrDestroyActionSet),
                              ENTRY(xrCreateAction),
                              ENTRY(xrDestroyAction),
                              ENTRY(xrSuggestInteractionProfileBindings),
                              ENTRY(xrAttachSessionActionSets),
                              ENTRY(xrCreateActionSpace),
                              ENTRY(xrSyncActions),
                              ENTRY(xrGetActionStateBoolean),
                              ENTRY(xrGetActionStateFloat),
                              ENTRY(xrGetActionStateVector2f),
                              ENTRY(xrGetActionStatePose),
                              ENTRY(xrGetCurrentInteractionProfile),
                              ENTRY(xrEnumerateBoundSourcesForAction),
                              ENTRY(xrGetInputSourceLocalizedName),
                              ENTRY(xrApplyHapticFeedback),
                              ENTRY(xrStopHapticFeedback),
                              ENTRY(xrResultToString),
                              ENTRY(xrStructureTypeToString),
                              ENTRY(xrConvertTimespecTimeToTimeKHR),
                              ENTRY(xrConvertTimeToTimespecTimeKHR)};
    static const simxr::DispatchTable table(entries, simxr::LinuxRuntime::Extensions());
    if (!table.CoversAdvertisedCommands())
        return XR_ERROR_RUNTIME_FAILURE;
    return table.Resolve(name, i ? &i->core->Extensions() : nullptr, *p);
}
extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loader, XrNegotiateRuntimeRequest *runtime)
{
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
    runtime->getInstanceProcAddr = simxr::AbiBoundary<run_xrGetInstanceProcAddr>::Invoke;
    return XR_SUCCESS;
}

extern "C" __attribute__((visibility("default"))) int simxrGetGameBridgeApi(std::uint32_t version, SimxrGameBridgeApi* api) noexcept
{
    try
    {
        if (!api || version != SIMXR_GAME_BRIDGE_VERSION || api->size < sizeof(*api)) return 0;
        sim::Host().StartControlEndpoint();
        if (!sim::Host().Core().Game().FillApi(version, *api)) return 0;
        api->shutdown = [](void*) -> int
        {
            try
            {
                return sim::Host().Shutdown() ? 1 : 0;
            }
            catch (...) { return 0; }
        };
        return 1;
    }
    catch (...) { return 0; }
}
