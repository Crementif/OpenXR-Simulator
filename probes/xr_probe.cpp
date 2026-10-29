// Standalone replay of BetterVR's OpenXR call sequence against whatever runtime
// XR_RUNTIME_JSON points at. Mirrors src/rendering/openxr.cpp + renderer.cpp +
// swapchain.cpp so a runtime that passes this is a plausible drop-in.
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include "Pch.h"
#include "ProbeEnvironment.h"
#include "ProbeSession.h"
#include "control/ControlSchema.h"
#include "control/LocalControlClient.h"
#include "control/SimulationControl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

namespace simxr::probe
{
using Microsoft::WRL::ComPtr;
static int s_fail = 0;
static int s_warn = 0;

static void Ok(const char* what)
{
	printf("  [ ok ] %s\n", what);
}
[[noreturn]] static void Fail(const char* what, XrResult r)
{
	printf("  [FAIL] %s -> XrResult %d\n", what, static_cast<int>(r));
	throw std::runtime_error(what);
}
static void Warn(const char* what)
{
	printf("  [warn] %s\n", what);
	s_warn++;
}
static void Step(const char* what)
{
	printf("\n== %s\n", what);
}

[[noreturn]] static void UnknownCompletion(const char* operation)
{
	std::fprintf(stderr, "FAIL: %s; GPU completion is unknown, preserving native owners until process exit\n",
				 operation);
	std::fflush(nullptr);
	std::_Exit(1);
}

static std::unique_ptr<control::LocalControlClient> HoldControllerInputs()
{
	const auto* selected = std::getenv("SIMXR_CONTROL_SOCKET");
	const auto endpoint = selected && *selected ? std::string(selected) :
		std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId());
	auto client = std::make_unique<control::LocalControlClient>(endpoint);
	const auto exchange = [&](control::OpCode operation, std::vector<control::Field> fields = {})
	{
		control::ControlPacket request;
		request.message.opcode = static_cast<std::uint16_t>(operation);
		request.message.fields = std::move(fields);
		auto response = client->Request(std::move(request)).message;
		const auto* error = response.Find(control::tag::Error);
		if (!error || error->AsU32() != 0)
			throw std::runtime_error("Controller input control request failed");
		return response;
	};
	exchange(control::OpCode::Hello, {control::Field::Text(control::tag::ClientName, "XR probe"),
		control::Field::U64(control::tag::RequiredCapabilities,
			static_cast<std::uint64_t>(control::Capability::SimulationCommands))});
	const auto sessions = exchange(control::OpCode::Sessions);
	const auto* field = sessions.Find(control::tag::Sessions);
	if (!field)
		throw std::runtime_error("Control session list is missing");
	const auto entries = control::DecodeFields(field->value);
	if (entries.error != control::DecodeError::None || entries.fields.size() != 1)
		throw std::runtime_error("Controller input requires one live session");
	auto identity = control::DecodeFields(entries.fields.front().value);
	if (identity.error != control::DecodeError::None)
		throw std::runtime_error("Invalid control session identity");
	control::Message scope;
	scope.fields = std::move(identity.fields);
	const auto* sessionField = scope.Find(control::tag::Session);
	const auto* generationField = scope.Find(control::tag::Generation);
	const auto session = sessionField ? sessionField->AsU64() : std::nullopt;
	const auto generation = generationField ? generationField->AsU64() : std::nullopt;
	if (!session || !*session || !generation || !*generation)
		throw std::runtime_error("Control session identity is missing");
	std::vector<control::Field> commands;
	const auto add = [&](const SimulationCommand& command)
	{
		commands.push_back(control::EncodeSimulationCommand(command, static_cast<std::uint16_t>(commands.size() + 1)));
	};
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		add(TakeControl{hand ? ControlChannel::RightControls : ControlChannel::LeftControls});
		ControllerControls input;
		input.primary = input.primaryTouch = true;
		input.grip = input.gripForce = .7f;
		input.stick = input.trackpad = {-.5f, .25f};
		add(SetControllerControls{hand, input});
	}
	exchange(control::OpCode::SubmitSimulation,
		{control::Field::U64(control::tag::Session, *session), control::Field::U64(control::tag::Generation, *generation),
		 control::Field::U64(control::tag::TransactionId, 1), control::Field::U64(control::tag::ApplyFrame, 1),
		 control::Field::U32(control::tag::SourceKind, 0), control::Field::U32(control::tag::LatePolicy, 1),
		 control::Field::Bytes(control::tag::Commands, control::EncodeFields(commands))});
	return client;
}

static void Check(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		Fail(operation, result);
	Ok(operation);
}

static XrInstance s_instance = XR_NULL_HANDLE;
static XrPath Path(const char* value)
{
	XrPath path = XR_NULL_PATH;
	const auto result = xrStringToPath(s_instance, value, &path);
	if (XR_FAILED(result))
		Fail(value, result);
	return path;
}

static int Run(int argc, char** argv)
{
	if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--queries-only") != 0))
	{
		std::fputs("Usage: xr_probe [--queries-only]\n", stderr);
		return 2;
	}
	const int totalFrames = EnvironmentCount("SIMXR_PROBE_FRAMES", 30, 1);
	const int sleepMs = EnvironmentCount("SIMXR_PROBE_SLEEP_MS", 0, 0);
	const bool queriesOnly = argc == 2 && strcmp(argv[1], "--queries-only") == 0;
	Step("xrEnumerateInstanceExtensionProperties");
	std::uint32_t extCount = 0;
	Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "enumerate extension count");
	std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
	Check(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()), "enumerate extensions");
	bool hasD3D12 = false, hasDepth = false, hasTimeConv = false, hasDebugUtils = false;
	for (auto& e : exts)
	{
		printf("       - %s\n", e.extensionName);
		if (!strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME))
			hasD3D12 = true;
		if (!strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME))
			hasDepth = true;
		if (!strcmp(e.extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME))
			hasTimeConv = true;
		if (!strcmp(e.extensionName, XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
			hasDebugUtils = true;
	}
	if (!hasD3D12)
	{
		printf("  [FAIL] XR_KHR_D3D12_enable missing - BetterVR cannot run\n");
		return 1;
	}
	if (!hasDepth)
	{
		printf("  [FAIL] XR_KHR_composition_layer_depth missing - BetterVR requires it unconditionally\n");
		return 1;
	}
	if (!hasTimeConv)
	{
		printf(
			"  [FAIL] XR_KHR_win32_convert_performance_counter_time missing - BetterVR requires it unconditionally\n");
		return 1;
	}
	Ok("all three mandatory BetterVR extensions present");
	if (!hasDebugUtils)
		Warn("XR_EXT_debug_utils absent (optional; BetterVR just logs a notice)");

	Step("xrCreateInstance");
	std::vector<const char*> enabled = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
										XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
										XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	ici.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
	ici.enabledExtensionNames = enabled.data();
	ici.applicationInfo = {"BetterVR", 1, "Cemu", 1, XR_API_VERSION_1_0};
	Check(xrCreateInstance(&ici, &s_instance), "xrCreateInstance");

	PFN_xrGetD3D12GraphicsRequirementsKHR pfnReq = nullptr;
	Check(xrGetInstanceProcAddr(s_instance, "xrGetD3D12GraphicsRequirementsKHR",
								reinterpret_cast<PFN_xrVoidFunction*>(&pfnReq)),
		  "get xrGetD3D12GraphicsRequirementsKHR");
	PFN_xrConvertWin32PerformanceCounterToTimeKHR pfnPcToTime = nullptr;
	PFN_xrConvertTimeToWin32PerformanceCounterKHR pfnTimeToPc = nullptr;
	Check(xrGetInstanceProcAddr(s_instance, "xrConvertWin32PerformanceCounterToTimeKHR",
								reinterpret_cast<PFN_xrVoidFunction*>(&pfnPcToTime)),
		  "get xrConvertWin32PerformanceCounterToTimeKHR");
	Check(xrGetInstanceProcAddr(s_instance, "xrConvertTimeToWin32PerformanceCounterKHR",
								reinterpret_cast<PFN_xrVoidFunction*>(&pfnTimeToPc)),
		  "get xrConvertTimeToWin32PerformanceCounterKHR");

	Step("system + properties");
	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	Check(xrGetSystem(s_instance, &sgi, &systemId), "xrGetSystem");

	XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
	Check(xrGetSystemProperties(s_instance, systemId, &sysProps), "xrGetSystemProperties");
	XrInstanceProperties instProps = {XR_TYPE_INSTANCE_PROPERTIES};
	Check(xrGetInstanceProperties(s_instance, &instProps), "xrGetInstanceProperties");
	printf("       systemName = \"%s\"\n", sysProps.systemName);
	printf("       runtimeName = \"%s\"  version %u.%u.%u\n", instProps.runtimeName,
		   static_cast<unsigned>(XR_VERSION_MAJOR(instProps.runtimeVersion)),
		   static_cast<unsigned>(XR_VERSION_MINOR(instProps.runtimeVersion)),
		   static_cast<unsigned>(XR_VERSION_PATCH(instProps.runtimeVersion)));
	printf("       orientationTracking=%d positionTracking=%d\n", sysProps.trackingProperties.orientationTracking,
		   sysProps.trackingProperties.positionTracking);
	if (strstr(instProps.runtimeName, "Meta XR Simulator"))
		Ok("runtimeName matches BetterVR's isMetaSimulator sniff");
	else
		Warn("runtimeName does not contain \"Meta XR Simulator\" -> BetterVR's isMetaSimulator branch stays off");

	XrViewConfigurationProperties vcProps = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
	Check(xrGetViewConfigurationProperties(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &vcProps),
		  "xrGetViewConfigurationProperties(PRIMARY_STEREO)");
	printf("       fovMutable = %d\n", vcProps.fovMutable);

	Step("xrEnumerateViewConfigurationViews");
	std::uint32_t viewCount = 0;
	Check(xrEnumerateViewConfigurationViews(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
											&viewCount, nullptr),
		  "view config view count");
	if (viewCount != 2)
	{
		printf("  [FAIL] expected 2 views, got %u - BetterVR asserts on this\n", viewCount);
		return 1;
	}
	Ok("view count == 2");
	std::array<XrViewConfigurationView, 2> vcv = {XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
												  XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	Check(xrEnumerateViewConfigurationViews(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
											&viewCount, vcv.data()),
		  "view config views");
	printf("       recommended %ux%u  max %ux%u  samples %u\n", vcv[0].recommendedImageRectWidth,
		   vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight,
		   vcv[0].recommendedSwapchainSampleCount);
	if (vcv[0].recommendedImageRectWidth == 0 || vcv[0].recommendedImageRectHeight == 0)
	{
		printf("  [FAIL] zero recommended resolution\n");
		s_fail++;
	}

	Step("startup query contracts");
	auto expect = [](XrResult actual, XrResult expected, const char* label)
	{
		if (actual == expected)
			Ok(label);
		else
			Fail(label, actual);
	};
	vcv[0].recommendedImageRectWidth = 123;
	expect(xrEnumerateViewConfigurationViews(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1,
											 &viewCount, vcv.data()),
		   XR_ERROR_SIZE_INSUFFICIENT, "short view buffer rejected");
	if (viewCount != 2 || vcv[0].recommendedImageRectWidth != 123)
		Fail("short view buffer count and output preservation", XR_ERROR_RUNTIME_FAILURE);
	expect(xrEnumerateViewConfigurationViews(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO, 0,
											 &viewCount, nullptr),
		   XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "unsupported view configuration");
	expect(xrEnumerateEnvironmentBlendModes(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO, 0,
											&viewCount, nullptr),
		   XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "unsupported blend configuration");
	vcv[1].type = XR_TYPE_UNKNOWN;
	expect(xrEnumerateViewConfigurationViews(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
											 &viewCount, vcv.data()),
		   XR_ERROR_VALIDATION_FAILURE, "invalid view output header");
	if (vcv[0].recommendedImageRectWidth != 123)
		Fail("invalid view array partially written", XR_ERROR_RUNTIME_FAILURE);
	vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	XrBaseOutStructure unknown{XR_TYPE_UNKNOWN, nullptr};
	vcv[0].next = &unknown;
	Check(xrEnumerateViewConfigurationViews(s_instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
											&viewCount, vcv.data()),
		  "view enumeration with unknown chained output");
	if (vcv[0].next != &unknown)
		Fail("caller view chain replaced", XR_ERROR_RUNTIME_FAILURE);
	vcv[0].next = nullptr;
	if (hasTimeConv)
	{
		PFN_xrConvertWin32PerformanceCounterToTimeKHR toTime = nullptr;
		PFN_xrConvertTimeToWin32PerformanceCounterKHR toCounter = nullptr;
		Check(xrGetInstanceProcAddr(s_instance, "xrConvertWin32PerformanceCounterToTimeKHR",
									reinterpret_cast<PFN_xrVoidFunction*>(&toTime)),
			  "counter conversion procedure");
		Check(xrGetInstanceProcAddr(s_instance, "xrConvertTimeToWin32PerformanceCounterKHR",
									reinterpret_cast<PFN_xrVoidFunction*>(&toCounter)),
			  "time conversion procedure");
		LARGE_INTEGER counter{}, frequency{};
		QueryPerformanceCounter(&counter);
		QueryPerformanceFrequency(&frequency);
		XrTime time = 123;
		Check(toTime(s_instance, &counter, &time), "current counter conversion");
		Check(toCounter(s_instance, time, &counter), "current time conversion");
		counter.QuadPart = -1;
		time = 123;
		expect(toTime(s_instance, &counter, &time), XR_ERROR_TIME_INVALID, "negative counter rejected");
		if (time != 123)
			Fail("invalid counter modified output", XR_ERROR_RUNTIME_FAILURE);
		counter.QuadPart = 123;
		expect(toCounter(s_instance, -1, &counter), XR_ERROR_TIME_INVALID, "negative time rejected");
		if (counter.QuadPart != 123)
			Fail("invalid time modified output", XR_ERROR_RUNTIME_FAILURE);
		if (frequency.QuadPart < 1000000000LL)
		{
			counter.QuadPart = INT64_MAX;
			expect(toTime(s_instance, &counter, &time), XR_ERROR_TIME_INVALID, "counter overflow rejected");
		}
	}
	if (queriesOnly)
	{
		Check(xrDestroyInstance(s_instance), "query instance teardown");
		return s_fail ? 1 : 0;
	}

	XrGraphicsRequirementsD3D12KHR gfxReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
	Check(pfnReq(s_instance, systemId, &gfxReq), "xrGetD3D12GraphicsRequirementsKHR");
	printf("       adapterLuid = %08lX:%08lX  minFeatureLevel = 0x%X\n",
		   static_cast<unsigned long>(gfxReq.adapterLuid.HighPart),
		   static_cast<unsigned long>(gfxReq.adapterLuid.LowPart), static_cast<unsigned>(gfxReq.minFeatureLevel));

	Step("D3D12 device on the runtime's adapter");
	// The debug layer is the whole point of the render pass below: a barrier with
	// the wrong StateBefore is undefined behaviour that silently works on plenty of
	// drivers, and only becomes a hard error once validation is on.
	ComPtr<ID3D12Debug> d3dDebug;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3dDebug))))
	{
		d3dDebug->EnableDebugLayer();
		Ok("D3D12 debug layer enabled");
	}
	else
		Warn("D3D12 debug layer unavailable - install the Graphics Tools feature to catch resource-state bugs");
	ComPtr<IDXGIFactory4> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
	{
		printf("  [FAIL] CreateDXGIFactory1\n");
		return 1;
	}
	ComPtr<IDXGIAdapter1> adapter;
	if (gfxReq.adapterLuid.LowPart || gfxReq.adapterLuid.HighPart)
	{
		ComPtr<IDXGIAdapter1> a;
		for (UINT i = 0;; ++i)
		{
			const HRESULT result = factory->EnumAdapters1(i, &a);
			if (result == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(result))
			{
				std::puts("  [FAIL] enumerate DXGI adapters");
				return 1;
			}
			DXGI_ADAPTER_DESC1 d{};
			if (FAILED(a->GetDesc1(&d)))
			{
				std::puts("  [FAIL] read DXGI adapter description");
				return 1;
			}
			if (d.AdapterLuid.LowPart == gfxReq.adapterLuid.LowPart &&
				d.AdapterLuid.HighPart == gfxReq.adapterLuid.HighPart)
			{
				adapter = a;
				break;
			}
			a.Reset();
		}
		if (!adapter)
			Warn("runtime's adapterLuid matched no adapter; falling back to default");
	}
	ComPtr<ID3D12Device> device;
	D3D_FEATURE_LEVEL fl = gfxReq.minFeatureLevel ? gfxReq.minFeatureLevel : D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D12CreateDevice(adapter.Get(), fl, IID_PPV_ARGS(&device))))
	{
		printf("  [FAIL] D3D12CreateDevice\n");
		return 1;
	}
	Ok("D3D12CreateDevice at the runtime's minFeatureLevel");
	ComPtr<ID3D12CommandQueue> queue;
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
	{
		printf("  [FAIL] CreateCommandQueue\n");
		return 1;
	}
	Ok("CreateCommandQueue");

	ComPtr<ID3D12InfoQueue> infoQueue;
	device->QueryInterface(IID_PPV_ARGS(&infoQueue));

	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	ComPtr<ID3D12Fence> fence;
	UINT64 fenceValue = 0;
	std::unique_ptr<void, decltype(&CloseHandle)> fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr),
															 &CloseHandle);
	if (!fenceEvent)
	{
		std::puts("  [FAIL] create fence event");
		return 1;
	}
	ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc))) ||
		FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr,
										 IID_PPV_ARGS(&cmdList))) ||
		FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
	{
		printf("  [FAIL] could not create command list / fence\n");
		return 1;
	}
	if (FAILED(cmdList->Close()))
	{
		std::puts("  [FAIL] close initial command list");
		return 1;
	}
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.NumDescriptors = 1;
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap))))
		{
			printf("  [FAIL] RTV heap\n");
			return 1;
		}
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap))))
		{
			printf("  [FAIL] DSV heap\n");
			return 1;
		}
	}
	Ok("command list, fence and RTV/DSV heaps");

	try
	{
		Step("xrCreateSession (D3D12 binding)");
		XrGraphicsBindingD3D12KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
		binding.device = device.Get();
		binding.queue = queue.Get();
		XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
		sci.systemId = systemId;
		sci.next = &binding;
		XrSession session = XR_NULL_HANDLE;
		Check(xrCreateSession(s_instance, &sci, &session), "xrCreateSession");

		const XrPosef identity = {{0, 0, 0, 1}, {0, 0, 0}};
		XrSpace stageSpace = XR_NULL_HANDLE, headSpace = XR_NULL_HANDLE;
		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		rsci.poseInReferenceSpace = identity;
		Check(xrCreateReferenceSpace(session, &rsci, &stageSpace), "xrCreateReferenceSpace(STAGE)");
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		Check(xrCreateReferenceSpace(session, &rsci, &headSpace), "xrCreateReferenceSpace(VIEW)");

		Step("actions (BetterVR's shape: subaction paths, pose/bool/float/vec2/vibration)");
		std::array<XrPath, 2> handPaths = {Path("/user/hand/left"), Path("/user/hand/right")};
		if (handPaths[0] == XR_NULL_PATH || handPaths[1] == XR_NULL_PATH)
		{
			printf("  [FAIL] xrStringToPath returned XR_NULL_PATH\n");
			throw std::runtime_error("Probe check failed");
		}
		else
			Ok("xrStringToPath for hand paths");

		XrActionSet gameplaySet = XR_NULL_HANDLE, menuSet = XR_NULL_HANDLE;
		XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
		strcpy_s(asci.actionSetName, "gameplay_fps");
		strcpy_s(asci.localizedActionSetName, "Gameplay");
		Check(xrCreateActionSet(s_instance, &asci, &gameplaySet), "xrCreateActionSet(gameplay_fps)");
		strcpy_s(asci.actionSetName, "menu");
		strcpy_s(asci.localizedActionSetName, "Menu Navigation");
		Check(xrCreateActionSet(s_instance, &asci, &menuSet), "xrCreateActionSet(menu)");

		auto mkAction = [&](XrActionSet set, const char* id, const char* name, XrActionType type, XrAction& out)
		{
			XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
			aci.actionType = type;
			strncpy_s(aci.actionName, id, XR_MAX_ACTION_NAME_SIZE - 1);
			strncpy_s(aci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
			aci.countSubactionPaths = static_cast<std::uint32_t>(handPaths.size());
			aci.subactionPaths = handPaths.data();
			XrResult r = xrCreateAction(set, &aci, &out);
			if (XR_FAILED(r))
			{
				char buf[128];
				sprintf_s(buf, "xrCreateAction(%s)", id);
				Fail(buf, r);
			}
		};
		XrAction gripPose{}, aimPose{}, move{}, camera{}, jump{}, grab{}, rumble{}, menuSelect{}, simpleOnly{};
		mkAction(gameplaySet, "pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, gripPose);
		mkAction(gameplaySet, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, aimPose);
		mkAction(gameplaySet, "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, move);
		mkAction(gameplaySet, "camera", "Camera Rotation", XR_ACTION_TYPE_VECTOR2F_INPUT, camera);
		mkAction(gameplaySet, "jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT, jump);
		mkAction(gameplaySet, "grab_interact", "Interact", XR_ACTION_TYPE_FLOAT_INPUT, grab);
		mkAction(gameplaySet, "rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, rumble);
		mkAction(menuSet, "select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT, menuSelect);
		mkAction(gameplaySet, "simple_only", "Simple profile only", XR_ACTION_TYPE_BOOLEAN_INPUT, simpleOnly);
		if (!s_fail)
			Ok("all action types accepted (incl. VIBRATION_OUTPUT)");

		Step("xrSuggestInteractionProfileBindings");
		{
			std::vector<XrActionSuggestedBinding> b = {
				{gripPose, Path("/user/hand/left/input/grip/pose")},
				{gripPose, Path("/user/hand/right/input/grip/pose")},
				{aimPose, Path("/user/hand/left/input/aim/pose")},
				{aimPose, Path("/user/hand/right/input/aim/pose")},
				{move, Path("/user/hand/left/input/thumbstick")},
				{camera, Path("/user/hand/right/input/thumbstick")},
				{jump, Path("/user/hand/right/input/a/click")},
				{grab, Path("/user/hand/left/input/squeeze/value")},
				{rumble, Path("/user/hand/left/output/haptic")},
				{menuSelect, Path("/user/hand/right/input/a/click")},
			};
			XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
			sb.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
			sb.countSuggestedBindings = static_cast<std::uint32_t>(b.size());
			sb.suggestedBindings = b.data();
			Check(xrSuggestInteractionProfileBindings(s_instance, &sb), "suggest bindings (oculus/touch_controller)");
			sb.interactionProfile = Path("/interaction_profiles/khr/simple_controller");
			std::vector<XrActionSuggestedBinding> simple = {
				{gripPose, Path("/user/hand/left/input/grip/pose")},
				{gripPose, Path("/user/hand/right/input/grip/pose")},
				{menuSelect, Path("/user/hand/right/input/select/click")},
				{simpleOnly, Path("/user/hand/right/input/select/click")},
			};
			sb.countSuggestedBindings = static_cast<std::uint32_t>(simple.size());
			sb.suggestedBindings = simple.data();
			Check(xrSuggestInteractionProfileBindings(s_instance, &sb), "suggest bindings (khr/simple_controller)");
		}

		Step("xrAttachSessionActionSets + action spaces");
		{
			std::array<XrActionSet, 2> sets = {gameplaySet, menuSet};
			XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
			ai.countActionSets = static_cast<std::uint32_t>(sets.size());
			ai.actionSets = sets.data();
			Check(xrAttachSessionActionSets(session, &ai), "xrAttachSessionActionSets");
		}
		XrSpace handSpaces[2] = {};
		for (int side = 0; side < 2; ++side)
		{
			XrActionSpaceCreateInfo aspci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			aspci.action = gripPose;
			aspci.subactionPath = handPaths[side];
			aspci.poseInActionSpace = identity;
			Check(xrCreateActionSpace(session, &aspci, &handSpaces[side]),
				  side == 0 ? "xrCreateActionSpace(left grip)" : "xrCreateActionSpace(right grip)");
		}
		{
			XrInteractionProfileState ips = {XR_TYPE_INTERACTION_PROFILE_STATE};
			Check(xrGetCurrentInteractionProfile(session, handPaths[0], &ips), "xrGetCurrentInteractionProfile(left)");
			if (ips.interactionProfile != XR_NULL_PATH)
			{
				char buf[XR_MAX_PATH_LENGTH];
				std::uint32_t n = 0;
				if (XR_SUCCEEDED(xrPathToString(s_instance, ips.interactionProfile, sizeof(buf), &n, buf)))
					printf("       active profile: %s\n", buf);
			}
			else
			{
				Warn("no active interaction profile (BetterVR's DetectActiveControllerType falls back)");
			}
		}

		Step("swapchains (exactly BetterVR's two formats)");
		std::uint32_t fmtCount = 0;
		Check(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr), "swapchain format count");
		std::vector<std::int64_t> formats(fmtCount);
		Check(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, formats.data()), "swapchain formats");
		bool haveColor = false, haveDepth = false;
		for (std::int64_t f : formats)
		{
			if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
				haveColor = true;
			if (f == DXGI_FORMAT_D32_FLOAT)
				haveDepth = true;
		}
		if (!haveColor)
		{
			printf("  [FAIL] DXGI_FORMAT_R8G8B8A8_UNORM_SRGB not offered - Swapchain<> ctor throws\n");
			throw std::runtime_error("Probe check failed");
		}
		else
			Ok("R8G8B8A8_UNORM_SRGB offered");
		if (!haveDepth)
		{
			printf("  [FAIL] DXGI_FORMAT_D32_FLOAT not offered - depth Swapchain<> ctor throws\n");
			throw std::runtime_error("Probe check failed");
		}
		else
			Ok("D32_FLOAT offered");

		// SIMXR_PROBE_SUBRECT replays the shape VR mods use: render 1920x1080 into a bigger
		// square swapchain and declare the used region with subImage.imageRect. Off by
		// default so the plain BetterVR replay is unchanged.
		const bool subRect = EnvOn("SIMXR_PROBE_SUBRECT");

		// Held steady for a whole run rather than cycled per frame, so an observer taking a
		// screenshot knows which blend mode it shows.
		XrCompositionLayerFlags quadFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		const char* quadBlendName = "premultiplied";
		if (const char* env = getenv("SIMXR_PROBE_QUAD_BLEND"))
		{
			if (_stricmp(env, "opaque") == 0)
			{
				quadFlags = 0;
				quadBlendName = "opaque (source alpha ignored)";
			}
			else if (_stricmp(env, "straight") == 0 || _stricmp(env, "unpremultiplied") == 0)
			{
				quadFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
							XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
				quadBlendName = "straight (unpremultiplied)";
			}
		}
		printf("       quad blend mode: %s (layerFlags=0x%llX)\n", quadBlendName,
			   static_cast<unsigned long long>(quadFlags));
		const std::uint32_t width = subRect ? 2048u : 1440u; // a game-render-sized request, not the recommended size
		const std::uint32_t height = subRect ? 2048u : 1584u;
		const XrRect2Di viewRect =
			subRect ? XrRect2Di{{64, 128}, {1920, 1080}}
					: XrRect2Di{{0, 0}, {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)}};
		if (subRect)
			printf("       sub-rect mode: %ux%u swapchain, imageRect (%d,%d) %dx%d\n", width, height, viewRect.offset.x,
				   viewRect.offset.y, viewRect.extent.width, viewRect.extent.height);
		struct Chain
		{
			XrSwapchain m_handle = XR_NULL_HANDLE;
			std::vector<ComPtr<ID3D12Resource>> m_tex;
		};
		auto makeChain = [&](DXGI_FORMAT fmt, bool depth, Chain& c, const char* label) -> bool
		{
			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.width = width;
			ci.height = height;
			ci.arraySize = 1;
			ci.sampleCount = 1;
			ci.format = fmt;
			ci.mipCount = 1;
			ci.faceCount = 1;
			ci.usageFlags =
				(depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) |
				XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			XrResult r = xrCreateSwapchain(session, &ci, &c.m_handle);
			if (XR_FAILED(r))
			{
				Fail(label, r);
			}
			std::uint32_t n = 0;
			if (XR_FAILED(xrEnumerateSwapchainImages(c.m_handle, 0, &n, nullptr)) || n == 0)
			{
				printf("  [FAIL] %s: no swapchain images\n", label);
				throw std::runtime_error("Probe check failed");
			}
			std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
			r = xrEnumerateSwapchainImages(c.m_handle, n, &n,
										   reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
			if (XR_FAILED(r))
			{
				Fail(label, r);
			}
			for (auto& im : imgs)
			{
				if (!im.texture)
				{
					printf("  [FAIL] %s: null ID3D12Resource in swapchain image\n", label);
					throw std::runtime_error("Probe check failed");
				}
				D3D12_RESOURCE_DESC d = im.texture->GetDesc();
				if (d.Width != width || d.Height != height)
				{
					printf("  [FAIL] %s: image is %llux%u, asked for %ux%u\n", label,
						   static_cast<unsigned long long>(d.Width), d.Height, width, height);
					throw std::runtime_error("Probe check failed");
				}
				c.m_tex.push_back(im.texture);
			}
			// The texture must belong to OUR device or every later copy is UB.
			ComPtr<ID3D12Device> owner;
			if (FAILED(c.m_tex[0]->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device.Get())
			{
				printf("  [FAIL] %s: swapchain texture belongs to a different ID3D12Device\n", label);
				throw std::runtime_error("Probe check failed");
			}
			printf("       %s: %u images at %ux%u on the app's device\n", label, n, width, height);
			return true;
		};
		Chain colorL, colorR, depthL, depthR, quadChain;
		bool chainsOk = true;
		chainsOk &= makeChain(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, colorL, "color swapchain (left)");
		chainsOk &= makeChain(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, colorR, "color swapchain (right)");
		chainsOk &= makeChain(DXGI_FORMAT_D32_FLOAT, true, depthL, "depth swapchain (left)");
		chainsOk &= makeChain(DXGI_FORMAT_D32_FLOAT, true, depthR, "depth swapchain (right)");
		chainsOk &= makeChain(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, quadChain, "quad-layer swapchain (2D HUD)");
		if (!chainsOk)
			throw std::runtime_error("Probe check failed");
		Ok("all five swapchains created");

		Step("session state machine (xrPollEvent -> READY -> xrBeginSession)");
		XrSessionState state = BeginProbeSession(s_instance, session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

		const bool projectionOnly = EnvOn("SIMXR_PROBE_PROJECTION_ONLY");

		char stepLabel[128];
		snprintf(stepLabel, sizeof(stepLabel),
				 "frame loop x %d (waitFrame/beginFrame/locateViews/acquire/release/endFrame + syncActions)",
				 totalFrames);
		Step(stepLabel);
		int framesRendered = 0, focusedFrames = 0, quadOnlyFrames = 0;
		const bool testBindings = EnvOn("SIMXR_PROBE_TEST_BINDINGS");
		const auto controls = testBindings ? HoldControllerInputs() : nullptr;
		bool sawBindingInputs = false;
		bool sawSaneFov = true;
		for (int f = 0; f < totalFrames; ++f)
		{
			PollXrEvents(s_instance, [&](const XrEventDataBuffer& event)
			{
				if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
					state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
			});
			if (state == XR_SESSION_STATE_FOCUSED)
				focusedFrames++;

			XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
			XrFrameState fs = {XR_TYPE_FRAME_STATE};
			XrResult r = xrWaitFrame(session, &fwi, &fs);
			if (XR_FAILED(r))
			{
				Fail("xrWaitFrame", r);
			}
			if (f == 0)
				printf("       predictedDisplayTime=%lld period=%lld shouldRender=%d\n",
					   static_cast<long long>(fs.predictedDisplayTime),
					   static_cast<long long>(fs.predictedDisplayPeriod), fs.shouldRender);
			if (fs.predictedDisplayTime <= 0)
			{
				printf("  [FAIL] predictedDisplayTime is not positive\n");
				throw std::runtime_error("Probe check failed");
			}

			XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
			r = xrBeginFrame(session, &fbi);
			if (XR_FAILED(r))
			{
				Fail("xrBeginFrame", r);
			}

			// BetterVR syncs actions every frame while the session is up.
			XrActiveActionSet aas = {gameplaySet, XR_NULL_PATH};
			XrActionsSyncInfo asi = {XR_TYPE_ACTIONS_SYNC_INFO};
			asi.countActiveActionSets = 1;
			asi.activeActionSets = &aas;
			r = xrSyncActions(session, &asi);
			if (XR_FAILED(r) && r != XR_SESSION_NOT_FOCUSED)
			{
				Fail("xrSyncActions", r);
			}

			XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
			gi.action = gripPose;
			gi.subactionPath = handPaths[0];
			XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
			if (XR_FAILED(xrGetActionStatePose(session, &gi, &ps)))
			{
				printf("  [FAIL] xrGetActionStatePose\n");
				throw std::runtime_error("Probe check failed");
			}
			gi.action = jump;
			gi.subactionPath = handPaths[1];
			XrActionStateBoolean bs = {XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_FAILED(xrGetActionStateBoolean(session, &gi, &bs)))
			{
				printf("  [FAIL] xrGetActionStateBoolean\n");
				throw std::runtime_error("Probe check failed");
			}
			gi.action = move;
			gi.subactionPath = handPaths[0];
			XrActionStateVector2f v2 = {XR_TYPE_ACTION_STATE_VECTOR2F};
			if (XR_FAILED(xrGetActionStateVector2f(session, &gi, &v2)))
			{
				printf("  [FAIL] xrGetActionStateVector2f\n");
				throw std::runtime_error("Probe check failed");
			}
			gi.action = grab;
			XrActionStateFloat fv = {XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetActionStateFloat(session, &gi, &fv)))
			{
				printf("  [FAIL] xrGetActionStateFloat\n");
				throw std::runtime_error("Probe check failed");
			}
			if (testBindings && f >= 2 && bs.isActive && bs.currentState && v2.isActive &&
				fabsf(v2.currentState.x + 0.5f) < 0.01f && fabsf(v2.currentState.y - 0.25f) < 0.01f && fv.isActive &&
				fabsf(fv.currentState - 0.7f) < 0.01f)
			{
				gi.action = simpleOnly;
				gi.subactionPath = handPaths[1];
				XrActionStateBoolean unsupported = {XR_TYPE_ACTION_STATE_BOOLEAN};
				if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &unsupported)) && !unsupported.isActive)
					sawBindingInputs = true;
			}

			// hand pose location (BetterVR chains XrSpaceVelocity onto this)
			if (handSpaces[0])
			{
				XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
				XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION, &vel};
				if (XR_FAILED(xrLocateSpace(handSpaces[0], stageSpace, fs.predictedDisplayTime, &loc)))
				{
					printf("  [FAIL] xrLocateSpace(hand->stage)\n");
					throw std::runtime_error("Probe check failed");
				}
			}
			XrSpaceLocation headLoc = {XR_TYPE_SPACE_LOCATION};
			if (XR_FAILED(xrLocateSpace(headSpace, stageSpace, fs.predictedDisplayTime, &headLoc)))
			{
				printf("  [FAIL] xrLocateSpace(view->stage)\n");
				throw std::runtime_error("Probe check failed");
			}

			XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = fs.predictedDisplayTime;
			vli.space = stageSpace;
			XrViewState vs = {XR_TYPE_VIEW_STATE};
			std::uint32_t n = 2;
			std::array<XrView, 2> views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
			r = xrLocateViews(session, &vli, &vs, n, &n, views.data());
			if (XR_FAILED(r))
			{
				Fail("xrLocateViews", r);
			}
			if (f == 0)
			{
				printf("       viewStateFlags=0x%llX  n=%u\n", static_cast<unsigned long long>(vs.viewStateFlags), n);
				printf("       L pos=(%.3f,%.3f,%.3f) fov=(%.3f,%.3f,%.3f,%.3f)\n", views[0].pose.position.x,
					   views[0].pose.position.y, views[0].pose.position.z, views[0].fov.angleLeft,
					   views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown);
				printf("       R pos=(%.3f,%.3f,%.3f) fov=(%.3f,%.3f,%.3f,%.3f)\n", views[1].pose.position.x,
					   views[1].pose.position.y, views[1].pose.position.z, views[1].fov.angleLeft,
					   views[1].fov.angleRight, views[1].fov.angleUp, views[1].fov.angleDown);
				if (views[0].pose.position.x == views[1].pose.position.x)
					Warn("both eyes share an X position - no stereo IPD separation");
			}
			if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
				!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
			{
				if (f == 0)
				{
					printf("  [FAIL] viewState missing ORIENTATION_VALID/POSITION_VALID - BetterVR treats views as "
						   "unusable\n");
					throw std::runtime_error("Probe check failed");
				}
			}
			if (views[0].fov.angleLeft >= views[0].fov.angleRight || views[0].fov.angleDown >= views[0].fov.angleUp)
				sawSaneFov = false;

			// Acquire, actually draw into the image, then release WITHOUT any barrier -
			// exactly what Layer3D::RecordRender does. It never transitions the
			// swapchain because XR_KHR_D3D12_enable hands colour images over in
			// RENDER_TARGET and depth in DEPTH_WRITE and wants them back that way
			// (renderer.cpp:492 and :514). A runtime that tracks them as anything else
			// issues a mismatched barrier, which is what validation catches here.
			auto cycle = [&](Chain& c, bool depth, int eye = -1) -> bool
			{
				std::uint32_t idx = 0;
				if (XR_FAILED(xrAcquireSwapchainImage(c.m_handle, nullptr, &idx)))
					return false;
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = XR_INFINITE_DURATION;
				if (XR_FAILED(xrWaitSwapchainImage(c.m_handle, &wi)))
					return false;
				if (idx >= c.m_tex.size())
					return false;

				if (FAILED(cmdAlloc->Reset()) || FAILED(cmdList->Reset(cmdAlloc.Get(), nullptr)))
					return false;
				if (depth)
				{
					D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
					dv.Format = DXGI_FORMAT_D32_FLOAT;
					dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
					D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
					device->CreateDepthStencilView(c.m_tex[idx].Get(), &dv, h);
					cmdList->ClearDepthStencilView(h, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				}
				else
				{
					D3D12_RENDER_TARGET_VIEW_DESC rv = {};
					rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
					rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
					D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
					device->CreateRenderTargetView(c.m_tex[idx].Get(), &rv, h);

					// Everything outside the declared rect gets a colour the preview must
					// never show. Green on screen means imageRect was ignored somewhere.
					if (subRect)
					{
						const float outside[4] = {0.0f, 0.9f, 0.1f, 1.0f};
						cmdList->ClearRenderTargetView(h, outside, 0, nullptr);
					}
					const D3D12_RECT rect = {viewRect.offset.x, viewRect.offset.y,
											 viewRect.offset.x + viewRect.extent.width,
											 viewRect.offset.y + viewRect.extent.height};
					if (eye >= 0)
					{
						const float clear[4] = {0.1f, 0.2f, 0.4f, 1.0f};
						cmdList->ClearRenderTargetView(h, clear, 1, &rect);

						// A flat clear is indistinguishable between the eyes, so a stereo
						// checker can only ever report "no parallax" on it. Mark each eye
						// with the same square at a 20px horizontal offset: real, known
						// disparity, inside the center window a disparity search looks at.
						const float mark[4] = {0.95f, 0.9f, 0.25f, 1.0f};
						const LONG cx = rect.left + viewRect.extent.width / 2 + (eye == 0 ? 10 : -10);
						const LONG cy = rect.top + viewRect.extent.height / 2;
						D3D12_RECT square = {cx - 100, cy - 100, cx + 100, cy + 100};
						cmdList->ClearRenderTargetView(h, mark, 1, &square);
					}
					else
					{
						// The quad (2D) layer, in a colour nothing else uses: it is the only
						// way to see, in the preview window, whether the overlay survives the
						// eye blit that lands in the same frame.
						const float hud[4] = {0.8f, 0.1f, 0.5f, 1.0f};
						cmdList->ClearRenderTargetView(h, hud, 1, &rect);

						// A flat opaque fill looks identical under all three blend modes, so it
						// cannot tell a runtime that honours layerFlags from one that ignores
						// them. These four bands can.
						const LONG bandW = viewRect.extent.width / 5;
						const LONG bandTop = rect.top + viewRect.extent.height / 4;
						const LONG bandBot = rect.top + viewRect.extent.height / 2;
						const float bands[4][4] = {
							{0.0f, 0.0f, 0.0f, 0.0f}, // invisible unless source alpha is ignored
							{1.0f, 1.0f, 1.0f, 0.0f}, // invisible only under straight alpha; white
													  // under premultiplied and under opaque
							{1.0f, 1.0f, 1.0f, 0.5f}, // 188 if the compositor blends in linear
													  // light, 128 if it blends the sRGB bytes
							{1.0f, 1.0f, 1.0f, 1.0f}, // control: unchanged by any mode
						};
						for (int b = 0; b < 4; ++b)
						{
							D3D12_RECT band = {rect.left + bandW * b, bandTop, rect.left + bandW * (b + 1), bandBot};
							cmdList->ClearRenderTargetView(h, bands[b], 1, &band);
						}
					}
				}
				if (FAILED(cmdList->Close()))
					return false;
				ID3D12CommandList* lists[] = {cmdList.Get()};
				queue->ExecuteCommandLists(1, lists);
				if (FAILED(queue->Signal(fence.Get(), ++fenceValue)))
					UnknownCompletion("Signal D3D12 clear");
				if (fence->GetCompletedValue() < fenceValue)
				{
					if (FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent.get())) ||
						WaitForSingleObject(fenceEvent.get(), 5000) != WAIT_OBJECT_0)
						UnknownCompletion("Wait for D3D12 clear");
				}
				const auto completed = fence->GetCompletedValue();
				if (completed < fenceValue || completed == UINT64_MAX)
					UnknownCompletion("D3D12 clear fence did not complete");

				XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				return XR_SUCCEEDED(xrReleaseSwapchainImage(c.m_handle, &ri));
			};
			// BetterVR gates its projection layer on IsInGame() (RND_Renderer::EndFrame),
			// so its whole boot and title sequence is quad-only frames. Replay both shapes.
			// Alternate in blocks rather than splitting the run in half, so a long run does not
			// make an observer wait out thousands of quad-only frames to see the mixed case.
			const int phaseLen = (totalFrames > 200) ? 100 : (totalFrames / 2);
			const bool quadOnly = !projectionOnly && ((phaseLen <= 0) || ((f / phaseLen) % 2) == 0);
			if (quadOnly)
				quadOnlyFrames++;

			bool cycled = cycle(quadChain, false);
			if (!quadOnly)
			{
				cycled = cycled && cycle(colorL, false, 0) && cycle(colorR, false, 1) && cycle(depthL, true) &&
						 cycle(depthR, true);
			}
			if (!cycled)
			{
				printf("  [FAIL] swapchain acquire/render/release cycle failed on frame %d\n", f);
				throw std::runtime_error("Probe check failed");
			}

			if (FAILED(device->GetDeviceRemovedReason()))
			{
				printf("  [FAIL] D3D12 device removed on frame %d (reason 0x%08X)\n", f,
					   static_cast<unsigned>(device->GetDeviceRemovedReason()));
				throw std::runtime_error("Probe check failed");
			}

			XrCompositionLayerDepthInfoKHR depthInfo[2] = {};
			XrCompositionLayerProjectionView projViews[2] = {};
			Chain* colorChains[2] = {&colorL, &colorR};
			Chain* depthChains[2] = {&depthL, &depthR};
			for (int e = 0; e < 2; ++e)
			{
				depthInfo[e] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
				depthInfo[e].subImage.swapchain = depthChains[e]->m_handle;
				depthInfo[e].subImage.imageRect = viewRect;
				depthInfo[e].subImage.imageArrayIndex = 0;
				depthInfo[e].minDepth = 0.0f;
				depthInfo[e].maxDepth = 1.0f;
				depthInfo[e].nearZ = 0.05f;
				depthInfo[e].farZ = INFINITY;

				projViews[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				projViews[e].next = &depthInfo[e];
				projViews[e].pose = views[e].pose;
				projViews[e].fov = views[e].fov;
				projViews[e].subImage.swapchain = colorChains[e]->m_handle;
				projViews[e].subImage.imageRect = viewRect;
				projViews[e].subImage.imageArrayIndex = 0;
			}
			XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
			proj.space = stageSpace;
			proj.viewCount = 2;
			proj.views = projViews;

			const float eyeY = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;

			XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
			quad.layerFlags = quadFlags;
			quad.space = stageSpace;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = quadChain.m_handle;
			quad.subImage.imageRect = viewRect;
			// World-locked at eye height, 2 m ahead, like a BetterVR 2D screen. A runtime
			// that reads a STAGE pose as head-relative throws this off the top of the view.
			quad.pose = identity;
			quad.pose.position.y = eyeY;
			quad.pose.position.z = -2.0f;
			quad.size = {1.6f, 0.9f};

			// A runtime that places quads by a screen-space bounding box draws this yawed quad as
			// an axis-aligned rectangle; one that projects the corners draws a trapezoid, and a
			// different one in each eye.
			XrCompositionLayerQuad angled = quad;
			angled.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			angled.pose.position = {-1.1f, eyeY, -1.6f};
			angled.pose.orientation = {0.0f, sinf(0.5f * 40.0f * 3.14159265f / 180.0f), 0.0f,
									   cosf(0.5f * 40.0f * 3.14159265f / 180.0f)};
			angled.size = {0.7f, 0.7f};

			// Must be absent from the left half of the preview entirely.
			XrCompositionLayerQuad rightOnly = quad;
			rightOnly.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
			rightOnly.pose.position = {1.1f, eyeY, -1.6f};
			rightOnly.size = {0.5f, 0.5f};

			// Overlaps `quad` and is submitted after it: proves layers composite in submission
			// order rather than in whatever order the runtime happens to walk them.
			XrCompositionLayerQuad overlap = quad;
			overlap.pose.position = {0.35f, eyeY - 0.25f, -1.9f};
			overlap.size = {0.5f, 0.5f};

			const XrCompositionLayerBaseHeader* layers[5];
			std::uint32_t layerCount = 0;
			if (!quadOnly)
				layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&angled);
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&rightOnly);
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&overlap);

			XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
			fei.displayTime = fs.predictedDisplayTime;
			fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			fei.layerCount = layerCount;
			fei.layers = const_cast<const XrCompositionLayerBaseHeader**>(layers);
			r = xrEndFrame(session, &fei);
			if (XR_FAILED(r))
			{
				Fail("xrEndFrame", r);
			}
			framesRendered++;
			if (sleepMs > 0)
				Sleep(static_cast<DWORD>(sleepMs));
		}
		printf("       rendered %d/%d frames (%d quad-only), %d of them with state==FOCUSED\n", framesRendered,
			   totalFrames, quadOnlyFrames, focusedFrames);
		if (framesRendered == totalFrames)
		{
			char label[160];
			snprintf(label, sizeof(label),
					 "%d clean frames: %d quad-only, then %d with a projection(+depth) layer and a quad layer",
					 totalFrames, quadOnlyFrames, totalFrames - quadOnlyFrames);
			Ok(label);
		}
		if (focusedFrames == 0)
			Warn("session never reached FOCUSED - BetterVR gates its input handling on that");
		if (!sawSaneFov)
		{
			printf("  [FAIL] nonsensical FOV angles (left>=right or down>=up)\n");
			throw std::runtime_error("Probe check failed");
		}
		if (testBindings)
		{
			if (sawBindingInputs)
				Ok("held controls resolve through active-profile bindings; another profile stays inactive");
			else
			{
				printf("  [FAIL] held controls or active-profile binding lookup\n");
				throw std::runtime_error("Probe check failed");
			}
		}
		Step("D3D12 validation messages");
		if (infoQueue)
		{
			const UINT64 n = infoQueue->GetNumStoredMessages();
			UINT64 errors = 0;
			for (UINT64 i = 0; i < n; ++i)
			{
				SIZE_T len = 0;
				if (FAILED(infoQueue->GetMessage(i, nullptr, &len)) || len == 0)
					continue;
				std::vector<char> raw(len);
				auto* m = reinterpret_cast<D3D12_MESSAGE*>(raw.data());
				if (FAILED(infoQueue->GetMessage(i, m, &len)))
					continue;
				if (m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || m->Severity == D3D12_MESSAGE_SEVERITY_ERROR)
				{
					if (errors < 5)
						printf("  [FAIL] D3D12 %s: %.*s\n",
							   m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : "ERROR",
							   static_cast<int>(
								   std::min<SIZE_T>(m->DescriptionByteLength, std::numeric_limits<int>::max())),
							   m->pDescription);
					errors++;
				}
			}
			if (errors)
			{
				printf("  [FAIL] %llu D3D12 validation error(s) during the frame loop\n",
					   static_cast<unsigned long long>(errors));
				throw std::runtime_error("Probe check failed");
			}
			else
				Ok("no D3D12 validation errors across the frame loop");
		}
		else
		{
			Warn("no ID3D12InfoQueue - resource-state bugs cannot be detected in this run");
		}

		Step("time conversion");
		LARGE_INTEGER pc;
		QueryPerformanceCounter(&pc);
		XrTime t = 0;
		Check(pfnPcToTime(s_instance, &pc, &t), "xrConvertWin32PerformanceCounterToTimeKHR");
		LARGE_INTEGER back = {};
		Check(pfnTimeToPc(s_instance, t, &back), "xrConvertTimeToWin32PerformanceCounterKHR");
		if (t <= 0)
		{
			printf("  [FAIL] converted XrTime is not positive\n");
			throw std::runtime_error("Probe check failed");
		}
		else
			printf("       XrTime=%lld  roundtrip QPC delta=%lld ticks\n", static_cast<long long>(t),
				   static_cast<long long>(back.QuadPart - pc.QuadPart));

		// RND_Renderer::~RND_Renderer calls these back to back without waiting for
		// STOPPING, and wraps xrEndSession in checkXRResult - which throws and pops a
		// MessageBox. A runtime that insists on the conformant STOPPING handshake
		// makes BetterVR's shutdown path fail, so probe it exactly as BetterVR does.
		Step("teardown (BetterVR's exact shutdown order)");
		Check(xrRequestExitSession(session), "xrRequestExitSession");
		Check(xrEndSession(session), "xrEndSession right after xrRequestExitSession");
		for (Chain* c : {&colorL, &colorR, &depthL, &depthR, &quadChain})
			if (c->m_handle)
				Check(xrDestroySwapchain(c->m_handle), "xrDestroySwapchain");
		for (XrSpace s : {handSpaces[0], handSpaces[1], headSpace, stageSpace})
			if (s)
				Check(xrDestroySpace(s), "xrDestroySpace");
		Check(xrDestroySession(session), "xrDestroySession");
		Check(xrDestroyInstance(s_instance), "xrDestroyInstance");

		printf("\n================ %d failure(s), %d warning(s) ================\n", s_fail, s_warn);
		if (s_fail)
			throw std::runtime_error("Probe check failed");
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

} // namespace simxr::probe

int main(int argc, char** argv)
{
	try
	{
		return simxr::probe::Run(argc, argv);
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
