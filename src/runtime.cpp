// Windows OpenXR loader entry points and native runtime composition.

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN

#include "Pch.h"

#include "simxr/Version.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "control/legacy/WindowsLegacyCapture.h"
#include "control/legacy/WindowsLegacyChannel.h"
#include "control/legacy/WindowsRuntimeSettings.h"
#include "control/legacy/backends/vulkan/VulkanLegacyCapture.h"
#include "control/legacy/backends/vulkan/WindowsVulkanCaptureChannel.h"
#include "flicker_detector.h"
#include "graphics/LayerBuilder.h"
#include "graphics/SwapchainAccess.h"
#include "graphics/SwapchainWaits.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "graphics/backends/vulkan/VulkanSessionGraphics.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
#include "graphics/backends/windows/WindowsGraphicsCapabilities.h"
#include "graphics/backends/windows/WindowsSessionGraphics.h"
#include "platform/windows/Direct3DSwapchain.h"
#include "platform/windows/OpenGLSwapchain.h"
#include "platform/windows/WindowsClock.h"
#include "platform/windows/WindowsRuntimeInput.h"
#include "platform/windows/WindowsRuntimePaths.h"
#include "presentation/FrameScheduler.h"
#include "runtime/DispatchTable.h"
#include "runtime/EntryPoints.h"
#include "runtime/EnumNames.h"
#include "runtime/EventQueue.h"
#include "runtime/Instance.h"
#include "runtime/NativeBindings.h"
#include "runtime/Runtime.h"
#include "runtime/Session.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"
#include "runtime/validation/LayerValidation.h"
#include "runtime/validation/StructureChain.h"
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <loader_interfaces.h>
#include <openxr/openxr_platform.h>
namespace settings = simxr::legacy::settings;

using Microsoft::WRL::ComPtr;

static simxr::Runtime& RuntimeState();

// File output is owned by the active instance's joined compatibility worker.
static std::mutex s_logMutex;
static std::weak_ptr<mcp::WindowsLegacyChannel> s_logChannel;
static const bool s_logVerbose = []()
{
	char v[16]{};
	const DWORD n = GetEnvironmentVariableA("SIMXR_VERBOSE", v, (DWORD)sizeof(v));
	return n > 0 && n < sizeof(v) && v[0] != '0';
}();

static void Log(const char* msg)
{
	std::shared_ptr<mcp::WindowsLegacyChannel> files;
	{
		std::lock_guard lock(s_logMutex);
		files = s_logChannel.lock();
	}
	if (files)
	{
		try
		{
			files->Log(msg);
		}
		catch (...)
		{
		}
	}
	else
	{
		OutputDebugStringA(msg); // Loader/startup diagnostics before an instance exists.
	}
}
static void Logf(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Log(buf);
}
// Runtime state
namespace rt
{

struct Instance
{
	std::mutex lifecycleMutex;
	std::mutex graphicsMutex;
	simxr::VulkanInstanceGraphics vulkan;
	XrInstance handle{XR_NULL_HANDLE};
	std::shared_ptr<simxr::Instance> core;
	std::shared_ptr<simxr::windows::WindowsRuntimeInput> input;
	std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
	std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
	std::shared_ptr<flicker::Diagnostics> diagnostics;
	bool closing = false;
};
using InstanceRef = simxr::HandleRef<XrInstance, Instance>;

struct Swapchain
{
	XrSwapchain handle{XR_NULL_HANDLE};
	std::int64_t format = 0;
	uint32_t width{0}, height{0}, arraySize{2};
	uint32_t mipCount{1};
	uint32_t sampleCount{1};
	// Backend type and images
	enum class Backend
	{
		D3D11,
		D3D12,
		OpenGL
	} backend{Backend::D3D11};
	std::unique_ptr<simxr::windows::Direct3DSwapchain> d3dImages;
	std::unique_ptr<simxr::windows::OpenGLSwapchain> glImages;
	std::shared_ptr<simxr::SwapchainAccess> access;
};

struct Session
{
	~Session()
	{
		// Also stop capture admission when native publication fails inside Attach.
		if (core)
			core->Close();
	}

	InstanceRef owner;
	std::mutex graphicsMutex;
	std::unordered_map<XrSwapchain, Swapchain> swapchains;
	XrSession handle{XR_NULL_HANDLE};
	std::shared_ptr<simxr::Session> core;
	std::shared_ptr<simxr::windows::WindowsRuntimeInput> input;
	std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
	std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
	std::shared_ptr<flicker::Diagnostics> diagnostics;
	ComPtr<ID3D11Device> d3d11Device;
	// DX12 support
	ComPtr<ID3D12Device> d3d12Device;
	ComPtr<ID3D12CommandQueue> d3d12Queue;
	bool usesD3D12 = false;
	std::shared_ptr<simxr::VulkanSessionGraphics> vulkan;
	std::shared_ptr<simxr::WindowsSessionGraphics> nativeGraphics;
	std::shared_ptr<simxr::SwapchainWaits> nativeWaits;
	bool nativeClosing = false;
	std::uint64_t acquireCount = 0;
	std::uint64_t releaseCount = 0;
	// OpenGL support
	HDC glDC{nullptr};
	HGLRC glRC{nullptr};
	bool usesOpenGL{false};
};

using SessionRef = simxr::HandleRef<XrSession, Session>;

struct NativeState
{
	std::mutex mutex;
	XrInstance currentInstance = XR_NULL_HANDLE;
	XrSession currentSession = XR_NULL_HANDLE;
	bool creatingInstance = false;
	simxr::NativeBindings<XrInstance, simxr::Instance, Instance> instances;
	simxr::NativeBindings<XrSession, simxr::Session, Session> sessions;
};

static NativeState& State()
{
	// Native cleanup may retain core objects or reenter the binding tables.
	(void)RuntimeState();
	static NativeState state;
	return state;
}

static InstanceRef RetainInstance(XrInstance handle)
{
	return State().instances.Find(RuntimeState().FindInstance(handle));
}
static InstanceRef RetainInstance()
{
	XrInstance handle;
	{
		std::lock_guard lock(State().mutex);
		handle = State().currentInstance;
	}
	return RetainInstance(handle);
}
static SessionRef RetainNativeSession(XrSession handle)
{
	return State().sessions.Find(RuntimeState().FindSession(handle));
}
static SessionRef RetainNativeSession()
{
	XrSession handle;
	{
		std::lock_guard lock(State().mutex);
		handle = State().currentSession;
	}
	return RetainNativeSession(handle);
}
static std::shared_ptr<simxr::Session> RetainSession(XrSession handle)
{
	const auto native = RetainNativeSession(handle);
	return native ? native->core : nullptr;
}
static std::shared_ptr<simxr::Session> RetainSession()
{
	const auto native = RetainNativeSession();
	return native ? native->core : nullptr;
}

} // namespace rt

static const char* SessionStateName(XrSessionState state)
{
	switch (state)
	{
	case XR_SESSION_STATE_UNKNOWN:
		return "UNKNOWN";
	case XR_SESSION_STATE_IDLE:
		return "IDLE";
	case XR_SESSION_STATE_READY:
		return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED:
		return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE:
		return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED:
		return "FOCUSED";
	case XR_SESSION_STATE_STOPPING:
		return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING:
		return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING:
		return "EXITING";
	default:
		return "UNKNOWN";
	}
}

static constexpr XrVersion s_supportedApiVersion = XR_MAKE_VERSION(1, 0, 27);

static const simxr::ExtensionRegistry& Extensions()
{
	static constexpr std::array<std::string_view, 1> d3d11Commands{"xrGetD3D11GraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 1> d3d12Commands{"xrGetD3D12GraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 1> openGLCommands{"xrGetOpenGLGraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 4> vulkanCommands{
		"xrGetVulkanInstanceExtensionsKHR", "xrGetVulkanDeviceExtensionsKHR", "xrGetVulkanGraphicsDeviceKHR",
		"xrGetVulkanGraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 4> vulkan2Commands{
		"xrCreateVulkanInstanceKHR", "xrCreateVulkanDeviceKHR", "xrGetVulkanGraphicsDevice2KHR",
		"xrGetVulkanGraphicsRequirements2KHR"};
	static constexpr std::array<std::string_view, 2> timeCommands{"xrConvertWin32PerformanceCounterToTimeKHR",
																  "xrConvertTimeToWin32PerformanceCounterKHR"};
	static const std::vector<simxr::ExtensionDefinition> definitions = []
	{
		std::vector<simxr::ExtensionDefinition> result{
			{XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D11_enable_SPEC_VERSION, d3d11Commands},
			{XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_enable_SPEC_VERSION, d3d12Commands},
			{XR_KHR_OPENGL_ENABLE_EXTENSION_NAME, XR_KHR_opengl_enable_SPEC_VERSION, openGLCommands},
			{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION, vulkanCommands},
			{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION, vulkan2Commands},
			{XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION, {}},
			{XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
			 XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION, timeCommands}};
		const auto simulation = simxr::SimulationExtensions({true, true});
		result.insert(result.end(), simulation.begin(), simulation.end());
		return result;
	}();
	static const simxr::ExtensionRegistry registry(definitions);
	return registry;
}

static simxr::Runtime& RuntimeState()
{
	simxr::InitializeVulkanImageDelivery();
	static simxr::Runtime runtime(Extensions());
	return runtime;
}

struct NativeControlEndpoint
{
	std::mutex mutex;
	std::unique_ptr<simxr::control::ControlService> service;
	std::unique_ptr<simxr::control::LocalControlServer> server;
	bool permanentlyClosed = false;
	~NativeControlEndpoint()
	{
		Close();
	}
	bool Start()
	{
		std::lock_guard lock(mutex);
		if (permanentlyClosed)
			return false;
		if (server)
			return true;
		try
		{
			const auto* selected = std::getenv("SIMXR_CONTROL_SOCKET");
			const auto path = selected && *selected
								  ? std::string(selected)
								  : std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId());
			auto nextService = std::make_unique<simxr::control::ControlService>(
				RuntimeState(), 16,
				simxr::control::CreateVulkanImageInspectionProvider(
					[](XrSession handle)
					{
						const auto native = rt::RetainNativeSession(handle);
						if (!native)
							return simxr::control::ImageCapabilities{};
						std::lock_guard lock(native->graphicsMutex);
						if (native->nativeGraphics)
							return native->nativeGraphics->Capabilities();
						return native->vulkan ? native->vulkan->Capabilities() : simxr::control::ImageCapabilities{};
					}));
			auto nextServer = std::make_unique<simxr::control::LocalControlServer>(*nextService, path, 16);
			service = std::move(nextService);
			server = std::move(nextServer);
			Logf("[SimXR] Local control endpoint: %s (control/inspection; session-dependent native GPU transport)",
				 path.c_str());
			return true;
		}
		catch (const std::exception& error)
		{
			Logf("[SimXR] Local control endpoint unavailable: %s", error.what());
			return false;
		}
	}
	void Close(bool permanent = false) noexcept
	{
		std::unique_ptr<simxr::control::ControlService> previousService;
		std::unique_ptr<simxr::control::LocalControlServer> previousServer;
		{
			std::lock_guard lock(mutex);
			permanentlyClosed = permanentlyClosed || permanent;
			previousServer = std::move(server);
			previousService = std::move(service);
		}
		// No runtime/native lock spans transport joins or lease/input cleanup.
		if (previousServer)
			previousServer->Close();
		if (previousService)
			previousService->Close();
	}
};
static NativeControlEndpoint& ControlEndpoint()
{
	// Endpoint workers retire before bindings, which retire before the core runtime.
	(void)rt::State();
	static NativeControlEndpoint endpoint;
	return endpoint;
}
extern "C" __declspec(dllexport) int simxrGetGameBridgeApi(std::uint32_t version, SimxrGameBridgeApi* api) noexcept
{
	if (!api || version != SIMXR_GAME_BRIDGE_VERSION || api->size < sizeof(*api))
		return 0;
	try
	{
		if (!ControlEndpoint().Start())
			return 0;
		if (!RuntimeState().Game().FillApi(version, *api))
			return 0;
		api->shutdown = [](void*) -> int
		{
			try
			{
				if (!RuntimeState().Instances().empty())
					return 0;
				// The caller stops XR initialization before explicit shutdown.
				// Runtime.Close permanently rejects new instances; native joins
				// run here, outside DLL detach and outside runtime graph locks.
				RuntimeState().Close();
				ControlEndpoint().Close(true);
				return 1;
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

static XrResult RequireExtension(XrInstance instance, std::string_view extension)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Extensions().Contains(extension) ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

static XrResult RequireGraphicsExtension(XrInstance instance, XrSystemId system, std::string_view extension)
{
	auto result = RequireExtension(instance, extension);
	if (XR_FAILED(result))
		return result;
	return system == 1 ? XR_SUCCESS : XR_ERROR_SYSTEM_INVALID;
}

// ----------------- OpenXR runtime exports -----------------

static XrResult XRAPI_PTR xrGetInstanceProcAddr_runtime(XrInstance, const char* name, PFN_xrVoidFunction* fn);
static XrResult XRAPI_PTR xrDestroySession_runtime(XrSession session);
static XrResult DestroyNativeSession(const rt::SessionRef& session);
static XrResult XRAPI_PTR xrDestroySwapchain_runtime(XrSwapchain swapchain);

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest)
{

	try
	{
		Log("\n[SimXR] ========== OpenXR Simulator Runtime Starting ==========\n");
		if (!loaderInfo || !runtimeRequest)
		{
			Log("[SimXR] xrNegotiateLoaderRuntimeInterface: ERROR - null parameters");
			return XR_ERROR_INITIALIZATION_FAILED;
		}

		// The loader FreeLibrary's the runtime after xrDestroyInstance. That drops the last
		// reference this process may hold on d3d12/dxgi/opengl32, and unloading those out
		// from under a live Vulkan driver - the same driver, in a Vulkan session - hangs the
		// process.
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
							   (LPCWSTR)&xrNegotiateLoaderRuntimeInterface, &self);
		}

		Logf("[SimXR] xrNegotiateLoaderRuntimeInterface: loaderInfo=%p, runtimeRequest=%p", loaderInfo, runtimeRequest);
		Logf("[SimXR]   Loader minInterfaceVersion=%u, maxInterfaceVersion=%u, minApiVersion=0x%llX, "
			 "maxApiVersion=0x%llX",
			 loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion,
			 static_cast<unsigned long long>(loaderInfo->minApiVersion),
			 static_cast<unsigned long long>(loaderInfo->maxApiVersion));

		if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
			loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
			loaderInfo->structSize != sizeof(*loaderInfo) ||
			runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
			runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
			runtimeRequest->structSize != sizeof(*runtimeRequest) ||
			loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
			loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION ||
			loaderInfo->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loaderInfo->minApiVersion > s_supportedApiVersion)
			return XR_ERROR_INITIALIZATION_FAILED;

		runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
		runtimeRequest->getInstanceProcAddr = simxr::AbiBoundary<xrGetInstanceProcAddr_runtime>::Invoke;
		runtimeRequest->runtimeApiVersion = std::min(loaderInfo->maxApiVersion, s_supportedApiVersion);

		Logf("[SimXR] xrNegotiateLoaderRuntimeInterface: SUCCESS - runtimeApiVersion=0x%llX",
			 static_cast<unsigned long long>(runtimeRequest->runtimeApiVersion));
		return XR_SUCCESS;
	}
	catch (...)
	{
		Log("[SimXR] xrNegotiateLoaderRuntimeInterface: EXCEPTION caught!");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
}

static XrResult XRAPI_PTR xrGetD3D12GraphicsRequirementsKHR_runtime(XrInstance instance, XrSystemId system,
																	XrGraphicsRequirementsD3D12KHR* requirements)
{
	const auto result = RequireGraphicsExtension(instance, system, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!requirements || requirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	LUID adapter{};
	const auto found = simxr::windows::Direct3DAdapterLuid(adapter);
	if (XR_FAILED(found))
		return found;
	requirements->adapterLuid = adapter;
	requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetD3D11GraphicsRequirementsKHR_runtime(XrInstance instance, XrSystemId system,
																	XrGraphicsRequirementsD3D11KHR* requirements)
{
	const auto result = RequireGraphicsExtension(instance, system, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!requirements || requirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	LUID adapter{};
	const auto found = simxr::windows::Direct3DAdapterLuid(adapter);
	if (XR_FAILED(found))
		return found;
	requirements->adapterLuid = adapter;
	requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	return XR_SUCCESS;
}

// xrGetOpenGLGraphicsRequirementsKHR (XR_KHR_opengl_enable)
static XrResult XRAPI_PTR xrGetOpenGLGraphicsRequirementsKHR_runtime(XrInstance instance, XrSystemId systemId,
																	 XrGraphicsRequirementsOpenGLKHR* req)
{
	auto result = RequireGraphicsExtension(instance, systemId, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR)
		return XR_ERROR_VALIDATION_FAILURE;

	Logf("[SimXR] xrGetOpenGLGraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p", instance,
		 (unsigned long long)systemId, req);
	if (!req)
	{
		Log("[SimXR] xrGetOpenGLGraphicsRequirementsKHR: ERROR - null req");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;

	// Canonical GLSL and direct-state-access image storage require OpenGL 4.5.
	req->minApiVersionSupported = XR_MAKE_VERSION(4, 5, 0);
	req->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);

	Logf("[SimXR] xrGetOpenGLGraphicsRequirementsKHR: min=%d.%d.%d, max=%d.%d.%d",
		 XR_VERSION_MAJOR(req->minApiVersionSupported), XR_VERSION_MINOR(req->minApiVersionSupported),
		 XR_VERSION_PATCH(req->minApiVersionSupported), XR_VERSION_MAJOR(req->maxApiVersionSupported),
		 XR_VERSION_MINOR(req->maxApiVersionSupported), XR_VERSION_PATCH(req->maxApiVersionSupported));

	Log("[SimXR] xrGetOpenGLGraphicsRequirementsKHR: SUCCESS");
	return XR_SUCCESS;
}

// --- XR_KHR_vulkan_enable / XR_KHR_vulkan_enable2 -------------------------------------------

static XrResult GetVulkanRequirements(XrInstance instance, XrSystemId system,
									  XrGraphicsRequirementsVulkanKHR* requirements, std::string_view extension)
{
	auto result = RequireGraphicsExtension(instance, system, extension);
	if (XR_FAILED(result))
		return result;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.Requirements(system, requirements);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsRequirementsKHR_runtime(XrInstance instance, XrSystemId system,
																	 XrGraphicsRequirementsVulkanKHR* requirements)
{
	return GetVulkanRequirements(instance, system, requirements, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsRequirements2KHR_runtime(XrInstance instance, XrSystemId system,
																	  XrGraphicsRequirementsVulkan2KHR* requirements)
{
	return GetVulkanRequirements(instance, system, requirements, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
}

static XrResult XRAPI_PTR xrGetVulkanInstanceExtensionsKHR_runtime(XrInstance instance, XrSystemId system,
																   uint32_t capacity, uint32_t* countOutput,
																   char* buffer)
{
	auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.InstanceExtensions(system, capacity, countOutput, buffer);
}

static XrResult XRAPI_PTR xrGetVulkanDeviceExtensionsKHR_runtime(XrInstance instance, XrSystemId system,
																 uint32_t capacity, uint32_t* countOutput, char* buffer)
{
	auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.DeviceExtensions(system, capacity, countOutput, buffer);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsDeviceKHR_runtime(XrInstance instance, XrSystemId system,
															   VkInstance vkInstance, VkPhysicalDevice* out)
{
	auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.PhysicalDevice(system, vkInstance, out);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsDevice2KHR_runtime(XrInstance instance,
																const XrVulkanGraphicsDeviceGetInfoKHR* getInfo,
																VkPhysicalDevice* out)
{
	auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!getInfo || getInfo->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.PhysicalDevice(getInfo->systemId, getInfo->vulkanInstance, out);
}

// Thin passthrough: the app's own vkCreateInstance runs, with the interop extensions the
// runtime needs appended to whatever it asked for.
static XrResult XRAPI_PTR xrCreateVulkanInstanceKHR_runtime(XrInstance instance,
															const XrVulkanInstanceCreateInfoKHR* createInfo,
															VkInstance* vulkanInstance, VkResult* vulkanResult)
{
	auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.CreateInstance(createInfo, vulkanInstance, vulkanResult);
}

static XrResult XRAPI_PTR xrCreateVulkanDeviceKHR_runtime(XrInstance instance,
														  const XrVulkanDeviceCreateInfoKHR* createInfo,
														  VkDevice* vulkanDevice, VkResult* vulkanResult)
{
	auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	const auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	return native->vulkan.CreateDevice(createInfo, vulkanDevice, vulkanResult);
}

// --- Minimal implementations ---

static XrResult XRAPI_PTR xrEnumerateApiLayerProperties_runtime(uint32_t propertyCapacityInput,
																uint32_t* propertyCountOutput,
																XrApiLayerProperties* properties)
{
	Log("[SimXR] xrEnumerateApiLayerProperties called");
	if (!propertyCountOutput || (propertyCapacityInput && !properties))
		return XR_ERROR_VALIDATION_FAILURE;
	*propertyCountOutput = 0;
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrEnumerateInstanceExtensionProperties_runtime(const char* layerName, uint32_t capacity,
																		 uint32_t* count,
																		 XrExtensionProperties* properties)
{
	if (layerName)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	return Extensions().Enumerate(capacity, count, properties);
}

static XrResult XRAPI_PTR xrCreateInstance_runtime(const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
	if (!instance)
		return XR_ERROR_VALIDATION_FAILURE;
	{
		std::lock_guard lock(rt::State().mutex);
		if (rt::State().creatingInstance || rt::State().currentInstance != XR_NULL_HANDLE)
			return XR_ERROR_LIMIT_REACHED;
		rt::State().creatingInstance = true;
	}
	struct InstanceCreation
	{
		simxr::Runtime::InstanceRef core;
		bool committed = false;
		~InstanceCreation()
		{
			if (!committed && core)
			{
				RuntimeState().DestroyInstance(core);
				if (!RuntimeState().Game().Snapshot().game)
					ControlEndpoint().Close();
				simxr::StopVulkanImageDelivery();
			}
			std::lock_guard lock(rt::State().mutex);
			rt::State().creatingInstance = false;
		}
	} creation;
	auto result = RuntimeState().CreateInstance(createInfo, creation.core);
	if (XR_FAILED(result))
		return result;
	auto native = std::make_unique<rt::Instance>();
	native->handle = creation.core;
	native->core = creation.core.Shared();
	native->input = std::make_shared<simxr::windows::WindowsRuntimeInput>();
	Logf("[SimXR] xrCreateInstance: app=%s version=%u", createInfo->applicationInfo.applicationName,
		 createInfo->applicationInfo.applicationVersion);

	static bool s_settingsLoaded = false;
	if (!s_settingsLoaded)
	{
		settings::LoadSettings(simxr::windows::RuntimeDataDirectory());
		s_settingsLoaded = true;
	}
	if (const auto* profile = std::getenv("SIMXR_HEADSET_PROFILE"))
	{
		const auto index = settings::FindHeadsetSpec(profile);
		if (index < 0)
			return XR_ERROR_INITIALIZATION_FAILED;
		settings::SetHeadsetProfile(static_cast<settings::HeadsetProfile>(index));
	}
	const auto directory = simxr::windows::RuntimeDataDirectory();
	native->diagnostics = std::make_shared<flicker::Diagnostics>(directory);
	native->compatibilityFiles = std::make_shared<mcp::WindowsLegacyChannel>(directory);
	native->compatibilityCapture = std::make_shared<mcp::WindowsLegacyCapture>(native->compatibilityFiles);
	ControlEndpoint().Start();
	const auto attached = rt::State().instances.Attach(creation.core, std::move(native));
	{
		std::lock_guard lock(s_logMutex);
		s_logChannel = attached->compatibilityFiles;
	}
	{
		std::lock_guard lock(rt::State().mutex);
		rt::State().currentInstance = attached;
	}
	*instance = attached;
	creation.committed = true;
	Log("[SimXR] xrCreateInstance: SUCCESS");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroyInstance_runtime(XrInstance instance)
{
	auto native = rt::RetainInstance(instance);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::unique_lock lifecycle(native->lifecycleMutex);
	if (!rt::RetainInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	native->closing = true;
	{
		const auto session = rt::RetainNativeSession();
		if (session && session->owner.Shared() == native.Shared())
		{
			const auto result = DestroyNativeSession(session);
			if (XR_FAILED(result))
				return result;
			const auto destroyed = RuntimeState().DestroySession(session);
			if (XR_FAILED(destroyed))
				return destroyed;
			rt::State().sessions.Remove(session);
			std::lock_guard lock(rt::State().mutex);
			if (rt::State().currentSession == session)
				rt::State().currentSession = XR_NULL_HANDLE;
		}
	}
	if (!RuntimeState().Game().Snapshot().game)
		ControlEndpoint().Close();
	native->compatibilityCapture->Close();
	native->compatibilityFiles->Close();
	native->diagnostics->Close();
	simxr::StopVulkanImageDelivery();
	const auto result = RuntimeState().DestroyInstance(instance);
	if (XR_FAILED(result))
		return result;
	rt::State().instances.Remove(instance);
	{
		std::lock_guard lock(s_logMutex);
		s_logChannel.reset();
	}
	lifecycle.unlock();
	native = {};
	{
		std::lock_guard lock(rt::State().mutex);
		if (rt::State().currentInstance == instance)
			rt::State().currentInstance = XR_NULL_HANDLE;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInstanceProperties_runtime(XrInstance instance, XrInstanceProperties* props)
{
	if (!RuntimeState().FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!props || props->type != XR_TYPE_INSTANCE_PROPERTIES)
		return XR_ERROR_VALIDATION_FAILURE;
	props->runtimeVersion = simxr::RuntimeVersion;
	strncpy(props->runtimeName, "OpenXR Simulator Runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
	props->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
	Log("[SimXR] xrGetInstanceProperties: returning OpenXR Simulator Runtime");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystem_runtime(XrInstance instance, const XrSystemGetInfo* info, XrSystemId* systemId)
{
	if (!RuntimeState().FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !systemId || info->type != XR_TYPE_SYSTEM_GET_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	Logf("[SimXR] xrGetSystem: formFactor=%d", info->formFactor);
	if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
	{
		Log("[SimXR] xrGetSystem: ERROR - form factor not HMD");
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	}
	*systemId = (XrSystemId)1;
	Log("[SimXR] xrGetSystem: SUCCESS -> systemId=1");
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystemProperties_runtime(XrInstance instance, XrSystemId system,
														XrSystemProperties* props)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	auto result = simxr::FillSimulationProperties(core->Extensions(), props);
	if (XR_FAILED(result))
		return result;
	std::strcpy(props->systemName, "OpenXR Simulator");
	props->systemId = 1;
	props->vendorId = 0;
	props->graphicsProperties = {4096, 4096, 16};
	props->trackingProperties = {XR_TRUE, XR_TRUE};
	return XR_SUCCESS;
}

static XrResult ValidateViewConfiguration(const simxr::EnabledExtensions& extensions, XrSystemId system,
										  XrViewConfigurationType type)
{
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO || (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
															  extensions.Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)))
		return XR_SUCCESS;
	return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurations_runtime(XrInstance instance, XrSystemId system,
																uint32_t capacity, uint32_t* count,
																XrViewConfigurationType* types)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (!count || (capacity && !types))
		return XR_ERROR_VALIDATION_FAILURE;
	const bool quadViews = core->Extensions().Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
	*count = quadViews ? 2 : 1;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (quadViews)
		types[1] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
	return XR_SUCCESS;
}

static XrResult GetMaxSwapchainSampleCount(const simxr::EnabledExtensions& extensions, uint32_t& output)
{
	output = 1;
	const bool openGL = extensions.Contains(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
	if (openGL)
	{
		const auto result = simxr::windows::OpenGLSwapchain::MaxSampleCount(output);
		if (XR_FAILED(result))
			return result;
	}
	const bool d3d11 = extensions.Contains(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
	const bool d3d12 = extensions.Contains(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
	if (d3d11 || d3d12)
	{
		uint32_t direct3DSamples = 1;
		const auto result = simxr::windows::Direct3DMaxSampleCount(d3d11, d3d12, direct3DSamples);
		if (XR_FAILED(result))
			return result;
		output = openGL ? std::min(output, direct3DSamples) : direct3DSamples;
	}
	if (extensions.Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) ||
		extensions.Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
	{
		simxr::VulkanInstanceGraphics graphics;
		uint32_t vulkanSamples = 1;
		const auto result = graphics.MaxSampleCount(vulkanSamples);
		if (XR_FAILED(result))
			return result;
		output = openGL || d3d11 || d3d12 ? std::min(output, vulkanSamples) : vulkanSamples;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurationViews_runtime(XrInstance instance, XrSystemId system,
																	XrViewConfigurationType viewType, uint32_t capacity,
																	uint32_t* count, XrViewConfigurationView* views)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	const auto validation = ValidateViewConfiguration(core->Extensions(), system, viewType);
	if (XR_FAILED(validation))
		return validation;
	if (!count || (capacity && !views))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = viewType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO ? 4 : 2;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	const bool foveationEnabled = core->Extensions().Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
	const std::array known{XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO};
	for (uint32_t index = 0; index < *count; ++index)
	{
		const simxr::StructureChain chain(views[index].next);
		if (views[index].type != XR_TYPE_VIEW_CONFIGURATION_VIEW ||
			XR_FAILED(chain.Validate(std::span(known).first(foveationEnabled ? 1 : 0))))
			return XR_ERROR_VALIDATION_FAILURE;
		if (foveationEnabled)
		{
			const auto request = chain.Find<XrFoveatedViewConfigurationViewVARJO>(known[0]);
			if (request && request->foveatedRenderingActive > XR_TRUE)
				return XR_ERROR_VALIDATION_FAILURE;
		}
	}
	uint32_t maxSamples = 1;
	const auto samplesResult = GetMaxSwapchainSampleCount(core->Extensions(), maxSamples);
	if (XR_FAILED(samplesResult))
		return samplesResult;
	simxr::ViewSettings viewSettings;
	settings::GetRenderResolution(viewSettings.outerWidth, viewSettings.outerHeight);
	for (uint32_t index = 0; index < *count; ++index)
	{
		views[index].recommendedImageRectWidth = index < 2 ? viewSettings.outerWidth : viewSettings.insetWidth;
		views[index].recommendedImageRectHeight = index < 2 ? viewSettings.outerHeight : viewSettings.insetHeight;
		views[index].recommendedSwapchainSampleCount = 1;
		views[index].maxImageRectWidth = settings::kMaxRenderExtent;
		views[index].maxImageRectHeight = settings::kMaxRenderExtent;
		views[index].maxSwapchainSampleCount = maxSamples;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateEnvironmentBlendModes_runtime(XrInstance instance, XrSystemId system,
																   XrViewConfigurationType viewType, uint32_t capacity,
																   uint32_t* count, XrEnvironmentBlendMode* modes)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	const auto validation = ValidateViewConfiguration(core->Extensions(), system, viewType);
	if (XR_FAILED(validation))
		return validation;
	if (!count || (capacity && !modes))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = 1;
	if (capacity)
		modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSession_runtime(XrInstance instance, const XrSessionCreateInfo* info,
												  XrSession* session)
{
	if (!info || !session || info->type != XR_TYPE_SESSION_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->systemId != 1)
		return XR_ERROR_SYSTEM_INVALID;

	if (!RuntimeState().FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	const auto owner = rt::RetainInstance(instance);
	if (!owner)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lifecycle(owner->lifecycleMutex);
	if (!rt::RetainInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (owner->closing)
		return XR_ERROR_INSTANCE_LOST;
	if (rt::RetainNativeSession())
		return XR_ERROR_LIMIT_REACHED;
	struct SessionCreation
	{
		simxr::Runtime::SessionRef core;
		std::unique_ptr<rt::Session> native;
		bool committed = false;
		~SessionCreation()
		{
			if (committed)
				return;
			if (core)
				RuntimeState().DestroySession(core);
			native.reset();
		}
	} creation;
	auto created = RuntimeState().CreateSession(instance, creation.core, std::make_shared<simxr::WindowsClock>());
	if (XR_FAILED(created))
		return created;
	creation.native = std::make_unique<rt::Session>();
	auto& native = *creation.native;
	native.owner = owner;
	native.handle = creation.core;
	native.core = creation.core.Shared();
	native.input = owner->input;
	native.compatibilityFiles = owner->compatibilityFiles;
	native.compatibilityCapture = owner->compatibilityCapture;
	native.diagnostics = owner->diagnostics;
	std::optional<simxr::VulkanBinding> vulkanBinding;
	auto publish = [&]() -> XrResult
	{
		XrResult result;
		if (vulkanBinding)
		{
			std::unique_ptr<simxr::VulkanSessionGraphics> graphics;
			const auto* validation = std::getenv("SIMXR_VULKAN_VALIDATION");
			result = simxr::VulkanSessionGraphics::Create(*vulkanBinding, creation.core.Shared(),
														  validation && std::strcmp(validation, "1") == 0, graphics);
			if (XR_FAILED(result))
				return result;
			graphics->SetObserver(std::make_unique<simxr::VulkanLegacyCapture>(
				graphics->Device(), creation.core.Shared(),
				std::make_shared<simxr::WindowsVulkanCaptureChannel>(
					native.compatibilityFiles, native.compatibilityCapture, creation.core.Shared(),
					[input = native.input]
					{
						return input->Head();
					},
					[input = native.input](const mcp::HeadPoseCommand& pose)
					{
						input->SetHead(pose);
					}),
				native.diagnostics));
			native.vulkan = std::move(graphics);
		}
		else if (native.d3d11Device || native.d3d12Device || native.usesOpenGL)
		{
			try
			{
				native.nativeWaits = std::make_shared<simxr::SwapchainWaits>();
				native.nativeClosing = false;
				if (native.usesOpenGL)
					native.nativeGraphics = std::make_shared<simxr::WindowsSessionGraphics>(native.glDC, native.glRC,
																							creation.core.Shared());
				else if (native.usesD3D12)
					native.nativeGraphics = std::make_shared<simxr::WindowsSessionGraphics>(
						native.d3d12Device.Get(), native.d3d12Queue.Get(), creation.core.Shared());
				else
					native.nativeGraphics = std::make_shared<simxr::WindowsSessionGraphics>(native.d3d11Device.Get(),
																							creation.core.Shared());
			}
			catch (const std::exception& error)
			{
				Logf("[SimXR] Native companion graphics unavailable: %s", error.what());
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			}
		}
		const auto time = simxr::RuntimeTime{
			std::chrono::duration_cast<simxr::Duration>(simxr::SteadyClock{}.Now().time_since_epoch())};
		result = creation.core->Ready(time);
		if (XR_FAILED(result))
			return result;
		const auto attached = rt::State().sessions.Attach(creation.core, std::move(creation.native));
		{
			std::lock_guard lock(rt::State().mutex);
			rt::State().currentSession = attached;
		}
		*session = attached;
		creation.committed = true;
		Logf("[SimXR] xrCreateSession: SUCCESS handle=%llu", (unsigned long long)*session);
		return XR_SUCCESS;
	};
	const simxr::StructureChain chain(info->next);
	const std::array bindings{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, XR_TYPE_GRAPHICS_BINDING_D3D12_KHR,
							  XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR, XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	if (XR_FAILED(chain.Validate(bindings)))
		return XR_ERROR_VALIDATION_FAILURE;
	std::size_t bindingCount = 0;
	for (auto type : bindings)
		if (chain.Find<XrBaseInStructure>(type))
			++bindingCount;
	if (bindingCount != 1)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	// Accept D3D11 and D3D12
	const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(info->next);
	while (entry)
	{
		if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
		{
			auto enabled = RequireExtension(instance, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
			if (XR_FAILED(enabled))
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			const auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
			if (!b->device)
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;

			// Log the device details
			ComPtr<IDXGIDevice> dxgiDevice;
			if (SUCCEEDED(b->device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
			{
				ComPtr<IDXGIAdapter> adapter;
				if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
				{
					DXGI_ADAPTER_DESC desc{};
					if (SUCCEEDED(adapter->GetDesc(&desc)))
						Logf("[SimXR] xrCreateSession: App D3D11 device LUID=%ld/%lu",
							 static_cast<long>(desc.AdapterLuid.HighPart),
							 static_cast<unsigned long>(desc.AdapterLuid.LowPart));
				}
			}

			native.d3d11Device = b->device;
			native.usesD3D12 = false;
			native.d3d12Device.Reset();
			native.d3d12Queue.Reset();
			// The shared adapter publishes GPU images to the companion.
			return publish();
		}
		else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
		{
			auto enabled = RequireExtension(instance, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
			if (XR_FAILED(enabled))
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			const auto* b12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
			if (!b12->device || !b12->queue)
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			native.usesD3D12 = true;
			native.d3d12Device = b12->device;
			native.d3d12Queue = b12->queue;
			native.d3d11Device.Reset();
			return publish();
		}
		else if (entry->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
		{
			if (!owner->core || (!owner->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
								 !owner->core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)))
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			// XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR is an alias of this, so one branch serves
			// XR_KHR_vulkan_enable and XR_KHR_vulkan_enable2 alike.
			simxr::VulkanBinding binding{};
			{
				std::lock_guard lock(owner->graphicsMutex);
				const auto result = owner->vulkan.SessionBinding(*info, binding);
				if (XR_FAILED(result))
					return result;
			}
			vulkanBinding = binding;
			native.usesD3D12 = false;
			native.usesOpenGL = false;
			return publish();
		}
		else if (entry->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR)
		{
			auto enabled = RequireExtension(instance, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
			if (XR_FAILED(enabled))
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			const auto* bGL = reinterpret_cast<const XrGraphicsBindingOpenGLWin32KHR*>(entry);
			if (!bGL->hDC || !bGL->hGLRC)
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			native.usesOpenGL = true;
			native.usesD3D12 = false;
			native.glDC = bGL->hDC;
			native.glRC = bGL->hGLRC;
			native.d3d11Device.Reset();
			native.d3d12Device.Reset();
			native.d3d12Queue.Reset();
			return publish();
		}
		entry = entry->next;
	}
	Log("[SimXR] xrCreateSession: ERROR - No supported graphics binding found (D3D11/D3D12/OpenGL)");
	return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}

static XrResult CloseVulkanSession(const rt::SessionRef& native)
{
	std::shared_ptr<simxr::VulkanSessionGraphics> graphics;
	{
		std::lock_guard lock(native->graphicsMutex);
		graphics = native->vulkan;
		if (!graphics)
			return XR_SUCCESS;
		graphics->BeginClose();
	}
	native->core->Close();
	auto result = graphics->Drain();
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(native->graphicsMutex);
	result = graphics->FinishClose();
	if (XR_SUCCEEDED(result))
		native->vulkan.reset();
	return result;
}

static XrResult DestroyNativeSession(const rt::SessionRef& native)
{
	std::shared_ptr<simxr::WindowsSessionGraphics> graphics;
	std::shared_ptr<simxr::SwapchainWaits> waits;
	{
		std::lock_guard lock(native->graphicsMutex);
		graphics = native->nativeGraphics;
		waits = native->nativeWaits;
		native->nativeClosing = true;
		if (waits)
			waits->Close();
		for (const auto& [handle, chain] : native->swapchains)
			chain.access->Close();
	}
	native->core->Close();
	if (waits && !waits->Drain(std::chrono::seconds(5)))
		return XR_ERROR_RUNTIME_FAILURE;
	if (graphics)
	{
		const auto closed = graphics->Close();
		if (XR_FAILED(closed))
			return closed;
	}
	const auto vulkanClosed = CloseVulkanSession(native);
	if (XR_FAILED(vulkanClosed))
		return vulkanClosed;
	{
		std::lock_guard lock(native->graphicsMutex);
		for (auto& [handle, chain] : native->swapchains)
			if (chain.glImages && !chain.glImages->Close())
				return XR_ERROR_RUNTIME_FAILURE;
		native->swapchains.clear();
	}
	// Keep the retired owner intact until all native waits and reads have completed.
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroySession_runtime(XrSession handle)
{
	const auto native = rt::RetainNativeSession(handle);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lifecycle(native->owner->lifecycleMutex);
	if (!rt::RetainNativeSession(handle))
		return XR_ERROR_HANDLE_INVALID;
	const auto result = DestroyNativeSession(native);
	if (XR_FAILED(result))
		return result;
	const auto destroyed = RuntimeState().DestroySession(handle);
	if (XR_FAILED(destroyed))
		return destroyed;
	rt::State().sessions.Remove(handle);
	{
		std::lock_guard lock(rt::State().mutex);
		if (rt::State().currentSession == handle)
			rt::State().currentSession = XR_NULL_HANDLE;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateSwapchainFormats_runtime(XrSession session, uint32_t capacity, uint32_t* count,
															  int64_t* formats)
{
	const auto native = rt::RetainNativeSession(session);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	if (native->vulkan)
	{
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
	if (native->usesOpenGL)
	{
		if (!count)
			return XR_ERROR_VALIDATION_FAILURE;
		const auto supported = simxr::windows::OpenGLSwapchain::Formats();
		*count = static_cast<std::uint32_t>(supported.size());
		if (capacity == 0)
			return XR_SUCCESS;
		if (!formats)
			return XR_ERROR_VALIDATION_FAILURE;
		if (capacity < supported.size())
			return XR_ERROR_SIZE_INSUFFICIENT;
		std::copy(supported.begin(), supported.end(), formats);
		return XR_SUCCESS;
	}

	const auto supportedFormats = simxr::windows::Direct3DSwapchainFormats();
	const auto formatCount = static_cast<uint32_t>(supportedFormats.size());

	if (!count || (capacity && !formats))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = formatCount;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < formatCount)
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(std::begin(supportedFormats), std::end(supportedFormats), formats);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSwapchain_runtime(XrSession session, const XrSwapchainCreateInfo* info,
													XrSwapchain* output)
{
	const auto native = rt::RetainNativeSession(session);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	if (!info || !output)
		return XR_ERROR_VALIDATION_FAILURE;
	if (native->vulkan)
		return native->vulkan->CreateSwapchain(*info, *output);
	if (native->nativeClosing)
		return XR_ERROR_SESSION_LOST;

	rt::Swapchain chain;
	XrResult result;
	std::uint32_t imageCount;
	if (native->usesOpenGL)
	{
		chain.backend = rt::Swapchain::Backend::OpenGL;
		result = simxr::windows::OpenGLSwapchain::Create(native->glDC, native->glRC, *info, chain.glImages);
		if (XR_FAILED(result))
			return result;
		imageCount = static_cast<std::uint32_t>(chain.glImages->Images().size());
	}
	else
	{
		if (native->usesD3D12)
		{
			chain.backend = rt::Swapchain::Backend::D3D12;
			result = simxr::windows::Direct3DSwapchain::Create(native->d3d12Device.Get(), *info, chain.d3dImages);
		}
		else
			result = simxr::windows::Direct3DSwapchain::Create(native->d3d11Device.Get(), *info, chain.d3dImages);
		if (XR_FAILED(result))
			return result;
		imageCount = chain.d3dImages->ImageCount();
	}
	chain.handle = simxr::AllocateHandle<XrSwapchain>();
	chain.format = info->format;
	chain.width = info->width;
	chain.height = info->height;
	chain.arraySize = info->arraySize;
	chain.mipCount = info->mipCount;
	chain.sampleCount = info->sampleCount;
	chain.access = std::make_shared<simxr::SwapchainAccess>(
		imageCount, (info->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0);
	const auto handle = chain.handle;
	native->swapchains.emplace(handle, std::move(chain));
	*output = handle;
	Logf("[SimXR] xrCreateSwapchain: sc=%p format=%lld %ux%u array=%u samples=%u", handle,
		 static_cast<long long>(info->format), info->width, info->height, info->arraySize, info->sampleCount);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateSwapchainImages_runtime(XrSwapchain sc, uint32_t capacity, uint32_t* count,
															 XrSwapchainImageBaseHeader* images)
{
	const auto native = rt::RetainNativeSession();
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	if (native->vulkan)
	{
		const auto chain = native->vulkan->FindSwapchain(sc);
		return chain ? chain->EnumerateImages(capacity, count, images) : XR_ERROR_HANDLE_INVALID;
	}
	auto it = native->swapchains.find(sc);
	if (it == native->swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	if (it->second.d3dImages)
		return it->second.d3dImages->EnumerateImages(capacity, count, images);
	if (!count || (capacity && !images))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto textures = it->second.glImages->Images();
	*count = static_cast<std::uint32_t>(textures.size());
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < textures.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	auto* output = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images);
	for (std::size_t image = 0; image < textures.size(); ++image)
		if (output[image].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
	for (std::size_t image = 0; image < textures.size(); ++image)
		output[image].image = textures[image];
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrAcquireSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageAcquireInfo* info,
														  uint32_t* index)
{
	const auto native = rt::RetainNativeSession();
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	if (native->vulkan)
	{
		const auto chain = native->vulkan->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		if (!index || (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO))
			return XR_ERROR_VALIDATION_FAILURE;
		return native->vulkan->Acquire(*chain.Shared(), *index);
	}
	auto it = native->swapchains.find(sc);
	if (it == native->swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	auto& ch = it->second;
	if (!index || (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO))
		return XR_ERROR_VALIDATION_FAILURE;
	uint32_t i;
	auto result = ch.access->Acquire(i);
	if (XR_FAILED(result))
		return result;
	*index = i;

	const auto acquireCount = ++native->acquireCount;
	// First few calls give startup context; the recurring line is verbose-only,
	// since Log() is deliberately expensive (see its comment).
	if (acquireCount <= 4 || (s_logVerbose && acquireCount % 60 == 1))
	{
		Logf("[SimXR] xrAcquireSwapchainImage: sc=%p idx=%u (format=%lld, %ux%u)", sc, i,
			 static_cast<long long>(ch.format), ch.width, ch.height);
	}
	return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrWaitSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageWaitInfo* info)
{
	const auto native = rt::RetainNativeSession();
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::unique_lock lock(native->graphicsMutex);
	if (auto graphics = native->vulkan)
	{
		const auto chain = graphics->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO || info->timeout < 0)
			return XR_ERROR_VALIDATION_FAILURE;
		auto registration = graphics->EnterWait();
		if (!registration)
			return XR_ERROR_SESSION_LOST;
		lock.unlock();
		return chain->Wait(info->timeout);
	}
	const auto found = native->swapchains.find(sc);
	if (found == native->swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO || info->timeout < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto access = found->second.access;
	const auto graphics = native->nativeGraphics;
	const auto waits = native->nativeWaits;
	if (!graphics || !waits)
		return XR_ERROR_SESSION_LOST;
	auto registration = waits->Enter();
	if (!registration)
		return XR_ERROR_SESSION_LOST;
	lock.unlock();
	return access->Wait(info->timeout);
}
static XrResult XRAPI_PTR xrReleaseSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageReleaseInfo* info)
{
	const auto native = rt::RetainNativeSession();
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	if (native->vulkan)
	{
		const auto chain = native->vulkan->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		return chain->Release();
	}
	auto it = native->swapchains.find(sc);
	if (it == native->swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	auto& ch = it->second;
	if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	auto result = ch.access->Release();
	if (XR_FAILED(result))
		return result;

	const auto releaseCount = ++native->releaseCount;
	bool shouldLog = releaseCount <= 10;
	if (shouldLog || (s_logVerbose && releaseCount % 60 == 1))
	{
		Logf("[SimXR] xrReleaseSwapchainImage: sc=%p released=%u", sc, *ch.access->ReleasedImage());
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrPollEvent_runtime(XrInstance instance, XrEventDataBuffer* buffer)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!buffer)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Events().Poll(*buffer);
}

static XrResult XRAPI_PTR xrBeginSession_runtime(XrSession handle, const XrSessionBeginInfo* info)
{
	const auto native = rt::RetainNativeSession(handle);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	const auto core = native->core;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	XrTime epoch;
	const auto clockResult = simxr::ReadPerformanceTime(epoch);
	if (XR_FAILED(clockResult))
		return clockResult;
	simxr::FrameSchedulerConfig config;
	config.epoch = {simxr::Duration{epoch}};
	std::uint64_t seed = 0;
	if (const auto* text = std::getenv("SIMXR_SEED"))
	{
		const std::string_view value{text};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
		if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
			return XR_ERROR_VALIDATION_FAILURE;
	}
	if (const auto* deterministic = std::getenv("SIMXR_DETERMINISTIC");
		deterministic && std::strcmp(deterministic, "0") != 0)
	{
		config.mode = simxr::ExecutionMode::Deterministic;
		config.epoch = {simxr::Duration{1000000000LL}};
	}
	const auto input = native->input->Snapshot();
	simxr::CommandConfiguration baseline;
	baseline.tracking = input.tracking;
	baseline.input = core->Input().Snapshot();
	baseline.views = input.views;
	auto configured = core->ConfigureCommands(seed, baseline, false);
	if (XR_FAILED(configured))
		return configured;
	simxr::PresentationPolicy policy = simxr::PresentationPolicy::Repeat;
	{
		std::lock_guard lock(native->graphicsMutex);
		if (native->vulkan || native->nativeGraphics)
		{
			if (const auto* text = std::getenv("SIMXR_APPLICATION_CADENCE"))
			{
				const std::string_view value{text};
				if (value == "auto")
					config.automaticCadence = true;
				else if (value == "1" || value == "2")
					config.cadence = value == "2" ? 2 : 1;
				else
					return XR_ERROR_VALIDATION_FAILURE;
			}
			if (const auto* text = std::getenv("SIMXR_PRESENTATION_POLICY"))
			{
				const std::string_view value{text};
				if (value == "rotational")
					policy = simxr::PresentationPolicy::Rotational;
				else if (value != "repeat")
					return XR_ERROR_VALIDATION_FAILURE;
			}
			if (policy == simxr::PresentationPolicy::Rotational && native->vulkan &&
				!native->vulkan->SupportsReprojection())
				return XR_ERROR_FEATURE_UNSUPPORTED;
		}
	}
	auto result = core->Begin(*info, config, policy);
	return result;
}
static XrResult XRAPI_PTR xrEndSession_runtime(XrSession handle)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	auto result = core->End({simxr::SteadyClock{}.Now().time_since_epoch()});
	return result;
}
static XrResult XRAPI_PTR xrRequestExitSession_runtime(XrSession handle)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	auto result = core->RequestExit({simxr::SteadyClock{}.Now().time_since_epoch()});
	return result;
}
static XrResult XRAPI_PTR xrWaitFrame_runtime(XrSession session, const XrFrameWaitInfo* info, XrFrameState* s)
{
	const auto native = rt::RetainNativeSession(session);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	if (!s || s->type != XR_TYPE_FRAME_STATE || (info && info->type != XR_TYPE_FRAME_WAIT_INFO))
		return XR_ERROR_VALIDATION_FAILURE;
	simxr::FramePrediction prediction;
	const auto core = native->core;
	const auto compatibilityFiles = native->compatibilityFiles;
	const auto publishNative = [&](const simxr::FramePrediction& admitted) -> XrResult
	{
		std::lock_guard lifecycle(native->owner->lifecycleMutex);
		if (native->owner->closing || rt::RetainNativeSession(session).Shared() != native.Shared())
			return XR_ERROR_SESSION_LOST;
		const auto input = native->input->Poll(admitted.displayPeriod, *compatibilityFiles);
		core->PublishTracking(admitted.displayTime, input.tracking, input.views, admitted.id);

		s->predictedDisplayPeriod = admitted.displayPeriod.count();
		s->predictedDisplayTime = admitted.displayTime.value.count();
		const auto state = core->Snapshot().state;
		s->shouldRender = state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED;
		return XR_SUCCESS;
	};
	return core->WaitFrame(prediction, publishNative);
}
static XrResult XRAPI_PTR xrBeginFrame_runtime(XrSession session, const XrFrameBeginInfo* info)
{
	if (session == XR_NULL_HANDLE)
		return XR_ERROR_HANDLE_INVALID;
	auto core = rt::RetainSession(session);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->BeginFrame();
}

static XrResult XRAPI_PTR xrEndFrame_runtime(XrSession session, const XrFrameEndInfo* info)
{
	const auto native = rt::RetainNativeSession(session);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	const auto core = native->core;
	const auto compatibilityCapture = native->compatibilityCapture;
	if (!info || info->type != XR_TYPE_FRAME_END_INFO || (info->layerCount && !info->layers))
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->displayTime <= 0)
		return XR_ERROR_TIME_INVALID;
	if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
	{
		return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
	}
	{
		std::lock_guard lock(native->graphicsMutex);
		if (native->nativeGraphics)
		{
			if (native->nativeClosing)
				return XR_ERROR_SESSION_LOST;
			// Retain the exact image selected before submission; callbacks never look it up again.
			struct SourceImage
			{
				XrSwapchain handle;
				std::shared_ptr<simxr::SwapchainAccess> access;
				std::uint32_t index;
			};
			std::vector<SourceImage> sources;
			return native->nativeGraphics->EndFrame(
				*info,
				[&native](XrSwapchain handle) -> std::optional<simxr::SwapchainDescription>
				{
					const auto found = native->swapchains.find(handle);
					if (found == native->swapchains.end())
						return {};
					const auto& chain = found->second;
					return simxr::SwapchainDescription{chain.width, chain.height, chain.arraySize,
													   chain.access->ReleasedImage().has_value()};
				},
				[&native, &sources](XrSwapchain handle) -> simxr::WindowsSessionGraphics::Source
				{
					const auto found = native->swapchains.find(handle);
					if (found == native->swapchains.end())
						return {};
					const auto& chain = found->second;
					const auto image = chain.access->ReleasedImage();
					if (!image)
						return {};
					if (std::none_of(sources.begin(), sources.end(),
									 [handle](const SourceImage& source)
									 {
										 return source.handle == handle;
									 }))
						sources.push_back({handle, chain.access, *image});
					if (chain.backend == rt::Swapchain::Backend::OpenGL)
					{
						if (!chain.glImages || *image >= chain.glImages->Images().size())
							return {};
						return simxr::OpenGLCompositionSource{chain.glImages->Images()[*image],
															  chain.glImages->Format(),
															  chain.width,
															  chain.height,
															  chain.arraySize,
															  chain.mipCount,
															  chain.sampleCount};
					}
					if (chain.backend == rt::Swapchain::Backend::D3D12)
						return simxr::D3D12CompositionSource{chain.d3dImages->Image12(*image),
															 chain.d3dImages->Format(),
															 chain.d3dImages->ReleaseState12()};
					return simxr::D3D11CompositionSource{chain.d3dImages->Image11(*image), chain.d3dImages->Format()};
				},
				[&sources](XrSwapchain handle, const std::shared_ptr<const simxr::GpuCompletion>& completion)
				{
					const auto source = std::find_if(sources.begin(), sources.end(),
													 [handle](const SourceImage& image)
													 {
														 return image.handle == handle;
													 });
					if (source != sources.end())
						source->access->MarkUsed(source->index, completion);
				});
		}

		if (native->vulkan)
		{
			const auto result = native->vulkan->EndFrame(*info);
			if (XR_SUCCEEDED(result))
			{
				const auto snapshot = core->Snapshot();
				const auto views = core->ViewsAt(simxr::RuntimeTime{simxr::Duration{info->displayTime}});
				const auto width = views.views.empty() ? 0u : views.views.front().description.width * 2;
				const auto height = views.views.empty() ? 0u : views.views.front().description.height;
				compatibilityCapture->SetFrame(snapshot.lastSubmitted.value);
				const auto head = native->input->Head();
				compatibilityCapture->PublishStatus({snapshot.lastSubmitted.value, width, height, "RGBA8",
													 SessionStateName(snapshot.state), head.yaw, head.pitch, head.roll,
													 head.x, head.y, head.z});
			}
			return result;
		}
	}
	return XR_ERROR_SESSION_LOST;
}

static XrResult XRAPI_PTR xrLocateViews_runtime(XrSession handle, const XrViewLocateInfo* info, XrViewState* state,
												uint32_t capacity, uint32_t* count, XrView* views)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->LocateViews(*info, state, capacity, count, views);
}

// Add missing space/action functions for compatibility
static XrResult XRAPI_PTR xrCreateReferenceSpace_runtime(XrSession handle, const XrReferenceSpaceCreateInfo* info,
														 XrSpace* out)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !out || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Spaces().CreateReferenceSpace(info->referenceSpaceType, info->poseInReferenceSpace, *out);
}

static XrResult XRAPI_PTR xrDestroySpace_runtime(XrSpace space)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Spaces().Destroy(space);
}

static XrResult XRAPI_PTR xrLocateSpace_runtime(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return simxr::LocateSpace(core->Spaces(), space, base, time, location, &core->Extensions());
}

static XrResult XRAPI_PTR xrEnumerateReferenceSpaces_runtime(XrSession handle, uint32_t capacity, uint32_t* count,
															 XrReferenceSpaceType* spaces)
{
	const auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!count || (capacity && !spaces))
		return XR_ERROR_VALIDATION_FAILURE;
	const bool foveationEnabled = core->Extensions().Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
	*count = foveationEnabled ? 4 : 3;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
	spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
	if (foveationEnabled)
		spaces[3] = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateActionSpace_runtime(XrSession handle, const XrActionSpaceCreateInfo* info,
													  XrSpace* out)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !out || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Spaces().CreateActionSpace(info->action, info->subactionPath, info->poseInActionSpace, *out);
}

static XrResult XRAPI_PTR xrCreateActionSet_runtime(XrInstance handle, const XrActionSetCreateInfo* info,
													XrActionSet* set)
{
	const auto core = RuntimeState().FindInstance(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !set)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().CreateSet(*info, *set);
}

static XrResult XRAPI_PTR xrCreateAction_runtime(XrActionSet set, const XrActionCreateInfo* info, XrAction* action)
{
	const auto instance = rt::RetainInstance();
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !action)
		return XR_ERROR_VALIDATION_FAILURE;
	return instance->core->Actions().CreateAction(set, *info, *action);
}

static XrResult XRAPI_PTR xrSuggestInteractionProfileBindings_runtime(XrInstance handle,
																	  const XrInteractionProfileSuggestedBinding* info)
{
	const auto core = RuntimeState().FindInstance(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().SuggestBindings(*info);
}

static XrResult XRAPI_PTR xrAttachSessionActionSets_runtime(XrSession handle, const XrSessionActionSetsAttachInfo* info)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Attach(*info);
}

static XrResult XRAPI_PTR xrGetActionStateBoolean_runtime(XrSession handle, const XrActionStateGetInfo* info,
														  XrActionStateBoolean* state)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().GetBoolean(*info, *state);
}

static XrResult XRAPI_PTR xrGetActionStateFloat_runtime(XrSession handle, const XrActionStateGetInfo* info,
														XrActionStateFloat* state)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().GetFloat(*info, *state);
}

static XrResult XRAPI_PTR xrGetActionStateVector2f_runtime(XrSession handle, const XrActionStateGetInfo* info,
														   XrActionStateVector2f* state)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().GetVector(*info, *state);
}

static XrResult XRAPI_PTR xrGetActionStatePose_runtime(XrSession handle, const XrActionStateGetInfo* info,
													   XrActionStatePose* state)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !state)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().GetPose(*info, *state);
}

static XrResult XRAPI_PTR xrStringToPath_runtime(XrInstance handle, const char* name, XrPath* path)
{
	const auto core = RuntimeState().FindInstance(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!name || !path)
		return XR_ERROR_VALIDATION_FAILURE;
	auto end = std::find(name, name + XR_MAX_PATH_LENGTH, '\0');
	if (end == name + XR_MAX_PATH_LENGTH)
		return XR_ERROR_PATH_FORMAT_INVALID;
	return core->Actions().StringToPath(std::string_view(name, end - name), *path);
}

static XrResult XRAPI_PTR xrPathToString_runtime(XrInstance handle, XrPath path, uint32_t capacity, uint32_t* count,
												 char* buffer)
{
	const auto core = RuntimeState().FindInstance(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Actions().PathToString(path, capacity, count, buffer);
}

static XrResult XRAPI_PTR xrGetCurrentInteractionProfile_runtime(XrSession handle, XrPath user,
																 XrInteractionProfileState* state)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!state || state->type != XR_TYPE_INTERACTION_PROFILE_STATE)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Actions().CurrentProfile(user, state->interactionProfile);
}

static XrResult XRAPI_PTR xrEnumerateBoundSourcesForAction_runtime(XrSession handle,
																   const XrBoundSourcesForActionEnumerateInfo* info,
																   uint32_t capacity, uint32_t* count, XrPath* out)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	std::vector<XrPath> paths;
	auto result = core->Actions().BoundSources(info->action, paths);
	if (XR_FAILED(result))
		return result;
	if (!count || (capacity && !out))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = static_cast<uint32_t>(paths.size());
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(paths.begin(), paths.end(), out);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInputSourceLocalizedName_runtime(XrSession handle,
																const XrInputSourceLocalizedNameGetInfo* info,
																uint32_t capacity, uint32_t* count, char* out)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || !count || (capacity && !out) || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	std::string name;
	const auto result = core->Actions().LocalizedName(info->sourcePath, info->whichComponents, name);
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

static XrResult XRAPI_PTR xrDestroyAction_runtime(XrAction action)
{
	const auto instance = rt::RetainInstance();
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	auto core = rt::RetainSession();
	if (core)
	{
		core->Haptics().RemoveAction(action, core->Snapshot().time);
	}
	return instance->core->Actions().DestroyAction(action);
}

static XrResult XRAPI_PTR xrDestroyActionSet_runtime(XrActionSet set)
{
	const auto instance = rt::RetainInstance();
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	for (const auto& action : instance->core->Actions().Actions())
		if (action.set == set)
			xrDestroyAction_runtime(action.handle);
	return instance->core->Actions().DestroySet(set);
}

static XrResult XRAPI_PTR xrSyncActions_runtime(XrSession handle, const XrActionsSyncInfo* info)
{
	const auto native = rt::RetainNativeSession(handle);
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	return native->core->SyncActions(*info, native->input->Snapshot().controls);
}

static XrResult XRAPI_PTR xrDestroySwapchain_runtime(XrSwapchain sc)
{
	const auto native = rt::RetainNativeSession();
	if (!native)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(native->graphicsMutex);
	if (native->vulkan)
	{
		const auto chain = native->vulkan->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		native->vulkan->DestroySwapchain(chain);
		return XR_SUCCESS;
	}
	const auto found = native->swapchains.find(sc);
	if (found == native->swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	found->second.access->Close();
	if (found->second.glImages && !found->second.glImages->Close())
		return XR_ERROR_RUNTIME_FAILURE;
	native->swapchains.erase(found);
	Logf("[SimXR] xrDestroySwapchain: sc=%p", sc);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrResultToString_runtime(XrInstance instance, XrResult value,
												   char buffer[XR_MAX_RESULT_STRING_SIZE])
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return simxr::ResultToString(value, buffer);
}

static XrResult XRAPI_PTR xrStructureTypeToString_runtime(XrInstance instance, XrStructureType value,
														  char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return simxr::StructureTypeToString(value, buffer);
}

static XrResult XRAPI_PTR xrGetReferenceSpaceBoundsRect_runtime(XrSession handle, XrReferenceSpaceType type,
																XrExtent2Df* bounds)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	if (!bounds)
		return XR_ERROR_VALIDATION_FAILURE;
	return core->Spaces().Bounds(type, *bounds);
}

static XrResult XRAPI_PTR xrGetViewConfigurationProperties_runtime(XrInstance instance, XrSystemId system,
																   XrViewConfigurationType type,
																   XrViewConfigurationProperties* props)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	const auto validation = ValidateViewConfiguration(core->Extensions(), system, type);
	if (XR_FAILED(validation))
		return validation;
	if (!props || props->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES ||
		XR_FAILED(simxr::StructureChain(props->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	props->viewConfigurationType = type;
	props->fovMutable = XR_TRUE;
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrApplyHapticFeedback_runtime(XrSession handle, const XrHapticActionInfo* info,
														const XrHapticBaseHeader* feedback)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->ApplyHaptic(info, feedback);
}
static XrResult XRAPI_PTR xrStopHapticFeedback_runtime(XrSession handle, const XrHapticActionInfo* info)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->StopHaptic(info);
}

static XrResult XRAPI_PTR xrEnumerateViveTrackerPathsHTCX_runtime(XrInstance handle, uint32_t capacity, uint32_t* count,
																  XrViveTrackerPathsHTCX* paths)
{
	auto instance = RuntimeState().FindInstance(handle);
	if (!instance)
		return XR_ERROR_HANDLE_INVALID;
	return instance->Trackers().Enumerate(capacity, count, paths);
}

static XrResult XRAPI_PTR xrCreateBodyTrackerFB_runtime(XrSession handle, const XrBodyTrackerCreateInfoFB* info,
														XrBodyTrackerFB* output)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().CreateFB(info, output);
}

static XrResult XRAPI_PTR xrCreateBodyTrackerBD_runtime(XrSession handle, const XrBodyTrackerCreateInfoBD* info,
														XrBodyTrackerBD* output)
{
	auto core = rt::RetainSession(handle);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().CreateBD(info, output);
}

static XrResult XRAPI_PTR xrDestroyBodyTrackerFB_runtime(XrBodyTrackerFB tracker)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().DestroyFB(tracker);
}

static XrResult XRAPI_PTR xrDestroyBodyTrackerBD_runtime(XrBodyTrackerBD tracker)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().DestroyBD(tracker);
}

static XrResult XRAPI_PTR xrLocateBodyJointsFB_runtime(XrBodyTrackerFB tracker, const XrBodyJointsLocateInfoFB* info,
													   XrBodyJointLocationsFB* output)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().LocateFB(tracker, info, output);
}

static XrResult XRAPI_PTR xrLocateBodyJointsBD_runtime(XrBodyTrackerBD tracker, const XrBodyJointsLocateInfoBD* info,
													   XrBodyJointLocationsBD* output)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().LocateBD(tracker, info, output);
}

static XrResult XRAPI_PTR xrGetBodySkeletonFB_runtime(XrBodyTrackerFB tracker, XrBodySkeletonFB* output)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().SkeletonFB(tracker, output);
}

static XrResult XRAPI_PTR xrSuggestBodyTrackingCalibrationOverrideMETA_runtime(
	XrBodyTrackerFB tracker, const XrBodyTrackingCalibrationInfoMETA* info)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().SuggestCalibrationFB(tracker, info);
}

static XrResult XRAPI_PTR xrResetBodyTrackingCalibrationMETA_runtime(XrBodyTrackerFB tracker)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().ResetCalibrationFB(tracker);
}

static XrResult XRAPI_PTR xrRequestBodyTrackingFidelityMETA_runtime(XrBodyTrackerFB tracker,
																	XrBodyTrackingFidelityMETA fidelity)
{
	auto core = rt::RetainSession();
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	return core->Body().RequestFidelityFB(tracker, fidelity);
}

static XrResult XRAPI_PTR xrConvertWin32PerformanceCounterToTimeKHR_runtime(XrInstance instance,
																			const LARGE_INTEGER* performanceCounter,
																			XrTime* time)
{
	const auto result = RequireExtension(instance, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!performanceCounter || !time)
		return XR_ERROR_VALIDATION_FAILURE;
	return simxr::PerformanceCounterToTime(performanceCounter->QuadPart, *time);
}

static XrResult XRAPI_PTR xrConvertTimeToWin32PerformanceCounterKHR_runtime(XrInstance instance, XrTime time,
																			LARGE_INTEGER* performanceCounter)
{
	const auto result = RequireExtension(instance, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!performanceCounter)
		return XR_ERROR_VALIDATION_FAILURE;
	std::int64_t counter;
	const auto converted = simxr::TimeToPerformanceCounter(time, counter);
	if (XR_SUCCEEDED(converted))
		performanceCounter->QuadPart = counter;
	return converted;
}

// ----------------------------------------------

static const simxr::DispatchEntry s_functions[] = {
	{"xrGetInstanceProcAddr", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetInstanceProcAddr_runtime>::Invoke, true},
	{"xrEnumerateApiLayerProperties",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateApiLayerProperties_runtime>::Invoke, true},
	{"xrEnumerateInstanceExtensionProperties",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateInstanceExtensionProperties_runtime>::Invoke, true},
	{"xrCreateInstance", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateInstance_runtime>::Invoke, true},
	{"xrDestroyInstance", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyInstance_runtime>::Invoke},
	{"xrGetInstanceProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetInstanceProperties_runtime>::Invoke},
	{"xrGetSystem", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetSystem_runtime>::Invoke},
	{"xrGetSystemProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetSystemProperties_runtime>::Invoke},
	{"xrEnumerateViewConfigurations",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateViewConfigurations_runtime>::Invoke},
	{"xrEnumerateViewConfigurationViews",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateViewConfigurationViews_runtime>::Invoke},
	{"xrEnumerateEnvironmentBlendModes",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateEnvironmentBlendModes_runtime>::Invoke},
	{"xrEnumerateViveTrackerPathsHTCX",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateViveTrackerPathsHTCX_runtime>::Invoke},
	{"xrCreateBodyTrackerFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateBodyTrackerFB_runtime>::Invoke},
	{"xrCreateBodyTrackerBD", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateBodyTrackerBD_runtime>::Invoke},
	{"xrDestroyBodyTrackerFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyBodyTrackerFB_runtime>::Invoke},
	{"xrDestroyBodyTrackerBD", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyBodyTrackerBD_runtime>::Invoke},
	{"xrLocateBodyJointsFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrLocateBodyJointsFB_runtime>::Invoke},
	{"xrLocateBodyJointsBD", (PFN_xrVoidFunction)simxr::AbiBoundary<xrLocateBodyJointsBD_runtime>::Invoke},
	{"xrGetBodySkeletonFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetBodySkeletonFB_runtime>::Invoke},
	{"xrSuggestBodyTrackingCalibrationOverrideMETA",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrSuggestBodyTrackingCalibrationOverrideMETA_runtime>::Invoke},
	{"xrResetBodyTrackingCalibrationMETA",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrResetBodyTrackingCalibrationMETA_runtime>::Invoke},
	{"xrRequestBodyTrackingFidelityMETA",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrRequestBodyTrackingFidelityMETA_runtime>::Invoke},
	{"xrCreateSession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateSession_runtime>::Invoke},
	{"xrDestroySession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroySession_runtime>::Invoke},
	{"xrEnumerateSwapchainFormats",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateSwapchainFormats_runtime>::Invoke},
	{"xrCreateSwapchain", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateSwapchain_runtime>::Invoke},
	{"xrDestroySwapchain", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroySwapchain_runtime>::Invoke},
	{"xrEnumerateSwapchainImages", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateSwapchainImages_runtime>::Invoke},
	{"xrAcquireSwapchainImage", (PFN_xrVoidFunction)simxr::AbiBoundary<xrAcquireSwapchainImage_runtime>::Invoke},
	{"xrWaitSwapchainImage", (PFN_xrVoidFunction)simxr::AbiBoundary<xrWaitSwapchainImage_runtime>::Invoke},
	{"xrReleaseSwapchainImage", (PFN_xrVoidFunction)simxr::AbiBoundary<xrReleaseSwapchainImage_runtime>::Invoke},
	{"xrBeginSession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrBeginSession_runtime>::Invoke},
	{"xrEndSession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEndSession_runtime>::Invoke},
	{"xrWaitFrame", (PFN_xrVoidFunction)simxr::AbiBoundary<xrWaitFrame_runtime>::Invoke},
	{"xrBeginFrame", (PFN_xrVoidFunction)simxr::AbiBoundary<xrBeginFrame_runtime>::Invoke},
	{"xrEndFrame", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEndFrame_runtime>::Invoke},
	{"xrPollEvent", (PFN_xrVoidFunction)simxr::AbiBoundary<xrPollEvent_runtime>::Invoke},
	{"xrLocateViews", (PFN_xrVoidFunction)simxr::AbiBoundary<xrLocateViews_runtime>::Invoke},
	{"xrGetD3D11GraphicsRequirementsKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetD3D11GraphicsRequirementsKHR_runtime>::Invoke},
	{"xrGetD3D12GraphicsRequirementsKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetD3D12GraphicsRequirementsKHR_runtime>::Invoke},
	{"xrGetOpenGLGraphicsRequirementsKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetOpenGLGraphicsRequirementsKHR_runtime>::Invoke},
	// XR_KHR_vulkan_enable
	{"xrGetVulkanGraphicsRequirementsKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsRequirementsKHR_runtime>::Invoke},
	{"xrGetVulkanInstanceExtensionsKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanInstanceExtensionsKHR_runtime>::Invoke},
	{"xrGetVulkanDeviceExtensionsKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanDeviceExtensionsKHR_runtime>::Invoke},
	{"xrGetVulkanGraphicsDeviceKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsDeviceKHR_runtime>::Invoke},
	// XR_KHR_vulkan_enable2
	{"xrGetVulkanGraphicsRequirements2KHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsRequirements2KHR_runtime>::Invoke},
	{"xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateVulkanInstanceKHR_runtime>::Invoke},
	{"xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateVulkanDeviceKHR_runtime>::Invoke},
	{"xrGetVulkanGraphicsDevice2KHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsDevice2KHR_runtime>::Invoke},
	{"xrRequestExitSession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrRequestExitSession_runtime>::Invoke},
	// Space functions
	{"xrCreateReferenceSpace", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateReferenceSpace_runtime>::Invoke},
	{"xrDestroySpace", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroySpace_runtime>::Invoke},
	{"xrLocateSpace", (PFN_xrVoidFunction)simxr::AbiBoundary<xrLocateSpace_runtime>::Invoke},
	{"xrEnumerateReferenceSpaces", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateReferenceSpaces_runtime>::Invoke},
	{"xrCreateActionSpace", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateActionSpace_runtime>::Invoke},
	// Action functions
	{"xrCreateActionSet", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateActionSet_runtime>::Invoke},
	{"xrDestroyActionSet", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyActionSet_runtime>::Invoke},
	{"xrCreateAction", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateAction_runtime>::Invoke},
	{"xrDestroyAction", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyAction_runtime>::Invoke},
	{"xrSuggestInteractionProfileBindings",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrSuggestInteractionProfileBindings_runtime>::Invoke},
	{"xrAttachSessionActionSets", (PFN_xrVoidFunction)simxr::AbiBoundary<xrAttachSessionActionSets_runtime>::Invoke},
	{"xrGetActionStateBoolean", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetActionStateBoolean_runtime>::Invoke},
	{"xrGetActionStateFloat", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetActionStateFloat_runtime>::Invoke},
	{"xrGetActionStatePose", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetActionStatePose_runtime>::Invoke},
	{"xrGetActionStateVector2f", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetActionStateVector2f_runtime>::Invoke},
	{"xrSyncActions", (PFN_xrVoidFunction)simxr::AbiBoundary<xrSyncActions_runtime>::Invoke},
	// Path functions
	{"xrStringToPath", (PFN_xrVoidFunction)simxr::AbiBoundary<xrStringToPath_runtime>::Invoke},
	{"xrPathToString", (PFN_xrVoidFunction)simxr::AbiBoundary<xrPathToString_runtime>::Invoke},
	// Interaction functions
	{"xrGetCurrentInteractionProfile",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetCurrentInteractionProfile_runtime>::Invoke},
	{"xrEnumerateBoundSourcesForAction",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateBoundSourcesForAction_runtime>::Invoke},
	{"xrGetInputSourceLocalizedName",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetInputSourceLocalizedName_runtime>::Invoke},
	// Utility functions
	{"xrResultToString", (PFN_xrVoidFunction)simxr::AbiBoundary<xrResultToString_runtime>::Invoke},
	{"xrStructureTypeToString", (PFN_xrVoidFunction)simxr::AbiBoundary<xrStructureTypeToString_runtime>::Invoke},
	{"xrGetReferenceSpaceBoundsRect",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetReferenceSpaceBoundsRect_runtime>::Invoke},
	{"xrGetViewConfigurationProperties",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetViewConfigurationProperties_runtime>::Invoke},
	// Haptic functions
	{"xrApplyHapticFeedback", (PFN_xrVoidFunction)simxr::AbiBoundary<xrApplyHapticFeedback_runtime>::Invoke},
	{"xrStopHapticFeedback", (PFN_xrVoidFunction)simxr::AbiBoundary<xrStopHapticFeedback_runtime>::Invoke},
	// Time conversion functions
	{"xrConvertWin32PerformanceCounterToTimeKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrConvertWin32PerformanceCounterToTimeKHR_runtime>::Invoke},
	{"xrConvertTimeToWin32PerformanceCounterKHR",
	 (PFN_xrVoidFunction)simxr::AbiBoundary<xrConvertTimeToWin32PerformanceCounterKHR_runtime>::Invoke},
};

static XrResult XRAPI_PTR xrGetInstanceProcAddr_runtime(XrInstance instance, const char* name,
														PFN_xrVoidFunction* function)
{
	if (!function)
		return XR_ERROR_VALIDATION_FAILURE;
	*function = nullptr;
	if (!name)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto core = RuntimeState().FindInstance(instance);
	if (instance != XR_NULL_HANDLE && !core)
		return XR_ERROR_HANDLE_INVALID;
	static const simxr::DispatchTable table(s_functions, Extensions());
	if (!table.CoversAdvertisedCommands())
		return XR_ERROR_RUNTIME_FAILURE;
	return table.Resolve(name, core ? &core->Extensions() : nullptr, *function);
}
