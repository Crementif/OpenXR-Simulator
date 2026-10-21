#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include "flicker_detector.h"
#include "Math.h"
#include "runtime/EventQueue.h"
#include "runtime/DispatchTable.h"
#include "runtime/HandleRegistry.h"
#include "runtime/EntryPoints.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"
#include "runtime/Runtime.h"
#include "runtime/NativeBindings.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"
#include "graphics/LayerBuilder.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "control/legacy/backends/vulkan/VulkanLegacyCapture.h"
#include "runtime/validation/StructureChain.h"
#include "control/ControlService.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "control/LocalControlServer.h"
#include "control/legacy/LegacyInput.h"
#include "control/legacy/LegacyFileChannel.h"
#include "simulation/HapticState.h"
#include "simulation/HeadsetProfiles.h"
#include "presentation/FrameScheduler.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
#include "platform/linux/VulkanSessionGraphics.h"
#include "platform/linux/VulkanInstanceGraphics.h"
#include "platform/linux/VulkanFrameSubmission.h"
#include "platform/linux/LinuxRuntimeStatus.h"
#include <algorithm>
#include <charconv>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <loader_interfaces.h>
#include <memory>
#include <limits>
#include <mutex>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

struct InstanceData;
struct SessionData;

namespace sim
{
using InstanceRef = simxr::HandleRef<XrInstance, InstanceData>;
using SessionRef = simxr::HandleRef<XrSession, SessionData>;
static simxr::NativeBindings<XrInstance, simxr::Instance, InstanceData> instances;
static simxr::NativeBindings<XrSession, simxr::Session, SessionData> sessions;
static simxr::Runtime& RuntimeState();
static InstanceRef FindInstance(XrInstance handle)
{
    return instances.Find(RuntimeState().FindInstance(handle));
}
static SessionRef FindSession(XrSession handle)
{
    return sessions.Find(RuntimeState().FindSession(handle));
}
// xrWaitFrame may run on a host frame-pump thread while xrEndFrame runs on
// the rendering thread. Serialize shared pose, lifecycle and capture state.
static std::recursive_mutex runtimeMutex;
static std::mutex instanceLifecycleMutex;
static XrTime MonotonicTime()
{
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
static std::string dataDir()
{
    static const std::string path = [] {
        const char *explicitPath = std::getenv("SIMXR_DATA_DIR");
        if (explicitPath && *explicitPath)
            return std::string(explicitPath);
        const char *base = std::getenv("XDG_DATA_HOME");
        if (base && *base)
            return std::string(base) + "/OpenXR-Simulator";
        const char *home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.local/share/OpenXR-Simulator";
    }();
    return path;
}
static void log(const std::string& message);
} // namespace sim
struct InstanceData
{
	InstanceData(simxr::Runtime::InstanceRef instance, std::shared_ptr<simxr::LegacyFileChannel> channel,
				 std::shared_ptr<flicker::Diagnostics> observer, simxr::ExecutionMode executionMode)
		: core(std::move(instance)), files(std::move(channel)), diagnostics(std::move(observer)), mode(executionMode),
		  status(files, getpid(), mode)
	{
	}
	const simxr::Runtime::InstanceRef core;
	simxr::LegacyInput legacyInput;
	const std::shared_ptr<simxr::LegacyFileChannel> files;
	const std::shared_ptr<flicker::Diagnostics> diagnostics;
	const simxr::ExecutionMode mode;
	simxr::VulkanInstanceGraphics graphics;
	simxr::LinuxRuntimeStatus status;
};
struct SessionData
{
	SessionData(sim::InstanceRef instance, simxr::Runtime::SessionRef session,
				std::unique_ptr<simxr::VulkanSessionGraphics> native)
		: owner(std::move(instance)), core(std::move(session)), graphics(std::move(native))
	{
	}
	const sim::InstanceRef owner;
	const simxr::Runtime::SessionRef core;
	const std::unique_ptr<simxr::VulkanSessionGraphics> graphics;
};
namespace sim
{
static InstanceRef instance;
static SessionRef session;
class SessionCreation final
{
  public:
	explicit SessionCreation(simxr::Runtime::SessionRef core) : m_core(std::move(core))
	{
	}
	~SessionCreation()
	{
		if (m_committed)
			return;
		RuntimeState().DestroySession(m_core);
	}
	SessionCreation(const SessionCreation&) = delete;
	SessionCreation& operator=(const SessionCreation&) = delete;
	void Commit() noexcept
	{
		m_committed = true;
	}

  private:
	const simxr::Runtime::SessionRef m_core;
	bool m_committed = false;
};
static bool valid(XrInstance i)
{
    return static_cast<bool>(RuntimeState().FindInstance(i));
}
static bool valid(XrSession s)
{
    return static_cast<bool>(RuntimeState().FindSession(s));
}
static void log(const std::string& message)
{
    if (instance)
        instance->files->Log(message);
}
static void PollInputCommands(SessionData& native, simxr::FrameId submitted)
{
    auto& target = *native.owner.Shared();
    auto commands = target.files->TakeCommands();
    static constexpr std::array kinds{
        simxr::LegacyInputCommand::HeadPose, simxr::LegacyInputCommand::Ipd,
        simxr::LegacyInputCommand::Fov, simxr::LegacyInputCommand::ControllerPose,
        simxr::LegacyInputCommand::ControllerHold};
    for (std::size_t index = 0; index < kinds.size(); ++index)
        if (commands.input[index])
            target.legacyInput.Apply(kinds[index], *commands.input[index]);
    if (commands.sequence)
        target.legacyInput.StartSequence(std::move(*commands.sequence), submitted);
    if (target.legacyInput.AdvanceSequence(submitted))
        native.graphics->RequestCapture({"both", "all"}, submitted);
}
static simxr::RuntimeTime SessionTime(const SessionData& session)
{
	if (session.owner->mode == simxr::ExecutionMode::Deterministic)
		return session.core->Snapshot().time;
	return {simxr::Duration{MonotonicTime()}};
}
static void PublishStatus(const InstanceRef& target, const simxr::Runtime::SessionRef& current = {},
						  std::optional<simxr::LinuxRuntimeStatus::FrameCounts> submitted = {}) noexcept
{
	try
	{
		const auto snapshot = current ? current->Snapshot() : simxr::SessionSnapshot{};
		target->status.Update(snapshot, target->legacyInput, simxr::LinuxRuntimeStatus::Clock::now(), submitted);
	}
	catch (...)
	{
		try
		{
			target->files->Log("Runtime status observation failed");
		}
		catch (...)
		{
		}
	}
}
static const simxr::ExtensionRegistry& Extensions()
{
	static constexpr std::array<std::string_view, 4> vulkanCommands{
		"xrGetVulkanInstanceExtensionsKHR", "xrGetVulkanDeviceExtensionsKHR", "xrGetVulkanGraphicsDeviceKHR",
		"xrGetVulkanGraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 4> vulkan2Commands{
		"xrCreateVulkanInstanceKHR", "xrCreateVulkanDeviceKHR", "xrGetVulkanGraphicsDevice2KHR",
		"xrGetVulkanGraphicsRequirements2KHR"};
	static constexpr std::array<std::string_view, 2> timeCommands{"xrConvertTimespecTimeToTimeKHR",
																  "xrConvertTimeToTimespecTimeKHR"};
	static const std::vector<simxr::ExtensionDefinition> definitions = [] {
        std::vector<simxr::ExtensionDefinition> result{
            {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION, vulkanCommands},
            {XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION, {}},
            {XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_KHR_convert_timespec_time_SPEC_VERSION, timeCommands}};
        const auto simulation = simxr::SimulationExtensions({true});
        result.insert(result.end(), simulation.begin(), simulation.end());
        if (!std::getenv("SIMXR_VULKAN_V1_ONLY"))
            result.push_back({XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION, vulkan2Commands});
        return result;
    }();
    static const simxr::ExtensionRegistry registry(definitions);
    return registry;
}
static simxr::Runtime& RuntimeState()
{
    // Its observer outlives the runtime and endpoint workers; no GPU is created.
    simxr::InitializeVulkanImageDelivery();
    static simxr::Runtime runtime(Extensions());
    return runtime;
}
struct Endpoint
{
        simxr::control::ControlService service{RuntimeState(), 16,
            simxr::control::CreateVulkanImageInspectionProvider([](XrSession handle) {
                const auto native = FindSession(handle);
                return native ? native->graphics->Capabilities() : simxr::control::ImageCapabilities{};
            })};
        std::unique_ptr<simxr::control::LocalControlServer> server;
    Endpoint()
        {
            try
            {
                const auto* selected = std::getenv("SIMXR_CONTROL_SOCKET");
                const auto path = selected && *selected ? std::string(selected)
                    : dataDir() + "/control." + std::to_string(getpid()) + ".sock";
                server = std::make_unique<simxr::control::LocalControlServer>(service, path);
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr, "SimXR: local control endpoint unavailable: %s\n", error.what());
            }
        }
};
static Endpoint& ControlEndpoint()
{
    static Endpoint endpoint;
    return endpoint;
}
static void StartControlEndpoint()
{
    (void)ControlEndpoint();
}
static void CloseControlEndpoint()
{
    auto& endpoint = ControlEndpoint();
    if (endpoint.server) endpoint.server->Close();
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
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (layer)
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    return sim::Extensions().Enumerate(cap, n, p);
}
FN(xrCreateInstance)(const XrInstanceCreateInfo *info, XrInstance *out)
{
    std::lock_guard lifecycleLock(sim::instanceLifecycleMutex);
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    simxr::Runtime::InstanceRef core;
    const auto result = sim::RuntimeState().CreateInstance(info, core);
    if (XR_FAILED(result)) return result;
    struct InstanceCreation
    {
        simxr::Runtime::InstanceRef core;
        bool committed = false;
        ~InstanceCreation() { if (!committed) sim::RuntimeState().DestroyInstance(core); }
    } creation{core};
    const auto* sequencePath = std::getenv("SIMXR_SEQUENCE");
    auto files = std::make_shared<simxr::LegacyFileChannel>(sim::dataDir(), sequencePath ? sequencePath : "");
    auto diagnostics = std::make_shared<flicker::Diagnostics>(sim::dataDir());
    sim::StartControlEndpoint();
    std::unique_lock<std::recursive_mutex> lock(sim::runtimeMutex);
    try
    {
        const auto* deterministic = std::getenv("SIMXR_DETERMINISTIC");
        const auto mode = deterministic && std::strcmp(deterministic, "0") != 0
            ? simxr::ExecutionMode::Deterministic : simxr::ExecutionMode::RealTime;
        auto instance = sim::instances.Attach(core, std::make_unique<InstanceData>(core, files, diagnostics, mode));
        sim::instance = instance;
        auto& input = instance->legacyInput;
        auto views = input.Views();
        if (const auto* name = std::getenv("SIMXR_HEADSET_PROFILE"))
        {
            const auto* profile = simxr::FindHeadsetProfile(name);
            if (!profile) throw std::invalid_argument("Unknown SIMXR_HEADSET_PROFILE");
            views = simxr::ProfileViews(*profile);
        }
        if (auto v = std::getenv("SIMXR_WIDTH"))
            views.outerWidth = std::clamp(std::atoi(v), 16, 8192);
        if (auto v = std::getenv("SIMXR_HEIGHT"))
            views.outerHeight = std::clamp(std::atoi(v), 16, 8192);
        input.SetViews(views);
        sim::log(std::string("create instance app=") + info->applicationInfo.applicationName);
        *out = instance;
        creation.committed = true;
        return XR_SUCCESS;
    }
    catch (...)
    {
        sim::instances.Remove(core);
        auto failedInstance = std::move(sim::instance);
        lock.unlock();
        if (failedInstance) failedInstance->files->Close();
        throw;
    }
}
FN(xrGetInstanceProperties)(XrInstance iHandle, XrInstanceProperties *p)
{
    auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    p->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    std::strcpy(p->runtimeName, "OpenXR Simulator Linux Vulkan");
    return XR_SUCCESS;
}
FN(xrGetSystem)(XrInstance iHandle, const XrSystemGetInfo *info, XrSystemId *id)
{
    auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *id = 1;
    return XR_SUCCESS;
}
FN(xrGetSystemProperties)(XrInstance iHandle, XrSystemId id, XrSystemProperties *p)
{
    auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
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
static XrResult ValidateViewConfiguration(sim::InstanceRef instance, XrSystemId system, XrViewConfigurationType type)
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
    auto instance = sim::FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
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
    auto instance = sim::FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
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
    auto instance = sim::FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    const auto validation = ValidateViewConfiguration(instance, system, type);
    if (XR_FAILED(validation)) return validation;
    if (!n || (c && !p)) return XR_ERROR_VALIDATION_FAILURE;
    const uint32_t count = type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ? 4 : 2;
    *n = count;
    if (!c) return XR_SUCCESS;
    if (c < count) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t index = 0; index < count; ++index)
        if (p[index].type != XR_TYPE_VIEW_CONFIGURATION_VIEW || XR_FAILED(simxr::StructureChain(p[index].next).Validate({})))
            return XR_ERROR_VALIDATION_FAILURE;
    const auto& settings = instance->legacyInput.Views();
    for (uint32_t index = 0; index < count; ++index)
    {
        p[index].recommendedImageRectWidth = index < 2 ? settings.outerWidth : settings.insetWidth;
        p[index].recommendedImageRectHeight = index < 2 ? settings.outerHeight : settings.insetHeight;
        p[index].maxImageRectWidth = p[index].maxImageRectHeight = 8192;
        p[index].recommendedSwapchainSampleCount = p[index].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
FN(xrEnumerateEnvironmentBlendModes)(XrInstance handle, XrSystemId system, XrViewConfigurationType type, uint32_t c, uint32_t *n,
                                     XrEnvironmentBlendMode *p)
{
    auto instance = sim::FindInstance(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    const auto validation = ValidateViewConfiguration(instance, system, type);
    if (XR_FAILED(validation)) return validation;
    return sim::enumerate(c, n, p, std::vector<XrEnvironmentBlendMode>{XR_ENVIRONMENT_BLEND_MODE_OPAQUE});
}
FN(xrGetVulkanGraphicsRequirementsKHR)(XrInstance handle, XrSystemId system, XrGraphicsRequirementsVulkanKHR* output)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.Requirements(system, output);
}
FN(xrGetVulkanGraphicsRequirements2KHR)(XrInstance handle, XrSystemId system, XrGraphicsRequirementsVulkanKHR* output)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.Requirements(system, output);
}
FN(xrGetVulkanInstanceExtensionsKHR)(XrInstance handle, XrSystemId system, std::uint32_t capacity,
									std::uint32_t* count, char* output)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.InstanceExtensions(system, capacity, count, output);
}
FN(xrGetVulkanDeviceExtensionsKHR)(XrInstance handle, XrSystemId system, std::uint32_t capacity,
								  std::uint32_t* count, char* output)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.DeviceExtensions(system, capacity, count, output);
}
FN(xrCreateVulkanInstanceKHR)(XrInstance handle, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* output,
							VkResult* result)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.CreateInstance(info, output, result);
}
FN(xrCreateVulkanDeviceKHR)(XrInstance handle, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* output, VkResult* result)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.CreateDevice(info, output, result);
}
FN(xrGetVulkanGraphicsDeviceKHR)(XrInstance handle, XrSystemId system, VkInstance graphicsInstance,
								VkPhysicalDevice* output)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!instance->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return instance->graphics.PhysicalDevice(system, graphicsInstance, output);
}
FN(xrGetVulkanGraphicsDevice2KHR)(XrInstance handle, const XrVulkanGraphicsDeviceGetInfoKHR* info,
								 VkPhysicalDevice* output)
{
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
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
	const auto instance = sim::FindInstance(handle);
	std::lock_guard lock(sim::runtimeMutex);
	if (!sim::valid(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (sim::session)
		return XR_ERROR_LIMIT_REACHED;
	if (!info || !output || info->type != XR_TYPE_SESSION_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->systemId != 1)
		return XR_ERROR_SYSTEM_INVALID;
	const auto& extensions = instance->core->Extensions();
	if (!extensions.Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
		!extensions.Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	simxr::VulkanBinding binding{};
	const auto validated = instance->graphics.SessionBinding(*info, binding);
	if (XR_FAILED(validated))
		return validated;
	simxr::Runtime::SessionRef core;
	auto result = sim::RuntimeState().CreateSession(instance->core, core);
	if (XR_FAILED(result))
		return result;
	sim::SessionCreation creation(core);
	std::unique_ptr<simxr::VulkanSessionGraphics> graphics;
	const auto* requestedValidation = std::getenv("SIMXR_VULKAN_VALIDATION");
	const bool validation = requestedValidation && std::strcmp(requestedValidation, "1") == 0;
	result = simxr::VulkanSessionGraphics::Create(binding, core.Shared(), instance->files, instance->diagnostics,
												  validation, graphics);
	if (XR_FAILED(result))
		return result;
	auto native = std::make_unique<SessionData>(instance, core, std::move(graphics));
	result = core->Ready(
		{simxr::Duration{instance->mode == simxr::ExecutionMode::Deterministic ? 1000000000LL : sim::MonotonicTime()}});
	if (XR_FAILED(result))
		return result;
	sim::log("create Vulkan session queueFamily=" + std::to_string(binding.queueFamily) +
			 " queueIndex=" + std::to_string(binding.queueIndex));
	sim::PublishStatus(instance, core);
	// No fallible work after native publication.
	const auto attached = sim::sessions.Attach(core, std::move(native));
	sim::session = attached;
	*output = attached;
	creation.Commit();
	return XR_SUCCESS;
}
FN(xrBeginSession)(XrSession handle, const XrSessionBeginInfo* info)
{
	auto session = sim::FindSession(handle);
	std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	simxr::FrameSchedulerConfig config;
	config.epoch = {simxr::Duration{
		session->owner->mode == simxr::ExecutionMode::Deterministic ? 1000000000LL : sim::MonotonicTime()}};
	config.mode = session->owner->mode;
	std::uint64_t seed = 0;
	if (const auto* text = std::getenv("SIMXR_SEED"))
	{
		const std::string_view value{text};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
		if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
			return XR_ERROR_VALIDATION_FAILURE;
	}
	simxr::CommandConfiguration baseline;
	baseline.tracking = session->owner->legacyInput.Tracking();
	baseline.input = session->core->Input().Snapshot();
	baseline.input.controllers = session->owner->legacyInput.Controls();
	baseline.views = session->owner->legacyInput.Views();
	auto configured = session->core->ConfigureCommands(seed, baseline, false);
	if (XR_FAILED(configured))
		return configured;
	auto result = session->core->Begin(*info, config);
	return result;
}
FN(xrRequestExitSession)(XrSession handle)
{
	auto session = sim::FindSession(handle);
	std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	auto result = session->core->RequestExit(sim::SessionTime(*session.Shared()));
	sim::PublishStatus(session->owner, session->core);
	return result;
}
FN(xrEndSession)(XrSession handle)
{
	auto session = sim::FindSession(handle);
	std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	auto result = session->core->End(sim::SessionTime(*session.Shared()));
	sim::PublishStatus(session->owner, session->core);
	return result;
}
FN(xrPollEvent)(XrInstance iHandle, XrEventDataBuffer* p)
{
	auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (!p)
        return XR_ERROR_VALIDATION_FAILURE;
    return i->core->Events().Poll(*p);
}
FN(xrWaitFrame)(XrSession sHandle, const XrFrameWaitInfo *info, XrFrameState *p)
{
    auto s = sim::FindSession(sHandle);
    std::unique_lock<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(s))
        return XR_ERROR_HANDLE_INVALID;
    if (!p || p->type != XR_TYPE_FRAME_STATE || (info && info->type != XR_TYPE_FRAME_WAIT_INFO))
        return XR_ERROR_VALIDATION_FAILURE;
    auto core = s->core.Shared();
    apiLock.unlock();
    simxr::FramePrediction prediction;
    auto result = core->WaitFrame(prediction);
    apiLock.lock();
    if (!sim::valid(s) || s->core.Shared() != core)
        return XR_ERROR_HANDLE_INVALID;
    if (XR_FAILED(result))
        return result;
    s->graphics->PollCaptures();
    sim::PollInputCommands(*s.Shared(), s->core->Snapshot().lastSubmitted);
    const auto& input = s->owner->legacyInput;
    const auto tracking = input.Tracking(s->core->Snapshot().state == XR_SESSION_STATE_FOCUSED);
    s->core->PublishTracking(prediction.displayTime, tracking, input.Views(), prediction.id);
    p->predictedDisplayTime = s->core->Snapshot().lastIssued.displayTime.value.count();
    p->predictedDisplayPeriod = prediction.displayPeriod.count();
    p->shouldRender = s->core->Snapshot().state == XR_SESSION_STATE_VISIBLE || s->core->Snapshot().state == XR_SESSION_STATE_FOCUSED;
    return XR_SUCCESS;
}
FN(xrBeginFrame)(XrSession sHandle, const XrFrameBeginInfo *info)
{
    auto s = sim::FindSession(sHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(s))
        return XR_ERROR_HANDLE_INVALID;
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    return s->core->BeginFrame();
}
FN(xrLocateViews)(XrSession handle, const XrViewLocateInfo *info, XrViewState *state, uint32_t cap, uint32_t *count,
                  XrView *views)
{
    auto session = sim::FindSession(handle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->LocateViews(*info, state, cap, count, views);
}
FN(xrEnumerateSwapchainFormats)(XrSession handle, uint32_t capacity, uint32_t* count, int64_t* formats)
{
	auto session = sim::FindSession(handle);
	std::lock_guard lock(sim::runtimeMutex);
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
	auto session = sim::FindSession(handle);
	std::lock_guard lock(sim::runtimeMutex);
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
	std::lock_guard lock(sim::runtimeMutex);
	const auto session = sim::session;
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	return chain->EnumerateImages(capacity, count, images);
}
FN(xrAcquireSwapchainImage)(XrSwapchain handle, const XrSwapchainImageAcquireInfo* info, uint32_t* index)
{
	std::lock_guard lock(sim::runtimeMutex);
	const auto session = sim::session;
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
	std::unique_lock lock(sim::runtimeMutex);
	const auto session = sim::session;
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
	std::lock_guard lock(sim::runtimeMutex);
	const auto session = sim::session;
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return chain->Release();
}
FN(xrDestroySwapchain)(XrSwapchain handle)
{
	std::lock_guard lock(sim::runtimeMutex);
	const auto session = sim::session;
	const auto chain = session ? session->graphics->FindSwapchain(handle) : simxr::VulkanSessionGraphics::SwapchainRef{};
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	session->graphics->PollCaptures();
	session->graphics->DestroySwapchain(chain);
	return XR_SUCCESS;
}
FN(xrEndFrame)(XrSession handle, const XrFrameEndInfo* info)
{
	const auto session = sim::FindSession(handle);
	std::lock_guard lock(sim::runtimeMutex);
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
		sim::PublishStatus(session->owner, session->core,
						   simxr::LinuxRuntimeStatus::FrameCounts{info->layerCount, projectionCount});
	}
	catch (const std::exception& error)
	{
		try
		{
			session->owner->files->Log(std::string("submitted frame observer failed: ") + error.what());
		}
		catch (...)
		{
		}
	}
	catch (...)
	{
		try
		{
			session->owner->files->Log("submitted frame observer failed");
		}
		catch (...)
		{
		}
	}
	return XR_SUCCESS;
}
FN(xrEnumerateReferenceSpaces)(XrSession sHandle, uint32_t c, uint32_t *n, XrReferenceSpaceType *p)
{
    auto s = sim::FindSession(sHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(s))
        return XR_ERROR_HANDLE_INVALID;
    return sim::enumerate(c, n, p,
                          std::vector<XrReferenceSpaceType>{XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                            XR_REFERENCE_SPACE_TYPE_STAGE});
}
FN(xrCreateReferenceSpace)(XrSession handle, const XrReferenceSpaceCreateInfo *info, XrSpace *out)
{
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Spaces().CreateReferenceSpace(info->referenceSpaceType, info->poseInReferenceSpace, *out);
}
FN(xrGetReferenceSpaceBoundsRect)(XrSession handle, XrReferenceSpaceType type, XrExtent2Df *bounds)
{
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Spaces().Bounds(type, *bounds);
}
FN(xrLocateSpace)(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::session) return XR_ERROR_HANDLE_INVALID;
    return simxr::LocateSpace(sim::session->core->Spaces(), space, base, time, location, &sim::session->core->Extensions());
}
FN(xrDestroySpace)(XrSpace space)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::session) return XR_ERROR_HANDLE_INVALID;
    return sim::session->core->Spaces().Destroy(space);
}
FN(xrStringToPath)(XrInstance handle, const char *name, XrPath *path)
{
    auto instance = sim::FindInstance(handle);
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
    auto instance = sim::FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    return instance->core->Actions().PathToString(path, capacity, count, buffer);
}
FN(xrCreateActionSet)(XrInstance handle, const XrActionSetCreateInfo *info, XrActionSet *set)
{
    auto instance = sim::FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !set)
        return XR_ERROR_VALIDATION_FAILURE;
    return instance->core->Actions().CreateSet(*info, *set);
}
FN(xrCreateAction)(XrActionSet set, const XrActionCreateInfo *info, XrAction *action)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    if (!sim::instance)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !action)
        return XR_ERROR_VALIDATION_FAILURE;
    return sim::instance->core->Actions().CreateAction(set, *info, *action);
}
FN(xrSuggestInteractionProfileBindings)(XrInstance handle, const XrInteractionProfileSuggestedBinding *info)
{
    auto instance = sim::FindInstance(handle);
    if (!instance)
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return instance->core->Actions().SuggestBindings(*info);
}
FN(xrAttachSessionActionSets)(XrSession handle, const XrSessionActionSetsAttachInfo *info)
{
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Attach(*info);
}
FN(xrCreateActionSpace)(XrSession handle, const XrActionSpaceCreateInfo *info, XrSpace *out)
{
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Spaces().CreateActionSpace(info->action, info->subactionPath, info->poseInActionSpace, *out);
}
FN(xrSyncActions)(XrSession handle, const XrActionsSyncInfo *info)
{
    auto session = sim::FindSession(handle);
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    if (!sim::valid(session))
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->SyncActions(*info, session->owner->legacyInput.Controls());
}
FN(xrGetActionStateBoolean)(XrSession handle, const XrActionStateGetInfo *info, XrActionStateBoolean *state)
{
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetBoolean(*info, *state);
}
FN(xrGetActionStateFloat)(XrSession handle, const XrActionStateGetInfo *info, XrActionStateFloat *state)
{
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetFloat(*info, *state);
}
FN(xrGetActionStateVector2f)(XrSession handle, const XrActionStateGetInfo *info, XrActionStateVector2f *state)
{
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetVector(*info, *state);
}
FN(xrGetActionStatePose)(XrSession handle, const XrActionStateGetInfo *info, XrActionStatePose *state)
{
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().GetPose(*info, *state);
}
FN(xrGetCurrentInteractionProfile)(XrSession handle, XrPath user, XrInteractionProfileState *state)
{
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!state || state->type != XR_TYPE_INTERACTION_PROFILE_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    return session->core->Actions().CurrentProfile(user, state->interactionProfile);
}
FN(xrEnumerateBoundSourcesForAction)(XrSession handle, const XrBoundSourcesForActionEnumerateInfo *info,
                                     uint32_t capacity, uint32_t *count, XrPath *out)
{
    auto session = sim::FindSession(handle);
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
    auto session = sim::FindSession(handle);
    if (!session)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !count || (capacity && !out) || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    auto& registry = session->owner->core->Actions();
    auto source = registry.PathString(info->sourcePath);
    if (!source)
        return XR_ERROR_PATH_INVALID;
    XrPath user;
    auto result = registry.StringToPath(source->starts_with("/user/hand/left/") ? "/user/hand/left" : "/user/hand/right", user);
    if (XR_FAILED(result))
        return result;
    XrPath profile;
    result = session->core->Actions().CurrentProfile(user, profile);
    if (XR_FAILED(result))
        return result;
    std::string name;
    result = registry.LocalizedName(info->sourcePath, profile, info->whichComponents, name);
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
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->ApplyHaptic(info, feedback);
}
FN(xrStopHapticFeedback)(XrSession handle, const XrHapticActionInfo* info)
{
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->StopHaptic(info);
}
FN(xrDestroyAction)(XrAction action)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    if (!sim::instance)
        return XR_ERROR_HANDLE_INVALID;
    if (sim::session)
    {
        sim::session->core->Haptics().RemoveAction(action, sim::session->core->Snapshot().time);
    }
    return sim::instance->core->Actions().DestroyAction(action);
}
FN(xrDestroyActionSet)(XrActionSet set)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    if (!sim::instance)
        return XR_ERROR_HANDLE_INVALID;
    for (const auto& action : sim::instance->core->Actions().Actions())
        if (action.set == set)
            run_xrDestroyAction(action.handle);
    return sim::instance->core->Actions().DestroySet(set);
}
FN(xrDestroySession)(XrSession handle)
{
	const auto session = sim::FindSession(handle);
	std::unique_lock lock(sim::runtimeMutex);
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	session->graphics->BeginClose();
	lock.unlock();
	sim::RuntimeState().CloseDebugger(session);
	const auto drained = session->graphics->Drain();
	if (XR_FAILED(drained))
		return drained;
	lock.lock();
	if (!sim::valid(session))
		return XR_ERROR_HANDLE_INVALID;
	session->core->Display().PollRetired();
	session->core->Captures().Poll();
	const auto closed = session->graphics->FinishClose();
	if (XR_FAILED(closed))
		return closed;
	sim::RuntimeState().DestroySession(session);
	session->owner->core->Events().RemoveSession(session);
	sim::sessions.Remove(session);
	sim::session = {};
	sim::PublishStatus(session->owner);
	return XR_SUCCESS;
}
FN(xrEnumerateViveTrackerPathsHTCX)(XrInstance handle, uint32_t capacity, uint32_t* count, XrViveTrackerPathsHTCX* paths)
{
    auto instance = sim::RuntimeState().FindInstance(handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    return instance->Trackers().Enumerate(capacity, count, paths);
}
FN(xrCreateBodyTrackerFB)(XrSession handle, const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().CreateFB(info, output);
}

FN(xrCreateBodyTrackerBD)(XrSession handle, const XrBodyTrackerCreateInfoBD* info, XrBodyTrackerBD* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::FindSession(handle);
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().CreateBD(info, output);
}

FN(xrDestroyBodyTrackerFB)(XrBodyTrackerFB tracker)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().DestroyFB(tracker);
}

FN(xrDestroyBodyTrackerBD)(XrBodyTrackerBD tracker)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().DestroyBD(tracker);
}

FN(xrLocateBodyJointsFB)(XrBodyTrackerFB tracker, const XrBodyJointsLocateInfoFB* info, XrBodyJointLocationsFB* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().LocateFB(tracker, info, output);
}

FN(xrLocateBodyJointsBD)(XrBodyTrackerBD tracker, const XrBodyJointsLocateInfoBD* info, XrBodyJointLocationsBD* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().LocateBD(tracker, info, output);
}

FN(xrGetBodySkeletonFB)(XrBodyTrackerFB tracker, XrBodySkeletonFB* output)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().SkeletonFB(tracker, output);
}

FN(xrSuggestBodyTrackingCalibrationOverrideMETA)(XrBodyTrackerFB tracker, const XrBodyTrackingCalibrationInfoMETA* info)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().SuggestCalibrationFB(tracker, info);
}

FN(xrResetBodyTrackingCalibrationMETA)(XrBodyTrackerFB tracker)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().ResetCalibrationFB(tracker);
}

FN(xrRequestBodyTrackingFidelityMETA)(XrBodyTrackerFB tracker, XrBodyTrackingFidelityMETA fidelity)
{
    std::lock_guard<std::recursive_mutex> lock(sim::runtimeMutex);
    auto session = sim::session;
    if (!session) return XR_ERROR_HANDLE_INVALID;
    return session->core->Body().RequestFidelityFB(tracker, fidelity);
}
FN(xrDestroyInstance)(XrInstance iHandle)
{
    std::lock_guard lifecycleLock(sim::instanceLifecycleMutex);
    auto i = sim::FindInstance(iHandle);
    std::unique_lock<std::recursive_mutex> apiLock(sim::runtimeMutex);
    if (!sim::valid(i))
        return XR_ERROR_HANDLE_INVALID;
    if (sim::session)
    {
        const auto session = sim::session;
        apiLock.unlock();
        const auto result = run_xrDestroySession(session);
        if (XR_FAILED(result)) return result;
        apiLock.lock();
    }
    sim::log("destroy instance");
    sim::RuntimeState().DestroyInstance(i);
    sim::instances.Remove(i);
    sim::instance = {};
    apiLock.unlock();
    i->diagnostics->Close();
    i->files->Close();
    return XR_SUCCESS;
}
FN(xrConvertTimespecTimeToTimeKHR)(XrInstance iHandle, const timespec *t, XrTime *p)
{
    auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
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
    auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
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
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    std::snprintf(p, XR_MAX_RESULT_STRING_SIZE, "XrResult(%d)", r);
    return XR_SUCCESS;
}
FN(xrStructureTypeToString)(XrInstance, XrStructureType t, char *p)
{
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
    std::snprintf(p, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType(%d)", t);
    return XR_SUCCESS;
}
#define ENTRY(name) {#name, (PFN_xrVoidFunction)simxr::AbiBoundary<run_##name>::Invoke}
FN(xrGetInstanceProcAddr)(XrInstance iHandle, const char *name, PFN_xrVoidFunction *p)
{
    auto i = sim::FindInstance(iHandle);
    std::lock_guard<std::recursive_mutex> apiLock(sim::runtimeMutex);
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
    static const simxr::DispatchTable table(entries, sim::Extensions());
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
        sim::StartControlEndpoint();
        if (!sim::RuntimeState().Game().FillApi(version, *api)) return 0;
        api->shutdown = [](void*) -> int
        {
            try
            {
                if (!sim::RuntimeState().Instances().empty()) return 0;
                sim::RuntimeState().Close();
                sim::CloseControlEndpoint();
                return 1;
            }
            catch (...) { return 0; }
        };
        return 1;
    }
    catch (...) { return 0; }
}
