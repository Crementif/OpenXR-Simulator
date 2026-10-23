#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL
#include "Pch.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <bit>
#include <charconv>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <future>
#include <latch>

namespace simxr::probe
{
using Microsoft::WRL::ComPtr;
static void Check(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
[[maybe_unused]] static void CheckGpu(HRESULT result)
{
	if (FAILED(result))
		throw std::runtime_error("Direct3D failure " + std::to_string(result));
}
[[noreturn]] static void UnknownCompletion(const char* operation)
{
	std::fprintf(stderr, "FAIL: %s; preserving native owners until process exit\n", operation);
	std::fflush(nullptr);
	std::_Exit(1);
}

static bool EnvOn(const char* name)
{
	const char* value = std::getenv(name);
	return value && *value && std::strcmp(value, "0") != 0;
}

static std::uint32_t EnvironmentCount(const char* name, std::uint32_t fallback, std::uint32_t minimum)
{
	const char* value = std::getenv(name);
	if (!value || !*value)
		return fallback;
	const std::string_view number(value);
	std::uint32_t parsed = 0;
	const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
	if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || parsed < minimum)
		throw std::runtime_error(std::string("Invalid ") + name);
	return parsed;
}

#ifdef SIMXR_PROBE_OPENGL
template <typename T> static T OpenGLProc(const char* name)
{
	const auto procedure = wglGetProcAddress(name);
	const auto address = std::bit_cast<std::uintptr_t>(procedure);
	if (address <= 3 || address == std::numeric_limits<std::uintptr_t>::max())
		throw std::runtime_error(std::string("Missing OpenGL procedure: ") + name);
	return std::bit_cast<T>(procedure);
}

struct OpenGLApplication
{
	HWND m_window = nullptr;
	HDC m_device = nullptr;
	HGLRC m_context = nullptr;
	GLuint m_framebuffer = 0;
	PFNGLDELETEFRAMEBUFFERSPROC m_deleteFramebuffers = nullptr;
	PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC m_framebufferTextureLayer = nullptr;
	PFNGLBINDFRAMEBUFFERPROC m_bindFramebuffer = nullptr;
	PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC m_framebufferStatus = nullptr;

	OpenGLApplication()
	{
		try
		{
			m_window = CreateWindowExW(0, L"STATIC", L"OpenGL loader probe", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
									   GetModuleHandleW(nullptr), nullptr);
			if (!m_window)
				throw std::runtime_error("Create OpenGL window");
			m_device = GetDC(m_window);
			if (!m_device)
				throw std::runtime_error("Get OpenGL drawable");
			PIXELFORMATDESCRIPTOR format{};
			format.nSize = sizeof(format);
			format.nVersion = 1;
			format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
			format.iPixelType = PFD_TYPE_RGBA;
			format.cColorBits = 32;
			const int pixelFormat = ChoosePixelFormat(m_device, &format);
			if (!pixelFormat || !SetPixelFormat(m_device, pixelFormat, &format))
				throw std::runtime_error("Set OpenGL pixel format");
			m_context = wglCreateContext(m_device);
			if (!m_context || !wglMakeCurrent(m_device, m_context))
				throw std::runtime_error("Create OpenGL application context");
			m_deleteFramebuffers = OpenGLProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers");
			m_framebufferTextureLayer =
				OpenGLProc<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>("glNamedFramebufferTextureLayer");
			m_bindFramebuffer = OpenGLProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
			m_framebufferStatus = OpenGLProc<PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC>("glCheckNamedFramebufferStatus");
			OpenGLProc<PFNGLCREATEFRAMEBUFFERSPROC>("glCreateFramebuffers")(1, &m_framebuffer);
			if (!m_framebuffer || glGetError() != GL_NO_ERROR)
				throw std::runtime_error("Create OpenGL framebuffer");
		}
		catch (...)
		{
			Close();
			throw;
		}
	}
	OpenGLApplication(const OpenGLApplication&) = delete;
	OpenGLApplication& operator=(const OpenGLApplication&) = delete;
	~OpenGLApplication()
	{
		Close();
	}

	void Close() noexcept
	{
		if (m_context)
		{
			if (m_framebuffer)
			{
				if (!wglMakeCurrent(m_device, m_context))
					UnknownCompletion("Restore OpenGL context for framebuffer retirement");
				m_deleteFramebuffers(1, &m_framebuffer);
				if (glGetError() != GL_NO_ERROR)
					UnknownCompletion("Delete OpenGL framebuffer");
				m_framebuffer = 0;
			}
			if (wglGetCurrentContext() == m_context && !wglMakeCurrent(nullptr, nullptr))
				UnknownCompletion("Release current OpenGL context");
			if (!wglDeleteContext(m_context))
				UnknownCompletion("Destroy OpenGL context");
			m_context = nullptr;
		}
		if (m_device)
		{
			if (!ReleaseDC(m_window, m_device))
				UnknownCompletion("Release OpenGL drawable");
			m_device = nullptr;
		}
		if (m_window)
		{
			if (!DestroyWindow(m_window))
				UnknownCompletion("Destroy OpenGL window");
			m_window = nullptr;
		}
	}
	void Clear(GLuint texture, GLuint layer, const float* color)
	{
		m_framebufferTextureLayer(m_framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, static_cast<GLint>(layer));
		if (m_framebufferStatus(m_framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			throw std::runtime_error("Incomplete OpenGL swapchain framebuffer");
		m_bindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
		glEnable(GL_FRAMEBUFFER_SRGB);
		glClearColor(color[0], color[1], color[2], color[3]);
		glClear(GL_COLOR_BUFFER_BIT);
		if (glGetError() != GL_NO_ERROR)
			throw std::runtime_error("Render OpenGL swapchain");
	}
};
#endif
#ifdef SIMXR_PROBE_D3D12
struct D3D12Application
{
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_queue;
	ComPtr<ID3D12CommandAllocator> m_allocator;
	ComPtr<ID3D12GraphicsCommandList> m_commands;
	ComPtr<ID3D12DescriptorHeap> m_targets;
	ComPtr<ID3D12Fence> m_fence;
	std::uint64_t m_submitted = 0;
	D3D12Application(IDXGIAdapter1* adapter, D3D_FEATURE_LEVEL level)
	{
		CheckGpu(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&m_device)));
		D3D12_COMMAND_QUEUE_DESC queueInfo{};
		CheckGpu(m_device->CreateCommandQueue(&queueInfo, IID_PPV_ARGS(&m_queue)));
		CheckGpu(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator)));
		CheckGpu(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr,
											 IID_PPV_ARGS(&m_commands)));
		CheckGpu(m_commands->Close());
		D3D12_DESCRIPTOR_HEAP_DESC heap{};
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap.NumDescriptors = 1;
		CheckGpu(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_targets)));
		CheckGpu(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	}
	D3D12Application(const D3D12Application&) = delete;
	D3D12Application& operator=(const D3D12Application&) = delete;
	void Clear(ID3D12Resource* texture, UINT layer, const float* color)
	{
		CheckGpu(m_allocator->Reset());
		CheckGpu(m_commands->Reset(m_allocator.Get(), nullptr));
		D3D12_RENDER_TARGET_VIEW_DESC view{};
		view.Format = texture->GetDesc().Format;
		if (texture->GetDesc().SampleDesc.Count > 1)
		{
			view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
			view.Texture2DMSArray.FirstArraySlice = layer;
			view.Texture2DMSArray.ArraySize = 1;
		}
		else
		{
			view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			view.Texture2DArray.FirstArraySlice = layer;
			view.Texture2DArray.ArraySize = 1;
		}
		const auto target = m_targets->GetCPUDescriptorHandleForHeapStart();
		m_device->CreateRenderTargetView(texture, &view, target);
		m_commands->ClearRenderTargetView(target, color, 0, nullptr);
		CheckGpu(m_commands->Close());
		ID3D12CommandList* list = m_commands.Get();
		m_queue->ExecuteCommandLists(1, &list);
		if (FAILED(m_queue->Signal(m_fence.Get(), ++m_submitted)))
			UnknownCompletion("Signal D3D12 clear");
		const auto deadline = GetTickCount64() + 5000;
		while (m_fence->GetCompletedValue() < m_submitted && GetTickCount64() < deadline)
			Sleep(1);
		if (m_fence->GetCompletedValue() < m_submitted || m_fence->GetCompletedValue() == UINT64_MAX)
			UnknownCompletion("Application D3D12 clear did not complete");
	}
};
#endif
static int Run(int argc, char** argv)
{
	try
	{
		const auto limit = EnvironmentCount("XR_PROBE_FRAMES", 120, 2);
		const auto sessions = EnvironmentCount("XR_PROBE_SESSIONS", 1, 1);
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "Native companion probe");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
#ifdef SIMXR_PROBE_OPENGL
		const char* extension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#elif defined(SIMXR_PROBE_D3D12)
		const char* extension = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
#else
		const char* extension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
#endif
		bool quadViews = EnvOn("XR_PROBE_QUAD");
		bool foveated = EnvOn("XR_PROBE_FOVEATED");
		bool multisampled = false;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument(argv[index]);
			if (argument == "--quad")
				quadViews = true;
			else if (argument == "--foveated")
				foveated = true;
			else if (argument == "--msaa")
				multisampled = true;
			else
				throw std::runtime_error("Unknown probe argument: " + std::string(argument));
		}
		if (foveated && !quadViews)
			throw std::runtime_error("Foveated probe requires quad views");
		const std::uint32_t viewCount = quadViews ? 4 : 2;
		const auto viewType =
			quadViews ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		std::vector<const char*> extensions{extension};
		if (quadViews)
			extensions.push_back(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
		if (foveated)
			extensions.push_back(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
		create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
		create.enabledExtensionNames = extensions.data();
		XrInstance instance;
		Check(xrCreateInstance(&create, &instance), "Create instance");
		char resultName[XR_MAX_RESULT_STRING_SIZE]{};
		char structureName[XR_MAX_STRUCTURE_NAME_SIZE]{};
		Check(xrResultToString(instance, XR_ERROR_HANDLE_INVALID, resultName), "Name result");
		Check(xrStructureTypeToString(instance, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, structureName), "Name structure");
		if (std::strcmp(resultName, "XR_ERROR_HANDLE_INVALID") != 0 ||
			std::strcmp(structureName, "XR_TYPE_GRAPHICS_BINDING_D3D11_KHR") != 0)
			throw std::runtime_error("Runtime returned a placeholder enum name");
		Check(xrResultToString(instance, static_cast<XrResult>(-123456789), resultName), "Name unknown result");
		if (std::strcmp(resultName, "XR_UNKNOWN_FAILURE_-123456789") != 0 ||
			xrResultToString(instance, XR_SUCCESS, nullptr) != XR_ERROR_VALIDATION_FAILURE ||
			xrStructureTypeToString(instance, XR_TYPE_INSTANCE_PROPERTIES, nullptr) != XR_ERROR_VALIDATION_FAILURE ||
			xrResultToString(XR_NULL_HANDLE, XR_SUCCESS, resultName) != XR_ERROR_HANDLE_INVALID ||
			xrStructureTypeToString(XR_NULL_HANDLE, XR_TYPE_INSTANCE_PROPERTIES, structureName) !=
				XR_ERROR_HANDLE_INVALID)
			throw std::runtime_error("Invalid runtime stringification result or argument validation");
		XrSystemGetInfo getSystem{XR_TYPE_SYSTEM_GET_INFO};
		getSystem.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrSystemId system;
		Check(xrGetSystem(instance, &getSystem, &system), "Get system");
		std::uint32_t configurationCount = 0;
		Check(xrEnumerateViewConfigurations(instance, system, 0, &configurationCount, nullptr), "Count configurations");
		if (configurationCount != (quadViews ? 2u : 1u))
			throw std::runtime_error("Incorrect enabled configuration count");
		std::vector<XrViewConfigurationType> configurations(configurationCount);
		if (quadViews && xrEnumerateViewConfigurations(instance, system, 1, &configurationCount,
													   configurations.data()) != XR_ERROR_SIZE_INSUFFICIENT)
			throw std::runtime_error("Undersized configuration enumeration accepted");
		Check(xrEnumerateViewConfigurations(instance, system, configurationCount, &configurationCount,
											configurations.data()),
			  "Enumerate configurations");
		if (configurations.front() != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO || configurations.back() != viewType)
			throw std::runtime_error("Incorrect enabled configurations");
		XrViewConfigurationProperties properties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
		if (!quadViews &&
			xrGetViewConfigurationProperties(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO,
											 &properties) != XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED)
			throw std::runtime_error("Disabled quad configuration accepted");
		Check(xrGetViewConfigurationProperties(instance, system, viewType, &properties), "Configuration properties");
		std::uint32_t recommendedCount = 0;
		Check(xrEnumerateViewConfigurationViews(instance, system, viewType, 0, &recommendedCount, nullptr),
			  "Count views");
		if (recommendedCount != viewCount)
			throw std::runtime_error("Incorrect view count");
		std::vector<XrViewConfigurationView> recommended(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		recommended[0].recommendedImageRectWidth = 123;
		if (xrEnumerateViewConfigurationViews(instance, system, viewType, viewCount - 1, &recommendedCount,
											  recommended.data()) != XR_ERROR_SIZE_INSUFFICIENT ||
			recommended[0].recommendedImageRectWidth != 123)
			throw std::runtime_error("Undersized view enumeration mutated output");
		Check(xrEnumerateViewConfigurationViews(instance, system, viewType, viewCount, &recommendedCount,
												recommended.data()),
			  "Enumerate views");
		const auto maximumSamples = recommended.front().maxSwapchainSampleCount;
		for (const auto& view : recommended)
		{
			if (view.recommendedSwapchainSampleCount != 1 || !maximumSamples ||
				view.maxSwapchainSampleCount != maximumSamples)
				throw std::runtime_error("Invalid sample count recommendation");
		}
		if (multisampled && maximumSamples == 1)
			throw std::runtime_error("MSAA requested but unavailable");
		std::printf("Runtime maximum samples: %u\n", maximumSamples);
		if (quadViews && (!recommended[2].recommendedImageRectWidth || !recommended[2].recommendedImageRectHeight ||
						  recommended[2].recommendedImageRectWidth >= recommended[0].recommendedImageRectWidth))
			throw std::runtime_error("Incorrect inset dimensions");
		if (foveated)
		{
			XrFoveatedViewConfigurationViewVARJO request{XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO, nullptr,
														 XR_TRUE};
			recommended[2].next = &request;
			Check(xrEnumerateViewConfigurationViews(instance, system, viewType, viewCount, &recommendedCount,
													recommended.data()),
				  "Foveated view dimensions");
			request.foveatedRenderingActive = 2;
			if (xrEnumerateViewConfigurationViews(instance, system, viewType, viewCount, &recommendedCount,
												  recommended.data()) != XR_ERROR_VALIDATION_FAILURE)
				throw std::runtime_error("Invalid foveation boolean accepted");
			request.foveatedRenderingActive = XR_TRUE;
			request.next = &request;
			if (xrEnumerateViewConfigurationViews(instance, system, viewType, viewCount, &recommendedCount,
												  recommended.data()) != XR_ERROR_VALIDATION_FAILURE)
				throw std::runtime_error("Cyclic foveation chain accepted");
			recommended[2].next = nullptr;
		}
		std::uint32_t blendCount = 0;
		XrEnvironmentBlendMode blendMode{};
		Check(xrEnumerateEnvironmentBlendModes(instance, system, viewType, 1, &blendCount, &blendMode), "Blend mode");
		if (blendCount != 1 || blendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
			throw std::runtime_error("Incorrect blend mode");
#ifdef SIMXR_PROBE_OPENGL
		PFN_xrGetOpenGLGraphicsRequirementsKHR requirementsFunction;
		Check(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
									reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFunction)),
			  "Get OpenGL requirements function");
		XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
		Check(requirementsFunction(instance, system, &requirements), "Get OpenGL requirements");
		OpenGLApplication application;
		XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
		binding.hDC = application.m_device;
		binding.hGLRC = application.m_context;
#else
#ifdef SIMXR_PROBE_D3D12
		PFN_xrGetD3D12GraphicsRequirementsKHR requirementsFunction;
		Check(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
									reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFunction)),
			  "Get D3D12 requirements function");
		XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		Check(requirementsFunction(instance, system, &requirements), "Get D3D12 requirements");
#else
		PFN_xrGetD3D11GraphicsRequirementsKHR requirementsFunction;
		Check(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
									reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFunction)),
			  "Get D3D11 requirements function");
		XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
		Check(requirementsFunction(instance, system, &requirements), "Get D3D11 requirements");
#endif
		ComPtr<IDXGIFactory1> factory;
		CheckGpu(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT index = 0;; ++index)
		{
			CheckGpu(factory->EnumAdapters1(index, &adapter));
			DXGI_ADAPTER_DESC1 description{};
			CheckGpu(adapter->GetDesc1(&description));
			if (!std::memcmp(&description.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)))
				break;
			adapter.Reset();
		}
#ifdef SIMXR_PROBE_D3D12
		D3D12Application application(adapter.Get(), requirements.minFeatureLevel);
		XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
		binding.device = application.m_device.Get();
		binding.queue = application.m_queue.Get();
#else
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		CheckGpu(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &requirements.minFeatureLevel, 1,
								   D3D11_SDK_VERSION, &device, nullptr, &context));
		XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
		binding.device = device.Get();
#endif
#endif
		try
		{
			XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding};
			sessionInfo.systemId = system;
			XrPath right, triggerPath, profile;
			Check(xrStringToPath(instance, "/user/hand/right", &right), "Right hand path");
			Check(xrStringToPath(instance, "/user/hand/right/input/trigger/value", &triggerPath), "Trigger path");
			Check(xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile), "Profile path");
			XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
			std::strcpy(actionSetInfo.actionSetName, "probe");
			std::strcpy(actionSetInfo.localizedActionSetName, "Probe");
			XrActionSet actionSet;
			Check(xrCreateActionSet(instance, &actionSetInfo, &actionSet), "Create action set");
			XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
			actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
			std::strcpy(actionInfo.actionName, "trigger");
			std::strcpy(actionInfo.localizedActionName, "Trigger");
			actionInfo.countSubactionPaths = 1;
			actionInfo.subactionPaths = &right;
			XrAction trigger;
			Check(xrCreateAction(actionSet, &actionInfo, &trigger), "Create trigger action");
			const XrActionSuggestedBinding suggested{trigger, triggerPath};
			XrInteractionProfileSuggestedBinding suggestions{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
			suggestions.interactionProfile = profile;
			suggestions.countSuggestedBindings = 1;
			suggestions.suggestedBindings = &suggested;
			Check(xrSuggestInteractionProfileBindings(instance, &suggestions), "Suggest trigger binding");
			bool pressed = false, released = false;
			const bool resize = EnvOn("XR_PROBE_RESIZE");
			for (unsigned cycle = 0; cycle < sessions; ++cycle)
			{
				XrSession session = XR_NULL_HANDLE;
#ifdef SIMXR_PROBE_OPENGL
				auto missingBinding = sessionInfo;
				missingBinding.next = nullptr;
				if (xrCreateSession(instance, &missingBinding, &session) != XR_ERROR_GRAPHICS_DEVICE_INVALID ||
					session != XR_NULL_HANDLE)
					throw std::runtime_error("Failed session creation changed output or retained session capacity");
				Check(xrCreateSession(instance, &sessionInfo, &session), "Create session after rejected binding");
#else
				std::latch start(2);
				const auto createSession = [&]
				{
					XrSession output = XR_NULL_HANDLE;
					start.arrive_and_wait();
					const auto result = xrCreateSession(instance, &sessionInfo, &output);
					return std::pair{result, output};
				};
				auto first = std::async(std::launch::async, createSession);
				const auto secondResult = createSession();
				if (first.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
					UnknownCompletion("Concurrent session creation did not finish");
				const auto firstResult = first.get();
				const auto& accepted = firstResult.first == XR_SUCCESS ? firstResult : secondResult;
				const auto& rejected = firstResult.first == XR_SUCCESS ? secondResult : firstResult;
				if (accepted.first != XR_SUCCESS || accepted.second == XR_NULL_HANDLE ||
					rejected.first != XR_ERROR_LIMIT_REACHED || rejected.second != XR_NULL_HANDLE)
					throw std::runtime_error("Concurrent session creation corrupted admission or failed output");
				session = accepted.second;
#endif

				XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
				attach.countActionSets = 1;
				attach.actionSets = &actionSet;
				Check(xrAttachSessionActionSets(session, &attach), "Attach trigger action");
				XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
				spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
				spaceInfo.poseInReferenceSpace.orientation.w = 1;
				XrSpace space;
				Check(xrCreateReferenceSpace(session, &spaceInfo, &space), "Create space");
				std::uint32_t spaceCount = 0;
				Check(xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr), "Count spaces");
				if (spaceCount != (foveated ? 4u : 3u))
					throw std::runtime_error("Incorrect reference space count");
				std::vector<XrReferenceSpaceType> spaces(spaceCount);
				Check(xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, spaces.data()), "Enumerate spaces");
				XrSpace combinedEye = XR_NULL_HANDLE;
				if (foveated)
				{
					if (spaces.back() != XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO)
						throw std::runtime_error("Combined eye space missing");
					spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
					Check(xrCreateReferenceSpace(session, &spaceInfo, &combinedEye), "Create combined eye space");
				}
				XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
				swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
#ifdef SIMXR_PROBE_OPENGL
				swapchainInfo.format = GL_SRGB8_ALPHA8;
#else
				swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
#endif
				swapchainInfo.sampleCount = multisampled ? maximumSamples : 1;
				swapchainInfo.width = 256;
				swapchainInfo.height = 256;
				swapchainInfo.faceCount = swapchainInfo.mipCount = 1;
				swapchainInfo.arraySize = viewCount;
				for (unsigned invalidField = 0; invalidField < 5; ++invalidField)
				{
					auto invalid = swapchainInfo;
					if (invalidField == 0)
						invalid.type = XR_TYPE_UNKNOWN;
					if (invalidField == 1)
						invalid.arraySize = 0;
					if (invalidField == 2)
						invalid.mipCount = 0;
					if (invalidField == 3)
						invalid.sampleCount = 0;
					if (invalidField == 4)
						invalid.usageFlags = 0;
					XrSwapchain rejected = XR_NULL_HANDLE;
					if (xrCreateSwapchain(session, &invalid, &rejected) != XR_ERROR_VALIDATION_FAILURE || rejected)
						throw std::runtime_error("Malformed swapchain description was accepted");
				}
#ifndef SIMXR_PROBE_OPENGL
				const std::array<std::int64_t, 3> unsupportedFormats{swapchainInfo.format + (std::int64_t{1} << 32),
																	 swapchainInfo.format - (std::int64_t{1} << 32),
																	 DXGI_FORMAT_R8_UNORM};
				for (const auto format : unsupportedFormats)
				{
					auto invalid = swapchainInfo;
					invalid.format = format;
					XrSwapchain rejected = XR_NULL_HANDLE;
					if (xrCreateSwapchain(session, &invalid, &rejected) != XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED ||
						rejected)
						throw std::runtime_error("Unadvertised or narrowed Direct3D format was accepted");
				}
#endif
				XrSwapchain swapchain;
				std::uint32_t count = 0;
#ifdef SIMXR_PROBE_OPENGL
				using Image = XrSwapchainImageOpenGLKHR;
				constexpr auto imageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
#elif defined(SIMXR_PROBE_D3D12)
				using Image = XrSwapchainImageD3D12KHR;
				constexpr auto imageType = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
#else
				using Image = XrSwapchainImageD3D11KHR;
				constexpr auto imageType = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
#endif
				std::vector<Image> images;
				const auto createSwapchain = [&]
				{
					Check(xrCreateSwapchain(session, &swapchainInfo, &swapchain), "Create swapchain");
					Check(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), "Count images");
					if (!count)
						throw std::runtime_error("Swapchain has no images");
					images.assign(count, {imageType});
					Check(xrEnumerateSwapchainImages(swapchain, count, &count,
													 reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
						  "Enumerate images");
				};
				createSwapchain();
				const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				bool running = false;
				unsigned frames = 0;
				while (frames < limit)
				{
					XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
					XrResult pollResult;
					while ((pollResult = xrPollEvent(instance, &event)) == XR_SUCCESS)
					{
						if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
						{
							const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
							if (changed.state == XR_SESSION_STATE_READY)
							{
								XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
								begin.primaryViewConfigurationType = viewType;
								Check(xrBeginSession(session, &begin), "Begin session");
								running = true;
							}
						}
						event = {XR_TYPE_EVENT_DATA_BUFFER};
					}
					Check(pollResult, "Poll session events");
					if (!running)
					{
						if (std::chrono::steady_clock::now() >= readyDeadline)
							throw std::runtime_error("Session did not become ready");
						Sleep(1);
						continue;
					}
					if (resize && frames == limit / 2)
					{
						Check(xrDestroySwapchain(swapchain), "Destroy original swapchain");
						swapchainInfo.width = 384;
						swapchainInfo.height = 192;
						createSwapchain();
						std::printf("Session %u: resized stereo swapchain to %ux%u\n", cycle + 1, swapchainInfo.width,
									swapchainInfo.height);
					}
					XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
					XrFrameState state{XR_TYPE_FRAME_STATE};
					Check(xrWaitFrame(session, &wait, &state), "Wait frame");
					XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
					Check(xrBeginFrame(session, &begin), "Begin frame");
					const XrActiveActionSet active{actionSet, XR_NULL_PATH};
					XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
					sync.countActiveActionSets = 1;
					sync.activeActionSets = &active;
					Check(xrSyncActions(session, &sync), "Sync trigger action");
					XrActionStateGetInfo getAction{XR_TYPE_ACTION_STATE_GET_INFO};
					getAction.action = trigger;
					getAction.subactionPath = right;
					XrActionStateFloat action{XR_TYPE_ACTION_STATE_FLOAT};
					Check(xrGetActionStateFloat(session, &getAction, &action), "Read trigger action");
					if (action.isActive && action.currentState > 0.5f && !pressed)
					{
						pressed = true;
						std::puts("APP observed right trigger press");
					}
					if (pressed && action.isActive && action.currentState == 0 && !released)
					{
						released = true;
						std::puts("APP observed right trigger release");
					}
					XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
					locate.viewConfigurationType = viewType;
					locate.displayTime = state.predictedDisplayTime;
					locate.space = space;
					XrViewState viewState{XR_TYPE_VIEW_STATE};
					XrViewLocateFoveatedRenderingVARJO foveation{XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO, nullptr,
																 XR_TRUE};
					if (foveated)
						locate.next = &foveation;
					std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
					Check(xrLocateViews(session, &locate, &viewState, viewCount, &count, views.data()), "Locate views");
					if (count != viewCount)
						throw std::runtime_error("Incorrect located view count");
					if (combinedEye)
					{
						XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
						Check(xrLocateSpace(combinedEye, space, state.predictedDisplayTime, &location),
							  "Locate combined eye space");
					}
					XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
					std::uint32_t index;
					Check(xrAcquireSwapchainImage(swapchain, &acquire, &index), "Acquire image");
					if (index >= images.size())
						throw std::runtime_error("Acquired image index is out of bounds");
					XrSwapchainImageWaitInfo waitImage{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
					waitImage.timeout = XR_INFINITE_DURATION;
					Check(xrWaitSwapchainImage(swapchain, &waitImage), "Wait image");
					std::vector<XrCompositionLayerProjectionView> projectionViews(viewCount);
					for (UINT eye = 0; eye < viewCount; ++eye)
					{
						const float color[]{eye % 2 == 0 ? 1.0f : 0.0f, eye % 2 == 1 ? 1.0f : 0.0f,
											eye >= 2 ? 1.0f : 0.1f, 1};
#ifdef SIMXR_PROBE_OPENGL
						application.Clear(images[index].image, eye, color);
#elif defined(SIMXR_PROBE_D3D12)
						application.Clear(images[index].texture, eye, color);
#else
						D3D11_RENDER_TARGET_VIEW_DESC targetInfo{};
						targetInfo.Format = static_cast<DXGI_FORMAT>(swapchainInfo.format);
						if (multisampled)
						{
							targetInfo.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
							targetInfo.Texture2DMSArray.FirstArraySlice = eye;
							targetInfo.Texture2DMSArray.ArraySize = 1;
						}
						else
						{
							targetInfo.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
							targetInfo.Texture2DArray.FirstArraySlice = eye;
							targetInfo.Texture2DArray.ArraySize = 1;
						}
						ComPtr<ID3D11RenderTargetView> target;
						CheckGpu(device->CreateRenderTargetView(images[index].texture, &targetInfo, &target));
						context->ClearRenderTargetView(target.Get(), color);
#endif
						projectionViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
						projectionViews[eye].pose = views[eye].pose;
						projectionViews[eye].fov = views[eye].fov;
						projectionViews[eye].subImage = {swapchain,
														 {{0, 0},
														  {static_cast<std::int32_t>(swapchainInfo.width),
														   static_cast<std::int32_t>(swapchainInfo.height)}},
														 eye};
					}
					XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					Check(xrReleaseSwapchainImage(swapchain, &release), "Release image");
					XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
					projection.space = space;
					projection.viewCount = viewCount;
					projection.views = projectionViews.data();
					const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
					XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
					end.displayTime = state.predictedDisplayTime;
					end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
					end.layerCount = 1;
					end.layers = &layer;
					Check(xrEndFrame(session, &end), "End frame");
					++frames;
				}
				Check(xrRequestExitSession(session), "Request exit");
				Check(xrEndSession(session), "End session");
				Check(xrDestroySwapchain(swapchain), "Destroy swapchain");
				Check(xrDestroySpace(space), "Destroy space");
				if (combinedEye)
					Check(xrDestroySpace(combinedEye), "Destroy combined eye space");
				Check(xrDestroySession(session), "Destroy session");
				std::printf("Session %u: submitted %u frames and destroyed all session resources\n", cycle + 1, frames);
			}
			Check(xrDestroyActionSet(actionSet), "Destroy action set");
			Check(xrDestroyInstance(instance), "Destroy instance");
			XrInstance recreated = XR_NULL_HANDLE;
			Check(xrCreateInstance(&create, &recreated), "Recreate instance after complete retirement");
			if (recreated == instance)
				throw std::runtime_error("Recreated instance reused a retired identity");
			XrInstanceProperties recreatedProperties{XR_TYPE_INSTANCE_PROPERTIES};
			Check(xrGetInstanceProperties(recreated, &recreatedProperties), "Query recreated instance");
			Check(xrDestroyInstance(recreated), "Destroy recreated instance");

			if (EnvOn("XR_PROBE_REQUIRE_INPUT") && (!pressed || !released))
				throw std::runtime_error("Application did not observe both trigger press and release");
			std::printf("PASS: native loader completed %u sessions, %u frames each, views=%u, foveated=%s, resize=%s\n",
						sessions, limit, viewCount, foveated ? "yes" : "no", resize ? "yes" : "no");
			return 0;
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "FAIL: %s\n", error.what());
			UnknownCompletion("Probe failed while native graphics parents were live");
		}
		catch (...)
		{
			UnknownCompletion("Unexpected probe failure while native graphics parents were live");
		}
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}

} // namespace simxr::probe

int main(int argc, char** argv)
{
	return simxr::probe::Run(argc, argv);
}
