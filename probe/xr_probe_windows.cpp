#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
void Check(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
void CheckGpu(HRESULT result)
{
	if (FAILED(result)) throw std::runtime_error("Direct3D failure " + std::to_string(result));
}
#ifdef SIMXR_PROBE_OPENGL
struct OpenGLApplication
{
	HWND window = nullptr;
	HDC device = nullptr;
	HGLRC context = nullptr;
	GLuint framebuffer = 0;
	OpenGLApplication()
	{
		window = CreateWindowExW(0, L"STATIC", L"OpenGL loader probe", WS_POPUP, 0, 0, 16, 16,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		device = GetDC(window);
		PIXELFORMATDESCRIPTOR format{};
		format.nSize = sizeof(format);
		format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		format.iPixelType = PFD_TYPE_RGBA;
		format.cColorBits = 32;
		if (!window || !device || !SetPixelFormat(device, ChoosePixelFormat(device, &format), &format))
			throw std::runtime_error("Create OpenGL drawable");
		context = wglCreateContext(device);
		if (!context || !wglMakeCurrent(device, context))
			throw std::runtime_error("Create OpenGL application context");
		reinterpret_cast<PFNGLCREATEFRAMEBUFFERSPROC>(wglGetProcAddress("glCreateFramebuffers"))(1, &framebuffer);
	}
	~OpenGLApplication()
	{
		if (context)
		{
			wglMakeCurrent(device, context);
			reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(wglGetProcAddress("glDeleteFramebuffers"))(1, &framebuffer);
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(context);
		}
		if (device) ReleaseDC(window, device);
		if (window) DestroyWindow(window);
	}
	void Clear(GLuint texture, GLuint layer, const float* color)
	{
		reinterpret_cast<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>(wglGetProcAddress("glNamedFramebufferTextureLayer"))(
			framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, layer);
		reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(wglGetProcAddress("glBindFramebuffer"))(GL_FRAMEBUFFER, framebuffer);
		glEnable(GL_FRAMEBUFFER_SRGB);
		glClearColor(color[0], color[1], color[2], color[3]);
		glClear(GL_COLOR_BUFFER_BIT);
		if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Render OpenGL swapchain");
	}
};
#endif
#ifdef SIMXR_PROBE_D3D12
struct D3D12Application
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> commands;
	ComPtr<ID3D12DescriptorHeap> targets;
	ComPtr<ID3D12Fence> fence;
	std::uint64_t submitted = 0;
	D3D12Application(IDXGIAdapter1* adapter, D3D_FEATURE_LEVEL level)
	{
		CheckGpu(D3D12CreateDevice(adapter, level, IID_PPV_ARGS(&device)));
		D3D12_COMMAND_QUEUE_DESC queueInfo{};
		CheckGpu(device->CreateCommandQueue(&queueInfo, IID_PPV_ARGS(&queue)));
		CheckGpu(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
		CheckGpu(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
			IID_PPV_ARGS(&commands)));
		CheckGpu(commands->Close());
		D3D12_DESCRIPTOR_HEAP_DESC heap{};
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap.NumDescriptors = 1;
		CheckGpu(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)));
		CheckGpu(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	}
	void Clear(ID3D12Resource* texture, UINT layer, const float* color)
	{
		CheckGpu(allocator->Reset());
		CheckGpu(commands->Reset(allocator.Get(), nullptr));
		D3D12_RENDER_TARGET_VIEW_DESC view{};
		view.Format = texture->GetDesc().Format;
		view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		view.Texture2DArray.FirstArraySlice = layer;
		view.Texture2DArray.ArraySize = 1;
		const auto target = targets->GetCPUDescriptorHandleForHeapStart();
		device->CreateRenderTargetView(texture, &view, target);
		commands->ClearRenderTargetView(target, color, 0, nullptr);
		CheckGpu(commands->Close());
		ID3D12CommandList* list = commands.Get();
		queue->ExecuteCommandLists(1, &list);
		CheckGpu(queue->Signal(fence.Get(), ++submitted));
		const auto deadline = GetTickCount64() + 5000;
		while (fence->GetCompletedValue() < submitted && GetTickCount64() < deadline)
			Sleep(1);
		if (fence->GetCompletedValue() < submitted || fence->GetCompletedValue() == UINT64_MAX)
			throw std::runtime_error("Application D3D12 clear did not complete");
	}
};
#endif
int main()
{
	try
	{
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
		create.enabledExtensionCount = 1;
		create.enabledExtensionNames = &extension;
		XrInstance instance;
		Check(xrCreateInstance(&create, &instance), "Create instance");
		XrSystemGetInfo getSystem{XR_TYPE_SYSTEM_GET_INFO};
		getSystem.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrSystemId system;
		Check(xrGetSystem(instance, &getSystem, &system), "Get system");
#ifdef SIMXR_PROBE_OPENGL
		PFN_xrGetOpenGLGraphicsRequirementsKHR requirementsFunction;
		Check(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFunction)), "Get OpenGL requirements function");
		XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
		Check(requirementsFunction(instance, system, &requirements), "Get OpenGL requirements");
		OpenGLApplication application;
		XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
		binding.hDC = application.device;
		binding.hGLRC = application.context;
#else
#ifdef SIMXR_PROBE_D3D12
		PFN_xrGetD3D12GraphicsRequirementsKHR requirementsFunction;
		Check(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFunction)), "Get D3D12 requirements function");
		XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		Check(requirementsFunction(instance, system, &requirements), "Get D3D12 requirements");
#else
		PFN_xrGetD3D11GraphicsRequirementsKHR requirementsFunction;
		Check(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFunction)), "Get D3D11 requirements function");
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
			if (!std::memcmp(&description.AdapterLuid, &requirements.adapterLuid, sizeof(LUID))) break;
			adapter.Reset();
		}
#ifdef SIMXR_PROBE_D3D12
		D3D12Application application(adapter.Get(), requirements.minFeatureLevel);
		XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
		binding.device = application.device.Get();
		binding.queue = application.queue.Get();
#else
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		CheckGpu(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
			&requirements.minFeatureLevel, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
		XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
		binding.device = device.Get();
#endif
#endif
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
		const auto* requested = std::getenv("XR_PROBE_FRAMES");
		const unsigned limit = requested ? std::strtoul(requested, nullptr, 10) : 120;
		const auto* requestedSessions = std::getenv("XR_PROBE_SESSIONS");
		const unsigned sessions = requestedSessions ? std::strtoul(requestedSessions, nullptr, 10) : 1;
		const bool resize = std::getenv("XR_PROBE_RESIZE") != nullptr;
		if (limit < 2 || !sessions)
			throw std::runtime_error("Probe requires at least two frames and one session");
		for (unsigned cycle = 0; cycle < sessions; ++cycle)
		{
			XrSession session;
			Check(xrCreateSession(instance, &sessionInfo, &session), "Create session");
			XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
			attach.countActionSets = 1;
			attach.actionSets = &actionSet;
			Check(xrAttachSessionActionSets(session, &attach), "Attach trigger action");
			XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			spaceInfo.poseInReferenceSpace.orientation.w = 1;
			XrSpace space;
			Check(xrCreateReferenceSpace(session, &spaceInfo, &space), "Create space");
			XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
			swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
#ifdef SIMXR_PROBE_OPENGL
			swapchainInfo.format = GL_SRGB8_ALPHA8;
#else
			swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
#endif
			swapchainInfo.sampleCount = 1;
			swapchainInfo.width = 256;
			swapchainInfo.height = 256;
			swapchainInfo.faceCount = swapchainInfo.mipCount = 1;
			swapchainInfo.arraySize = 2;
			for (unsigned invalidField = 0; invalidField < 5; ++invalidField)
			{
				auto invalid = swapchainInfo;
				if (invalidField == 0) invalid.type = XR_TYPE_UNKNOWN;
				if (invalidField == 1) invalid.arraySize = 0;
				if (invalidField == 2) invalid.mipCount = 0;
				if (invalidField == 3) invalid.sampleCount = 0;
				if (invalidField == 4) invalid.usageFlags = 0;
				XrSwapchain rejected = XR_NULL_HANDLE;
				if (xrCreateSwapchain(session, &invalid, &rejected) != XR_ERROR_VALIDATION_FAILURE || rejected)
					throw std::runtime_error("Malformed swapchain description was accepted");
			}
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
				images.assign(count, {imageType});
				Check(xrEnumerateSwapchainImages(swapchain, count, &count,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "Enumerate images");
			};
			createSwapchain();
			bool running = false;
			unsigned frames = 0;
			while (frames < limit)
			{
				XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
				while (xrPollEvent(instance, &event) == XR_SUCCESS)
				{
					if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
					{
						const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
						if (changed.state == XR_SESSION_STATE_READY)
						{
							XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
							begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
							Check(xrBeginSession(session, &begin), "Begin session");
							running = true;
						}
					}
					event = {XR_TYPE_EVENT_DATA_BUFFER};
				}
				if (!running)
				{
					Sleep(1);
					continue;
				}
				if (resize && frames == limit / 2)
				{
					Check(xrDestroySwapchain(swapchain), "Destroy original swapchain");
					swapchainInfo.width = 384;
					swapchainInfo.height = 192;
					createSwapchain();
					std::printf("Session %u: resized stereo swapchain to %ux%u\n", cycle + 1,
						swapchainInfo.width, swapchainInfo.height);
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
				locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				locate.displayTime = state.predictedDisplayTime;
				locate.space = space;
				XrViewState viewState{XR_TYPE_VIEW_STATE};
				std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
				Check(xrLocateViews(session, &locate, &viewState, 2, &count, views.data()), "Locate views");
				XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				std::uint32_t index;
				Check(xrAcquireSwapchainImage(swapchain, &acquire, &index), "Acquire image");
				XrSwapchainImageWaitInfo waitImage{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				waitImage.timeout = XR_INFINITE_DURATION;
				Check(xrWaitSwapchainImage(swapchain, &waitImage), "Wait image");
				std::array<XrCompositionLayerProjectionView, 2> projectionViews;
				for (UINT eye = 0; eye < 2; ++eye)
				{
					const float color[]{eye == 0 ? 1.0f : 0.0f, eye == 1 ? 1.0f : 0.0f, 0.1f, 1};
#ifdef SIMXR_PROBE_OPENGL
					application.Clear(images[index].image, eye, color);
#elif defined(SIMXR_PROBE_D3D12)
					application.Clear(images[index].texture, eye, color);
#else
					D3D11_RENDER_TARGET_VIEW_DESC targetInfo{};
					targetInfo.Format = static_cast<DXGI_FORMAT>(swapchainInfo.format);
					targetInfo.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					targetInfo.Texture2DArray.FirstArraySlice = eye;
					targetInfo.Texture2DArray.ArraySize = 1;
					ComPtr<ID3D11RenderTargetView> target;
					CheckGpu(device->CreateRenderTargetView(images[index].texture, &targetInfo, &target));
					context->ClearRenderTargetView(target.Get(), color);
#endif
					projectionViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
					projectionViews[eye].pose = views[eye].pose;
					projectionViews[eye].fov = views[eye].fov;
					projectionViews[eye].subImage = {swapchain, {{0, 0}, {static_cast<std::int32_t>(swapchainInfo.width), static_cast<std::int32_t>(swapchainInfo.height)}}, eye};
				}
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				Check(xrReleaseSwapchainImage(swapchain, &release), "Release image");
				XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
				projection.space = space;
				projection.viewCount = 2;
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
			Check(xrDestroySession(session), "Destroy session");
			std::printf("Session %u: submitted %u frames and destroyed all session resources\n", cycle + 1, frames);
		}
		Check(xrDestroyActionSet(actionSet), "Destroy action set");
		Check(xrDestroyInstance(instance), "Destroy instance");
		if (std::getenv("XR_PROBE_REQUIRE_INPUT") && (!pressed || !released))
			throw std::runtime_error("Application did not observe both trigger press and release");
		std::printf("PASS: native loader completed %u sessions, %u stereo frames each, resize=%s\n",
			sessions, limit, resize ? "yes" : "no");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
