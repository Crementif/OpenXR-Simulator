// Minimal OpenXR Simulator Runtime (D3D11/D3D12/OpenGL)
// - Implements enough of the runtime interface to let OpenXR apps start and render into runtime-owned swapchains
// - Opens a desktop window and presents the app's submitted images side-by-side
// - Supports D3D11, D3D12, and OpenGL graphics APIs

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN

#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi.h>
#include <dxgi1_6.h>

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "flicker_detector.h"
#include "presentation/FrameScheduler.h"
#include "graphics/Swapchain.h"
#include "runtime/EventQueue.h"
#include "runtime/DispatchTable.h"
#include "runtime/EntryPoints.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"
#include "runtime/Runtime.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"
#include "Math.h"
#include "runtime/validation/LayerValidation.h"
#include "graphics/LayerBuilder.h"
#include "runtime/validation/StructureChain.h"
#include "platform/windows/WindowsClock.h"
#include "platform/windows/D3D11Preview.h"
#include "graphics/PreviewGeometry.h"
#include "platform/windows/OpenGLPreviewReadback.h"
#include "platform/windows/OpenGLSwapchain.h"
#include "platform/windows/D3D11PreviewContext.h"
#include "platform/windows/D3D12Preview.h"
#include "platform/windows/D3D12PreviewPipelines.h"
#include "simulation/LegacyControllerMotion.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include <charconv>
#include <memory>

static simxr::Runtime& RuntimeState();

// OpenGL headers - minimal definitions for what we need
#include <GL/gl.h>

#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <array>
#include <span>
#include <string_view>
#include <chrono>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <loader_interfaces.h>
#include "mcp_integration.h"
#include "projection_timing.h"
#include "ui_enhancements.h"

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "opengl32.lib")

// D3D12 helper - calculates subresource index (from d3dx12.h)
inline UINT D3D12CalcSubresource(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) {
    return MipSlice + ArraySlice * MipLevels + PlaneSlice * MipLevels * ArraySize;
}


// File output is owned by the active instance's joined compatibility worker.
static std::mutex g_LogMutex;
static std::weak_ptr<mcp::WindowsLegacyChannel> g_logChannel;
static const bool g_logVerbose = []() {
    char v[16]{};
    const DWORD n = GetEnvironmentVariableA("SIMXR_VERBOSE", v, (DWORD)sizeof(v));
    return n > 0 && n < sizeof(v) && v[0] != '0';
}();

static void Log(const char* msg) {
    std::shared_ptr<mcp::WindowsLegacyChannel> files;
    { std::lock_guard lock(g_LogMutex); files = g_logChannel.lock(); }
    if (files) {
        try { files->Log(msg); } catch (...) {}
    } else {
        OutputDebugStringA(msg); // Loader/startup diagnostics before an instance exists.
    }
}
static void Logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log(buf);
}
// Bounded: this runs on the app's render thread, so a handler that reposts as fast as we
// drain would wedge the host rather than cost a frame.
static void PumpMessages(HWND filter) {
    MSG msg;
    int drained = 0;
    while (PeekMessageW(&msg, filter, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (++drained >= 2000) {
            Logf("[SimXR] PUMP RUNAWAY: bailed after %d messages, last=0x%04X hwnd=%p",
                 drained, msg.message, msg.hwnd);
            break;
        }
    }
}

static void LogVf(const char* fmt, ...) {
    if (!g_logVerbose) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log(buf);
}

// Helper function to convert a typed format to typeless
static DXGI_FORMAT ToTypeless(DXGI_FORMAT format) {
    switch (format) {
        // R8G8B8A8 family
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
            
        // B8G8R8A8 family
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
            
        // R16G16B16A16 family
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
            
        // R32G32B32A32 family
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return DXGI_FORMAT_R32G32B32A32_TYPELESS;
            
        // R10G10B10A2 family
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
            
        // Already typeless or depth formats - return as-is
        default:
            return format;
    }
}

// Released from WndProc and instance teardown, both of which run long before the D3D12
// preview path where it is defined.
namespace rt { struct Session; }
// Runtime state
namespace rt {

struct Swapchain;

// Forward declarations
void PushState(XrSession s, XrSessionState newState);

// Global adapter LUID that we'll use consistently
static LUID g_adapterLuid = {};
static bool g_adapterLuidSet = false;

// Global persistent window that survives session creation/destruction
// The preview window needs a thread we own. Only the creating thread can retrieve a
// window's messages, and hosts differ over which thread calls which entry point; putting
// it on the app's render thread instead wedged Cemu inside SetWindowText on the first
// composited frame.
//
// The frame path calls in synchronously (SetWindowText, SetWindowPos), so this thread must
// stay in its message loop: whatever blocks here stalls the application's rendering.
struct PreviewUi {
    std::thread       thread;
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> finished{false};   // creation attempt is over, successfully or not
    std::atomic<DWORD> threadId{0};
};
static PreviewUi g_previewUi;
static std::mutex g_coreMutex;
static std::mutex g_instanceLifecycleMutex;

static HWND g_persistentWindow = nullptr;
static std::mutex g_windowMutex;
static ComPtr<IDXGISwapChain1> g_persistentSwapchain;
static UINT g_persistentWidth = 1920;
static UINT g_persistentHeight = 540;
static std::atomic<bool> g_windowClassRegistered{false};

// Latest XR swapchain (per-eye) source dimensions, populated by RenderProjection.
// Used by menu/keyboard resize callbacks to compute a target window size.
static std::atomic<UINT> g_sourceWidth{0};
static std::atomic<UINT> g_sourceHeight{0};

struct Instance {
    XrInstance handle{XR_NULL_HANDLE};
    std::shared_ptr<simxr::Instance> core;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> controllerMotion;
    std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
    std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
    std::shared_ptr<flicker::Diagnostics> diagnostics;
};

// Private D3D12 composition uses BGRA8 with a separate sRGB view for quad blending.
static constexpr DXGI_FORMAT kPreviewRTFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
static constexpr UINT kQuadConstantCount = simxr::windows::D3D12PreviewPipelines::QuadConstantCount;
static constexpr UINT kPreviewFrames = simxr::windows::PreviewFrameSchedule::Capacity;
static constexpr UINT kPreviewMaxWidth = 1920;
static constexpr UINT kPreviewMaxHeight = 1080;

// Copied submission metadata; native acquisition, submission and retirement belong
// to D3D12Preview. A delayed explicit readback retains the matching source identity.
struct PreviewFrameMetadata12 {
    uint32_t frame{0};
    bool hasProjection{false};
    uint32_t quadLayers{0};
    bool quadComposed{false};
    int32_t quadRects[2][4]{};
    float quadSourceAlphaCoverage{0.0f};
    float headYaw{0};
    float headPitch{0};
    float headRoll{0};
    float headX{0};
    float headY{0};
    float headZ{0};
    mcp::ProjLogEntry proj{};
};

// Everything a Vulkan session needs on top of the D3D12 compositor it shares with a
// D3D12 session. The swapchain images are D3D12 resources either way; a Vulkan session
// creates them shared and hands the app imported VkImages over the same memory, so the
// preview, screenshot and quad paths never learn there is a second API involved.
struct VulkanSession {
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    VkCommandPool cmdPool{VK_NULL_HANDLE};
    VkCommandBuffer cmdBuffer{VK_NULL_HANDLE};
    // One shared D3D12 fence imported as a timeline VkSemaphore orders the app's queue
    // against the preview queue. Values are a single monotonic counter (Vulkan signals
    // one, D3D12 signals the next) rather than a two-state ping-pong: some drivers reject a
    // timeline signal that does not strictly increase.
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceHandle{nullptr};
    VkSemaphore semaphore{VK_NULL_HANDLE};
    uint64_t counter{0};
    uint64_t appSignalled{0};       // last value the app's queue signalled, 0 = never
    uint64_t previewSignalled{0};   // last value the preview queue signalled, 0 = never
    uint64_t previewWaited{0};      // last previewSignalled the app's queue was made to wait on
    bool timeline{false};           // false: fall back to CPU waits around the composite
};

struct Session {
    XrSession handle{XR_NULL_HANDLE};
    std::shared_ptr<simxr::Session> core;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> controllerMotion;
    std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
    std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
    std::shared_ptr<flicker::Diagnostics> diagnostics;
    XrSessionState state{XR_SESSION_STATE_IDLE};
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    // DX12 support
    ComPtr<ID3D12Device> d3d12Device;
    ComPtr<ID3D12CommandQueue> d3d12Queue;
    bool usesD3D12{false};
    // Vulkan support. usesD3D12 is set alongside this: the compositor is the D3D12 one.
    bool usesVulkan{false};
    VulkanSession vk;
    // OpenGL support
    HDC glDC{nullptr};
    HGLRC glRC{nullptr};
    bool usesOpenGL{false};

    std::shared_ptr<simxr::windows::D3D12Preview> preview12;
    PreviewFrameMetadata12 previewMetadata12[kPreviewFrames];
	std::shared_ptr<const simxr::windows::D3D12PreviewPipelines> previewPipelines12;

	std::unique_ptr<simxr::windows::D3D11Preview> preview11;

    // OpenGL preview: readback buffers and upload textures reused across frames
    // instead of being reallocated per frame.
    std::vector<uint8_t> glEyePixels[2];
    std::vector<uint8_t> glQuadPixels;


    // Desktop preview window (no thread - handled on main thread)
    HWND hwnd{nullptr};
    std::atomic<bool> isFocused{false};
    ComPtr<IDXGISwapChain1> previewSwapchain;
    UINT previewWidth{1920};
    UINT previewHeight{540};
    DXGI_FORMAT previewFormat{DXGI_FORMAT_UNKNOWN};  // Track format for matching
    // Current client area of the preview window (updated on WM_SIZE).
    // 0 means "not yet known" — use an initial sensible size for the first frame.
    std::atomic<UINT> clientWidth{0};
    std::atomic<UINT> clientHeight{0};
    std::mutex previewMutex;
};

struct Swapchain {
    XrSwapchain handle{(XrSwapchain)1};
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    uint32_t width{0}, height{0}, arraySize{2};
    uint32_t mipCount{1};
    // Backend type and images
    enum class Backend { D3D11, D3D12, OpenGL } backend{Backend::D3D11};
    std::vector<ComPtr<ID3D11Texture2D>> images;      // D3D11 path
    std::vector<ComPtr<ID3D12Resource>> images12;     // D3D12 path
    std::vector<D3D12_RESOURCE_STATES> imageStates12;
    // State the app is required to release images in, per XR_KHR_D3D12_enable:
    // RENDER_TARGET for colour, DEPTH_WRITE for depth.
    D3D12_RESOURCE_STATES releaseState12{D3D12_RESOURCE_STATE_COMMON};
    // Vulkan path: images12 still holds the shared D3D12 resources the compositor reads,
    // imagesVk the VkImages bound over the same memory that the app is handed.
    bool isVulkan{false};
    std::vector<VkImage> imagesVk;
    std::vector<VkDeviceMemory> memoryVk;
    std::vector<HANDLE> sharedHandles;
	std::unique_ptr<simxr::windows::OpenGLSwapchain> glImages;
    std::unique_ptr<simxr::Swapchain> state;
    uint32_t lastAcquired{UINT32_MAX};  // Initialize to invalid
    uint32_t lastReleased{UINT32_MAX};  // Initialize to invalid
    uint32_t imageCount{3};
};

static Instance g_instance{};
static Session g_session{};
static std::unordered_map<XrSwapchain, Swapchain> g_swapchains;

static std::shared_ptr<simxr::Session> RetainSession(XrSession handle)
{
    return RuntimeState().FindSession(handle).Shared();
}

static std::shared_ptr<simxr::Session> RetainSession()
{
    XrSession handle;
    { std::lock_guard lock(g_coreMutex); handle = g_session.handle; }
    return RetainSession(handle);
}

// Retain the owner associated with this exact native/core session pair.
// Detaching an instance never resets controller state beneath an in-flight call.
static std::shared_ptr<simxr::legacy::LegacyControllerMotion>
RetainControllerMotion(const std::shared_ptr<simxr::Session>& session)
{
	std::lock_guard lock(g_coreMutex);
	if (!session || session != g_session.core)
		return {};
	return g_session.controllerMotion;
}

static std::shared_ptr<mcp::WindowsLegacyCapture> RetainCompatibilityCapture()
{
    std::lock_guard lock(g_coreMutex);
    return g_instance.compatibilityCapture;
}

// Head tracking state for mouse look and WASD movement
static XrVector3f g_headPos = {0.0f, 1.7f, 0.0f};  // Start at standing eye height
static float g_headYaw = 0.0f;    // Rotation around Y axis (left/right)
static float g_headPitch = 0.0f;  // Rotation around X axis (up/down)
static float g_headRoll = 0.0f;   // Rotation around forward axis (head tilt). MCP-only — no keyboard binding.

// ---------- Diagnostic state: settable IPD and asymmetric per-eye FOV ----------
//
// By default the simulator publishes a UI-selected headset profile with
// asymmetric per-eye FOV and nonzero IPD. Symmetric FOV is still available
// from the FOV menu for sanity checks.
//
// When g_useCustomFov / g_useCustomIpd are set (via MCP set_fov / set_ipd
// / set_headset_profile), xrLocateViews emits the configured values
// instead. Headset profiles (quest3, index, etc.) preset both at once.
//
// FOV is stored in radians per OpenXR XrFovf convention:
//   angleLeft  < 0,  angleRight > 0
//   angleDown  < 0,  angleUp    > 0
// Per eye: index 0 = left eye, 1 = right eye.
static bool  g_useCustomFov = false;
static float g_eyeFovL[2] = { 0, 0 };
static float g_eyeFovR[2] = { 0, 0 };
static float g_eyeFovU[2] = { 0, 0 };
static float g_eyeFovD[2] = { 0, 0 };

static bool  g_useCustomIpd = false;
static float g_customIpd = 0.064f;  // meters

static XrFovf MakeFovDeg(float angleLeft, float angleRight, float angleUp, float angleDown) {
    const float DEG2RAD = 3.14159265f / 180.0f;
    return XrFovf{
        angleLeft * DEG2RAD,
        angleRight * DEG2RAD,
        angleUp * DEG2RAD,
        angleDown * DEG2RAD
    };
}

static XrFovf GetUiFov(uint32_t eyeIndex) {
    // Shared with ui::ActiveFovAspect, which snaps the recommended render target to the
    // shape of whatever this returns.
    if (!ui::UsesProfileFov()) {
        int fovDeg = ui::g_uiState.fovDegrees;
        if (fovDeg <= 0 || fovDeg > 180) fovDeg = 90;
        float fovRadians = fovDeg * 0.5f * 3.14159265f / 180.0f;
        // XrFovf fields are angles in radians, not tangent-space extents.
        return XrFovf{ -fovRadians, fovRadians, fovRadians, -fovRadians };
    }

    const ui::EyeFov& e = ui::GetActiveHeadsetSpec().eye[eyeIndex == 0 ? 0 : 1];
    return MakeFovDeg(e.angleLeft, e.angleRight, e.angleUp, e.angleDown);
}

static float GetUiIpdMeters() {
    return (std::max)(0.0f, (std::min)(0.2f, ui::g_uiState.ipdMeters));
}

static void HandleUiSettingsCommand(int cmd) {
    if (ui::IsFovSettingsCommand(cmd)) {
        g_useCustomFov = false;
    }
    if (ui::IsIpdSettingsCommand(cmd)) {
        g_useCustomIpd = false;
    }
}

// Anaglyph preview overlay: when enabled, the simulator's preview window
// composites left+right eyes into a red/cyan stereo image instead of
// side-by-side. Reveals IPD bugs (eyes don't converge -> red/cyan ghost)
// at a glance.
static bool g_anaglyphPreview = false;

// Pose sweep: auto-oscillate yaw / pitch / roll on a sine wave so the
// app sees a continuous range of head orientations. Catches off-axis
// quaternion bugs in seconds — anything mis-converted from OpenXR's
// right-handed frame to a left-handed game frame becomes a "world
// wobbles wrong direction" symptom that's hard to miss.
static bool  g_poseSweepEnabled  = false;
static float g_poseSweepStartT   = 0.0f;
static float g_poseSweepYawAmp   = 0.5f;   // radians (~28 deg)
static float g_poseSweepPitchAmp = 0.3f;   // radians (~17 deg)
static float g_poseSweepRollAmp  = 0.3f;   // radians (~17 deg)
static float g_poseSweepFreq     = 0.25f;  // Hz
static bool g_mouseCapture = false;
static POINT g_lastMousePos = {0, 0};

// Mouse look and Home reach the head pose through these rather than writing it: the frame
// loop integrates WASD into the same fields, and it is the only thread that should.
static std::atomic<int>  g_lookDeltaX{0};
static std::atomic<int>  g_lookDeltaY{0};
static std::atomic<bool> g_resetPoseRequested{false};

// ShowCursor keeps a counter, not a flag, so both ends of look mode have to go through one
// place -- losing focus mid-drag used to clear g_mouseCapture without the matching show,
// hiding the cursor for the rest of the session.
static void SetMouseLook(HWND hwnd, bool active) {
    if (active == g_mouseCapture) return;
    g_mouseCapture = active;
    if (active) {
        SetCapture(hwnd);
        GetCursorPos(&g_lastMousePos);
        ShowCursor(FALSE);
    } else {
        ReleaseCapture();
        ShowCursor(TRUE);
    }
    Logf("[SimXR] Mouse look %s", active ? "engaged" : "released");
}

// Middle-button drag panning the zoomed preview, in client coordinates.
static bool g_panDrag = false;
static POINT g_panLastPos = {0, 0};

// Inside a WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE pair: the user is dragging the window's
// frame, and every step of it would otherwise rewrite settings.json.
static bool g_sizingDrag = false;

static double InputClockSeconds()
{
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

static simxr::legacy::ControllerKeyboardSample PollControllerKeyboard(float frameSeconds)
{
	const auto down = [](int key)
	{
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	};
	simxr::legacy::ControllerKeyboardSample input;
	input.active = true;
	input.frameSeconds = frameSeconds;
	input.animationKeyDown = down('M');
	input.resetRight = down(VK_NUMPAD5);
	input.translationDirection = {static_cast<float>(down(VK_NUMPAD6) - down(VK_NUMPAD4)),
								  static_cast<float>(down(VK_ADD) - down(VK_SUBTRACT)),
								  static_cast<float>(down(VK_NUMPAD2) - down(VK_NUMPAD8))};
	input.rotationDirection = {static_cast<float>(down(VK_NUMPAD9) - down(VK_NUMPAD7)),
							   static_cast<float>(down(VK_NUMPAD1) - down(VK_NUMPAD3))};
	auto& left = input.hands[0];
	left.trigger = down(VK_LCONTROL);
	left.grip = down(VK_LMENU);
	left.menu = down('G');
	left.primary = down('V');
	left.secondary = down('B');
	if (down('I'))
		left.stick.y = 1;
	if (down('K'))
		left.stick.y = -1;
	if (down('J'))
		left.stick.x = -1;
	if (down('L'))
		left.stick.x = 1;
	auto& right = input.hands[1];
	right.trigger = down(VK_SPACE);
	right.grip = down('F');
	right.menu = down(VK_TAB);
	right.primary = down('R');
	right.secondary = down('T');
	right.thumbClick = down(VK_RETURN);
	if (down(VK_UP))
		right.stick.y = 1;
	if (down(VK_DOWN))
		right.stick.y = -1;
	if (down(VK_LEFT))
		right.stick.x = -1;
	if (down(VK_RIGHT))
		right.stick.x = 1;
	return input;
}

static simxr::legacy::ControllerOverrideEdit<float> DecodeHoldAxis(float value, float presentMinimum,
																   float activeMinimum)
{
	using Edit = simxr::legacy::ControllerOverrideEdit<float>;
	if (!(value >= presentMinimum))
		return {};
	if (value < activeMinimum)
		return Edit::Release();
	return Edit::Set(value);
}

static simxr::legacy::ControllerOverrideEdit<bool> DecodeHoldButton(float value)
{
	using Edit = simxr::legacy::ControllerOverrideEdit<bool>;
	if (!(value >= -0.5f))
		return {};
	if (value < 0)
		return Edit::Release();
	return Edit::Set(value >= 0.5f);
}

static simxr::legacy::ControllerHoldEdit DecodeControllerHold(const mcp::ControllerHoldCommand& command)
{
	using namespace simxr::legacy;
	ControllerHoldEdit edit;
	if (command.hand >= 0)
		edit.hand = command.hand == 0 ? ControllerHand::Left : ControllerHand::Right;
	edit.clear = command.clear;
	edit.trigger = DecodeHoldAxis(command.trigger, -0.5f, 0);
	edit.grip = DecodeHoldAxis(command.grip, -0.5f, 0);
	edit.primary = DecodeHoldButton(command.primary);
	edit.secondary = DecodeHoldButton(command.secondary);
	edit.menu = DecodeHoldButton(command.menu);
	edit.thumbClick = DecodeHoldButton(command.thumbClick);
	edit.stickX = DecodeHoldAxis(command.stickX, -1.5f, -1);
	edit.stickY = DecodeHoldAxis(command.stickY, -1.5f, -1);
	if (command.yaw != mcp::kHoldAngleUnset)
		edit.yaw = ControllerOverrideEdit<float>::Set(command.yaw);
	if (command.pitch != mcp::kHoldAngleUnset)
		edit.pitch = ControllerOverrideEdit<float>::Set(command.pitch);
	if (command.roll != mcp::kHoldAngleUnset)
		edit.roll = ControllerOverrideEdit<float>::Set(command.roll);
	return edit;
}

// Helper function to create quaternion from yaw and pitch
XrQuaternionf QuatFromYawPitch(float yaw, float pitch) {
    // Create rotation: first yaw around Y, then pitch around X
    float cy = cosf(yaw * 0.5f);
    float sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f);
    float sp = sinf(pitch * 0.5f);

    // Combine rotations (yaw * pitch)
    XrQuaternionf q;
    q.w = cy * cp;
    q.x = cy * sp;
    q.y = sy * cp;
    q.z = -sy * sp;
    return q;
}

// Yaw / Pitch / Roll: q = q_yaw * q_pitch * q_roll. Roll is rotation
// about the head's local forward axis (head tilt), used by the
// MCP-injected diagnostic poses so off-axis quaternion-handedness
// bugs surface in the simulator.
XrQuaternionf QuatFromYawPitchRoll(float yaw, float pitch, float roll) {
    const float hy = yaw   * 0.5f, cy = cosf(hy), sy = sinf(hy);
    const float hp = pitch * 0.5f, cp = cosf(hp), sp = sinf(hp);
    const float hr = roll  * 0.5f, cr = cosf(hr), sr = sinf(hr);
    XrQuaternionf qyp;  // q_yaw * q_pitch (matches QuatFromYawPitch above)
    qyp.x = cy * sp;
    qyp.y = sy * cp;
    qyp.z = -sy * sp;
    qyp.w = cy * cp;
    XrQuaternionf q;  // qyp * q_roll where q_roll = (0, 0, sr, cr)
    q.x = qyp.x * cr + qyp.y * sr;
    q.y = qyp.y * cr - qyp.x * sr;
    q.z = qyp.z * cr + qyp.w * sr;
    q.w = qyp.w * cr - qyp.z * sr;
    return q;
}

static inline XrQuaternionf MultiplyQuaternions(const XrQuaternionf& a, const XrQuaternionf& b) {
    return XrQuaternionf{
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

// Rotate a vector by a quaternion (q * v * q^-1)
static inline XrVector3f RotateVectorByQuaternion(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf qv{ v.x, v.y, v.z, 0.0f };
    const XrQuaternionf qinv{ -q.x, -q.y, -q.z, q.w };
    const XrQuaternionf r = MultiplyQuaternions(MultiplyQuaternions(q, qv), qinv);
    return XrVector3f{ r.x, r.y, r.z };
}

static void GetEffectiveHeadAngles(float& yaw, float& pitch, float& roll) {
    yaw = g_headYaw; pitch = g_headPitch; roll = g_headRoll;
    if (!g_poseSweepEnabled) return;
    const float t = (float)GetTickCount64() * 0.001f - g_poseSweepStartT;
    const float TWO_PI = 6.2831853f;
    const float w = TWO_PI * g_poseSweepFreq;
    yaw   = g_poseSweepYawAmp   * sinf(w * t);
    pitch = g_poseSweepPitchAmp * sinf(w * t + 1.0f);
    roll  = g_poseSweepRollAmp  * sinf(w * t + 2.0f);
}

static XrFovf GetViewFov(uint32_t eye) {
    if (g_useCustomFov) return XrFovf{ g_eyeFovL[eye], g_eyeFovR[eye], g_eyeFovU[eye], g_eyeFovD[eye] };
    return GetUiFov(eye);
}

// --- Composition layer blending -------------------------------------------------------------------
// OpenXR 1.0, XrCompositionLayerFlagBits: the layer's alpha channel is only live when
// BLEND_TEXTURE_SOURCE_ALPHA is set, and is premultiplied into the colour channels unless
// UNPREMULTIPLIED_ALPHA says otherwise. CORRECT_CHROMATIC_ABERRATION is a legitimate no-op
// here - the preview draws no distortion mesh to correct.
enum class LayerBlend { Opaque, Premultiplied, Unpremultiplied };

static inline LayerBlend BlendForLayerFlags(XrCompositionLayerFlags flags) {
    if (!(flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)) return LayerBlend::Opaque;
    return (flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? LayerBlend::Unpremultiplied
                                                                    : LayerBlend::Premultiplied;
}

// The preview swapchain is created UNORM because FLIP_DISCARD rejects _SRGB, so every render
// target view over it has to opt back into the gamma encode by hand. Without this the GPU
// blends linear values into a buffer the display reads as sRGB and the layer comes out dark.
static inline DXGI_FORMAT SrgbRtvFormat(DXGI_FORMAT backbufferFormat) {
    if (backbufferFormat == DXGI_FORMAT_R8G8B8A8_UNORM) return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (backbufferFormat == DXGI_FORMAT_B8G8R8A8_UNORM) return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    return backbufferFormat;
}

static bool CreatePreviewRtv(Session& s, ID3D11Texture2D* backbuffer,
                             ComPtr<ID3D11RenderTargetView>& out) {
    D3D11_RENDER_TARGET_VIEW_DESC desc = {};
    desc.Format = SrgbRtvFormat(s.previewFormat);
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    if (SUCCEEDED(s.d3d11Device->CreateRenderTargetView(backbuffer, &desc, out.ReleaseAndGetAddressOf()))) {
        return true;
    }
    Log("[SimXR] Explicit sRGB RTV failed, falling back to auto format");
    return SUCCEEDED(s.d3d11Device->CreateRenderTargetView(backbuffer, nullptr, out.ReleaseAndGetAddressOf()));
}

static simxr::windows::D3D11Preview* EnsureD3D11Preview(Session& session)
{
	if (!session.d3d11Device)
		return nullptr;
	if (session.preview11 && session.preview11->IsFor(session.d3d11Device.Get()))
		return session.preview11.get();
	try
	{
		auto replacement = std::make_unique<simxr::windows::D3D11Preview>(session.d3d11Device.Get());
		session.preview11 = std::move(replacement);
		return session.preview11.get();
	}
	catch (const std::exception& error)
	{
		Logf("[SimXR] D3D11 preview initialization failed: %s", error.what());
		return nullptr;
	}
}

// Compute the natural canvas size for the current stereo layout, expressed in
// source pixels (i.e. independent of the window/client size). Used to derive
// the content aspect ratio for letterboxing.
static void ComputeContentDims(int srcW, int srcH,
                                ui::ViewMode mode, ui::DisplayLayout layout,
                                int& contentW, int& contentH) {
    if (mode == ui::ViewMode::BothEyes) {
        if (layout == ui::DisplayLayout::SideBySide) {
            contentW = srcW * 2;
            contentH = srcH;
        } else if (layout == ui::DisplayLayout::OverUnder) {
            contentW = srcW;
            contentH = srcH * 2;
        } else { // Anaglyph: both eyes overlap in same frame
            contentW = srcW;
            contentH = srcH;
        }
    } else {
        contentW = srcW;
        contentH = srcH;
    }
    if (contentW <= 0) contentW = 1;
    if (contentH <= 0) contentH = 1;
}

// Largest rect with (contentW : contentH) aspect that fits inside (dstW × dstH),
// centered. Used to letterbox/pillarbox the stereo image into the window.
struct FitRect { float x, y, w, h; };
static simxr::PreviewEyePacking PreviewPacking(ui::DisplayLayout layout)
{
	switch (layout)
	{
	case ui::DisplayLayout::OverUnder:
		return simxr::PreviewEyePacking::OverUnder;
	case ui::DisplayLayout::Anaglyph:
		return simxr::PreviewEyePacking::Anaglyph;
	default:
		return simxr::PreviewEyePacking::SideBySide;
	}
}

static simxr::PreviewEyeLayout
PrepareNativeEyeLayout(const FitRect& rectangle, ui::ViewMode selection, ui::DisplayLayout packing,
					   simxr::PreviewPixelAlignment alignment = simxr::PreviewPixelAlignment::Fractional)
{
	auto eyes = simxr::PreviewEyeSelection::Both;
	if (selection == ui::ViewMode::LeftEyeOnly)
		eyes = simxr::PreviewEyeSelection::Left;
	else if (selection == ui::ViewMode::RightEyeOnly)
		eyes = simxr::PreviewEyeSelection::Right;
	return simxr::PreparePreviewEyeLayout({rectangle.x, rectangle.y, rectangle.w, rectangle.h}, eyes,
										  PreviewPacking(packing), alignment);
}

static FitRect ComputeFitRect(int contentW, int contentH, int dstW, int dstH) {
    FitRect r{0.0f, 0.0f, (float)dstW, (float)dstH};
    if (contentW <= 0 || contentH <= 0 || dstW <= 0 || dstH <= 0) return r;
    double ca = (double)contentW / (double)contentH;
    double da = (double)dstW / (double)dstH;
    if (ca > da) {
        // Content wider than dest → pillar (full width, shorter height)
        r.w = (float)dstW;
        r.h = (float)((double)dstW / ca);
        r.x = 0.0f;
        r.y = ((float)dstH - r.h) * 0.5f;
    } else {
        // Content taller than dest → letterbox (full height, narrower width)
        r.h = (float)dstH;
        r.w = (float)((double)dstH * ca);
        r.x = ((float)dstW - r.w) * 0.5f;
        r.y = 0.0f;
    }
    return r;
}

// Shrink w x h, keeping its aspect, until it fits the desktop work area.
static void ClampToWorkArea(int& w, int& h) {
    if (w <= 0 || h <= 0) return;
    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) return;
    const int maxW = (int)((wa.right - wa.left) * 0.9);
    const int maxH = (int)((wa.bottom - wa.top) * 0.9);
    if (maxW <= 0 || maxH <= 0 || (w <= maxW && h <= maxH)) return;
    const double scale = (std::min)((double)maxW / w, (double)maxH / h);
    w = (std::max)(320, (int)(w * scale));
    h = (std::max)(240, (int)(h * scale));
}

// The shape the preview maps the eyes onto: the reported frustum at panel scale, laid out
// for the current view mode. This is deliberately NOT the app's buffer size. An app may
// render the frustum into a buffer of any shape - a 16:9 one has non-square pixels, and
// showing those 1:1 stretches the image ~2x horizontally. Resampling the buffer into the
// frustum's shape is what a real compositor does.
static void ComputeDisplayDims(int& contentW, int& contentH) {
    uint32_t eyeW = 0, eyeH = 0;
    ui::GetDisplayEyeSize(eyeW, eyeH);
    ComputeContentDims((int)eyeW, (int)eyeH, ui::g_uiState.viewMode, ui::g_uiState.displayLayout,
                       contentW, contentH);
}

static perf::ProjectionTimingTracker g_projectionTiming;

// Build the stats payload for the title bar (used when "Show Statistics" is on).
static ui::StatsInfo BuildStatsInfo(rt::Session& s) {
    ui::StatsInfo si;
    si.sourceW = (int)rt::g_sourceWidth.load();
    si.sourceH = (int)rt::g_sourceHeight.load();
    si.clientW = (int)s.clientWidth.load();
    si.clientH = (int)s.clientHeight.load();
    si.headX = rt::g_headPos.x;
    si.headY = rt::g_headPos.y;
    si.headZ = rt::g_headPos.z;
    constexpr float RAD2DEG = 57.2957795f;
    si.yawDeg   = rt::g_headYaw   * RAD2DEG;
    si.pitchDeg = rt::g_headPitch * RAD2DEG;
    si.rollDeg  = rt::g_headRoll  * RAD2DEG;
    const auto timing = g_projectionTiming.Snapshot(
        perf::ProjectionTimingTracker::Clock::now());
    si.projectionTimingActive = timing.active;
    si.projectionFps = timing.fps;
    si.latestFrameMs = timing.latestFrameMs;
    si.p50FrameMs = timing.p50FrameMs;
    si.p95FrameMs = timing.p95FrameMs;
    si.projectionTimingSamples = (uint32_t)timing.sampleCount;
    return si;
}

// Repaint the title from the message loop, so a toggle that changes what the
// title says lands immediately instead of at the render loop's next 500ms tick
// -- which never arrives at all if the app has stopped submitting frames.
static void RefreshTitleNow(HWND hwnd) {
    if (!hwnd || hwnd != rt::g_previewUi.hwnd.load()) return;
    ui::StatsInfo si = BuildStatsInfo(rt::g_session);
    ui::UpdateWindowTitle(hwnd, &si);
}

// Read the current preview-window client size (set on WM_SIZE). If the user
// hasn't sized it yet, fall back to the menu-zoom-derived target.
static void GetPreviewClientSize(rt::Session& s, int srcW, int srcH, int& outW, int& outH) {
    UINT cw = s.clientWidth.load();
    UINT ch = s.clientHeight.load();
    if (cw > 0 && ch > 0) {
        outW = (int)cw;
        outH = (int)ch;
        return;
    }
    // Initial fallback (first frame, before WM_SIZE has fired)
    uint32_t eyeW = 0, eyeH = 0;
    ui::GetDisplayEyeSize(eyeW, eyeH);
    int tw = 0, th = 0;
    ui::CalculateWindowSize((int)eyeW, (int)eyeH, tw, th);
    ClampToWorkArea(tw, th);
    outW = tw;
    outH = th;
}

// Publish what zoom and pan are measured against: the window's client area and the shape
// the eyes are laid out in. The render path refreshes this every frame; the input
// handlers refresh it before acting, so a wheel notch still lands on a stalled app.
static void RefreshPreviewGeometry(int clientW, int clientH) {
    int contentW = 0, contentH = 0;
    ComputeDisplayDims(contentW, contentH);
    ui::g_previewGeom = { clientW, clientH, contentW, contentH };
}

static void RefreshPreviewGeometry(HWND hWnd) {
    RECT cr{};
    if (!hWnd || !GetClientRect(hWnd, &cr)) return;
    RefreshPreviewGeometry(cr.right - cr.left, cr.bottom - cr.top);
}

// Where the stereo image lands inside a dstW x dstH target, in that target's pixels.
// "Fit to Window" gives the largest centred rect of the content's own aspect that fits;
// any other zoom is an absolute content scale placed by the pan offset, which the target
// then clips. Either way the target is cleared first, so whatever is left over is black.
static FitRect ComputePresentRect(int dstW, int dstH) {
    int contentW = 0, contentH = 0;
    ComputeDisplayDims(contentW, contentH);
    const ui::PreviewRect r = ui::ComputePreviewRect(ui::PreviewGeometry{ dstW, dstH, contentW, contentH });
    return FitRect{ r.x, r.y, r.w, r.h };
}

// Size of the D3D12 preview's offscreen RT. It follows the window's aspect and
// zoom geometry, but is capped independently from a maximized/high-DPI window
// so desktop debugging cannot consume an extra full frame of CPU time.
static void ComputePreviewRTSize(rt::Session& s, int& outW, int& outH) {
    int clientW = 0, clientH = 0;
    GetPreviewClientSize(s, 0, 0, clientW, clientH);
    const double scale = (std::min)({
        1.0,
        (double)kPreviewMaxWidth / (std::max)(1, clientW),
        (double)kPreviewMaxHeight / (std::max)(1, clientH),
    });
    outW = (std::max)(1, (int)std::lround(clientW * scale));
    outH = (std::max)(1, (int)std::lround(clientH * scale));
}

// Snap the preview window so its client aspect matches the content aspect,
// keeping the current client width. Height is ours to derive; width is whatever
// the user last chose. No-op when the aspect already matches. Called from
// WM_SIZE, and from the render path when the content aspect itself changes.
static void FitWindowToContentAspect(HWND hWnd) {
    if (!hWnd) return;
    // Fit mode deliberately permits any window/monitor aspect: it letterboxes into
    // whatever it is given, so maximizing must stay maximized instead of being snapped
    // back to an aspect-correct floating window.
    if (ui::g_uiState.fitToWindow) return;

    RECT cr{};
    if (!GetClientRect(hWnd, &cr)) return;
    const int cw = cr.right - cr.left;
    const int ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0) return;

    int contentW = 0, contentH = 0;
    ComputeDisplayDims(contentW, contentH);
    if (contentW <= 0 || contentH <= 0) return;

    const double contentAspect = (double)contentW / (double)contentH;
    const double clientAspect  = (double)cw / (double)ch;
    if (fabs(contentAspect - clientAspect) < 0.005 * (contentAspect + clientAspect)) return;

    int targetCH = (int)((double)cw / contentAspect + 0.5);
    if (targetCH < 1) targetCH = 1;

    RECT rc{0, 0, (LONG)cw, targetCH};
    AdjustWindowRectEx(&rc, (DWORD)GetWindowLongW(hWnd, GWL_STYLE), GetMenu(hWnd) != nullptr,
                       (DWORD)GetWindowLongW(hWnd, GWL_EXSTYLE));
    SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

// Re-shape the window for a new content aspect -- a view mode or layout change, which
// turns a side-by-side pair into a single eye and back. Zoom no longer touches the
// window, so this is the only thing that resizes it, and it keeps the client area the
// user had rather than jumping back to a panel-derived size every time.
static void ResizeWindowForContent(HWND hWnd) {
    if (!hWnd) return;

    int contentW = 0, contentH = 0;
    ComputeDisplayDims(contentW, contentH);
    if (contentW <= 0 || contentH <= 0) return;
    const double aspect = (double)contentW / (double)contentH;

    RECT cr{};
    if (!GetClientRect(hWnd, &cr)) return;
    const double area = (double)(cr.right - cr.left) * (double)(cr.bottom - cr.top);
    if (area < 1.0) return;

    int targetW = (int)(sqrt(area * aspect) + 0.5);
    int targetH = (int)(sqrt(area / aspect) + 0.5);
    ClampToWorkArea(targetW, targetH);

    RECT rc{0, 0, targetW, targetH};
    AdjustWindowRectEx(&rc, (DWORD)GetWindowLongW(hWnd, GWL_STYLE), GetMenu(hWnd) != nullptr,
                       (DWORD)GetWindowLongW(hWnd, GWL_EXSTYLE));
    SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

// Teardown/source destruction requires actual completion; a timeout is not idle.
static bool WaitForPreviewIdle(rt::Session& session)
{
	return !session.preview12 || session.preview12->WaitIdle();
}

static void ResetD3D12PreviewResources(rt::Session& session)
{
	auto preview = std::move(session.preview12);
	if (preview)
		preview->Close();
	session.previewPipelines12.reset();
}
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Dark menu bar: the WM_UAH* custom-draw messages plus the light seam
    // DefWindowProc leaves under the bar on WM_NCPAINT/WM_NCACTIVATE.
    LRESULT darkMenuResult = 0;
    if (ui::HandleDarkMenuMessage(hWnd, msg, wParam, lParam, &darkMenuResult)) {
        return darkMenuResult;
    }
    switch (msg) {
        case WM_CLOSE:
            if (rt::g_session.handle != XR_NULL_HANDLE) {
                rt::PushState(rt::g_session.handle, XR_SESSION_STATE_EXITING);
            }
            Log("[SimXR] WndProc: WM_CLOSE received");
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            // DON'T call PostQuitMessage - we're a DLL, not the main app!
            // PostQuitMessage would tell the host application to exit.
            Log("[SimXR] WndProc: WM_DESTROY received");
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                rt::g_session.isFocused = true;
                Log("[SimXR] WndProc: WM_ACTIVATE -> focused");
                // Push FOCUSED state if we were VISIBLE
                if (rt::g_session.state == XR_SESSION_STATE_VISIBLE) {
                    rt::PushState(rt::g_session.handle, XR_SESSION_STATE_FOCUSED);
                }
            } else {
                rt::g_session.isFocused = false;
                Log("[SimXR] WndProc: WM_ACTIVATE -> unfocused");
                rt::SetMouseLook(hWnd, false);  // also puts the hidden cursor back
                rt::g_panDrag = false;
                ReleaseCapture();
                // Push VISIBLE state if we were FOCUSED
                if (rt::g_session.state == XR_SESSION_STATE_FOCUSED) {
                    rt::PushState(rt::g_session.handle, XR_SESSION_STATE_VISIBLE);
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
            Logf("[SimXR] WM_LBUTTONDOWN: focused=%d", rt::g_session.isFocused.load());
            if (rt::g_session.isFocused) {
                rt::SetMouseLook(hWnd, true);
            }
            return 0;
        case WM_LBUTTONUP:
            rt::SetMouseLook(hWnd, false);
            return 0;
        case WM_MBUTTONDOWN:
            // Middle drag pans the zoomed image. Left is already mouse look, and this
            // way panning stays available while the head is being aimed.
            rt::g_panDrag = true;
            rt::g_panLastPos = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetCapture(hWnd);
            return 0;
        case WM_MBUTTONUP:
            if (rt::g_panDrag) {
                rt::g_panDrag = false;
                ReleaseCapture();
            }
            return 0;
        case WM_ENTERSIZEMOVE:
            rt::g_sizingDrag = true;
            return 0;
        case WM_EXITSIZEMOVE:
            rt::g_sizingDrag = false;
            ui::SaveSettings();
            return 0;
        case WM_SIZING: {
            // Fit mode accepts arbitrary window shapes and letterboxes into them. Manual
            // zoom mode keeps the aspect-locked inspector behavior that makes pixel-scale
            // comparisons predictable.
            if (ui::g_uiState.fitToWindow) break;

            // Constrain interactive resize to the current content aspect ratio
            // so the window itself follows the stereo layout (no letterbox).
            int contentW = 0, contentH = 0;
            rt::ComputeDisplayDims(contentW, contentH);
            if (contentW <= 0 || contentH <= 0) break;
            double aspect = (double)contentW / (double)contentH;

            // Measure the non-client overhead (borders + title bar + menu)
            DWORD style   = (DWORD)GetWindowLongW(hWnd, GWL_STYLE);
            DWORD exStyle = (DWORD)GetWindowLongW(hWnd, GWL_EXSTYLE);
            BOOL  hasMenu = GetMenu(hWnd) != nullptr;
            RECT probe{0, 0, 100, 100};
            AdjustWindowRectEx(&probe, style, hasMenu, exStyle);
            int ncW = (probe.right - probe.left) - 100;
            int ncH = (probe.bottom - probe.top) - 100;

            RECT* r = (RECT*)lParam;
            int clientW = (r->right - r->left) - ncW;
            int clientH = (r->bottom - r->top) - ncH;
            if (clientW < 1) clientW = 1;
            if (clientH < 1) clientH = 1;

            UINT edge = (UINT)wParam;
            // Decide which axis drives the constraint:
            //   left/right edge → width is canonical
            //   top/bottom edge → height is canonical
            //   corner → use whichever side the user pulled "further" so
            //            dragging outward grows the window in both axes
            bool widthCanonical;
            switch (edge) {
                case WMSZ_LEFT:
                case WMSZ_RIGHT:
                    widthCanonical = true;
                    break;
                case WMSZ_TOP:
                case WMSZ_BOTTOM:
                    widthCanonical = false;
                    break;
                default:
                    widthCanonical = ((double)clientW / aspect) >= (double)clientH;
                    break;
            }

            if (widthCanonical) {
                clientH = (int)((double)clientW / aspect + 0.5);
            } else {
                clientW = (int)((double)clientH * aspect + 0.5);
            }
            if (clientW < 1) clientW = 1;
            if (clientH < 1) clientH = 1;

            int outerW = clientW + ncW;
            int outerH = clientH + ncH;

            // Anchor the rect to whichever corner the user is NOT dragging
            switch (edge) {
                case WMSZ_LEFT:
                    r->left   = r->right  - outerW;
                    r->bottom = r->top    + outerH;
                    break;
                case WMSZ_RIGHT:
                    r->right  = r->left   + outerW;
                    r->bottom = r->top    + outerH;
                    break;
                case WMSZ_TOP:
                    r->top    = r->bottom - outerH;
                    r->right  = r->left   + outerW;
                    break;
                case WMSZ_BOTTOM:
                    r->bottom = r->top    + outerH;
                    r->right  = r->left   + outerW;
                    break;
                case WMSZ_TOPLEFT:
                    r->left   = r->right  - outerW;
                    r->top    = r->bottom - outerH;
                    break;
                case WMSZ_TOPRIGHT:
                    r->right  = r->left   + outerW;
                    r->top    = r->bottom - outerH;
                    break;
                case WMSZ_BOTTOMLEFT:
                    r->left   = r->right  - outerW;
                    r->bottom = r->top    + outerH;
                    break;
                case WMSZ_BOTTOMRIGHT:
                default:
                    r->right  = r->left   + outerW;
                    r->bottom = r->top    + outerH;
                    break;
            }
            return TRUE;
        }
        case WM_PAINT: {
            // The frame path owns the client area and never goes through WM_PAINT, so fall
            // through and leave it alone. Mirror off is the exception: nothing paints then.
            if (ui::g_uiState.previewFps > 0) {
                if (rt::g_session.usesD3D12) { ValidateRect(hWnd, nullptr); return 0; }
                break;
            }
            PAINTSTRUCT ps;
            if (HDC hdc = BeginPaint(hWnd, &ps)) {
                FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
                RECT rc{};
                GetClientRect(hWnd, &rc);
                HGDIOBJ oldFont = SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, ui::Colors::TextSecondary);
                DrawTextW(hdc,
                          L"Mirror Rate is off \x2014 Tools \x2192 Mirror Rate shows the preview again",
                          -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                SelectObject(hdc, oldFont);
                EndPaint(hWnd, &ps);
            }
            return 0;
        }
        case WM_SIZE: {
            // Capture the new client area. The render path picks this up next
            // frame and resizes the swapchain (D3D11/GL) or recomputes the
            // letterbox destination (D3D12 GDI). Ignore minimize (0×0).
            UINT cw = LOWORD(lParam);
            UINT ch = HIWORD(lParam);
            if (cw == 0 || ch == 0) return 0;
            rt::g_session.clientWidth.store(cw);
            rt::g_session.clientHeight.store(ch);

            // Remember the size for the next run. Recorded ahead of the aspect snap
            // below, which resizes again and lands here a second time -- so the
            // corrected size is what the last write leaves in the file. Skipped while
            // a drag is in progress; WM_EXITSIZEMOVE writes once at the end of it.
            ui::g_uiState.windowWidth = (int)cw;
            ui::g_uiState.windowHeight = (int)ch;
            if (!rt::g_sizingDrag) ui::SaveSettings();

            // Fit mode keeps the dimensions Windows assigned, including a maximized client
            // area, and lets ComputePreviewRect centre the image in them.
            if (ui::g_uiState.fitToWindow) return 0;

            // WM_SIZING constrains interactive drags, but maximize, snap, and
            // programmatic resizes bypass it. If the resulting client aspect
            // doesn't match content aspect, snap the window so we never need
            // to fall back to the letterbox bars at runtime.
            static thread_local bool inFix = false;
            if (inFix) return 0;

            int contentW = 0, contentH = 0;
            rt::ComputeDisplayDims(contentW, contentH);
            if (contentW <= 0 || contentH <= 0) return 0;
            double contentAspect = (double)contentW / (double)contentH;
            double clientAspect  = (double)cw / (double)ch;
            // Tolerance: don't fight ourselves over sub-pixel rounding.
            if (fabs(contentAspect - clientAspect) <
                0.005 * (contentAspect + clientAspect)) {
                return 0;
            }

            DWORD style   = (DWORD)GetWindowLongW(hWnd, GWL_STYLE);
            DWORD exStyle = (DWORD)GetWindowLongW(hWnd, GWL_EXSTYLE);
            BOOL  hasMenu = GetMenu(hWnd) != nullptr;

            if (wParam == SIZE_MAXIMIZED) {
                // Restore out of maximized state, then size to the largest
                // aspect-correct rect that fits in the monitor's work area.
                HMONITOR hmon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                if (!GetMonitorInfoW(hmon, &mi)) return 0;
                int availW = mi.rcWork.right - mi.rcWork.left;
                int availH = mi.rcWork.bottom - mi.rcWork.top;

                RECT probe{0, 0, 100, 100};
                AdjustWindowRectEx(&probe, style & ~WS_MAXIMIZE, hasMenu, exStyle);
                int ncW = (probe.right - probe.left) - 100;
                int ncH = (probe.bottom - probe.top) - 100;
                int maxCW = availW - ncW; if (maxCW < 1) maxCW = 1;
                int maxCH = availH - ncH; if (maxCH < 1) maxCH = 1;

                rt::FitRect fit = rt::ComputeFitRect(contentW, contentH, maxCW, maxCH);
                int targetCW = (int)fit.w; if (targetCW < 1) targetCW = 1;
                int targetCH = (int)fit.h; if (targetCH < 1) targetCH = 1;

                RECT rc{0, 0, targetCW, targetCH};
                AdjustWindowRectEx(&rc, style & ~WS_MAXIMIZE, hasMenu, exStyle);
                int outerW = rc.right - rc.left;
                int outerH = rc.bottom - rc.top;
                int x = mi.rcWork.left + (availW - outerW) / 2;
                int y = mi.rcWork.top  + (availH - outerH) / 2;

                inFix = true;
                ShowWindow(hWnd, SW_RESTORE);
                SetWindowPos(hWnd, nullptr, x, y, outerW, outerH, SWP_NOZORDER);
                inFix = false;
            } else {
                // Anything else (DPI change, programmatic, Aero snap, etc.):
                // preserve the new width, recompute height to match aspect.
                inFix = true;
                rt::FitWindowToContentAspect(hWnd);
                inFix = false;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (rt::g_panDrag) {
                const POINT pos{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ui::g_uiState.panX += (float)(pos.x - rt::g_panLastPos.x);
                ui::g_uiState.panY += (float)(pos.y - rt::g_panLastPos.y);
                rt::g_panLastPos = pos;
                rt::RefreshPreviewGeometry(hWnd);
                ui::ClampPan();
                return 0;
            }
            if (rt::g_mouseCapture) {
                POINT currentPos;
                GetCursorPos(&currentPos);
                
                // Calculate delta
                int deltaX = currentPos.x - rt::g_lastMousePos.x;
                int deltaY = currentPos.y - rt::g_lastMousePos.y;
                
                rt::g_lookDeltaX.fetch_add(deltaX);
                rt::g_lookDeltaY.fetch_add(deltaY);

                // Reset cursor to center of window to avoid hitting screen edges
                RECT rect;
                GetWindowRect(hWnd, &rect);
                int centerX = (rect.left + rect.right) / 2;
                int centerY = (rect.top + rect.bottom) / 2;
                SetCursorPos(centerX, centerY);
                rt::g_lastMousePos.x = centerX;
                rt::g_lastMousePos.y = centerY;
            }
            return 0;
        case WM_COMMAND:
            // The zoom items anchor on the middle of the window, so they need the
            // geometry to be current even if no frame has been presented since a resize.
            rt::RefreshPreviewGeometry(hWnd);
            if (ui::HandleMenuCommand(hWnd, wParam,
                [hWnd]() { rt::ResizeWindowForContent(hWnd); },
                []() { if (auto capture = rt::RetainCompatibilityCapture()) capture->RequestScreenshot(); },
                []() {
                    rt::g_resetPoseRequested.store(true);
                    ui::SetFitToWindow();
                    ui::SaveSettings();
                },
                [](int cmd) { rt::HandleUiSettingsCommand(cmd); }
            )) {
                rt::RefreshTitleNow(hWnd);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            // Ahead of the gate below, which swallows every key while look mode is on.
            if (wParam == VK_ESCAPE && rt::g_mouseCapture) {
                rt::SetMouseLook(hWnd, false);
                return 0;
            }
            if (!rt::g_mouseCapture) {
                rt::RefreshPreviewGeometry(hWnd);
                if (ui::HandleKeyboardShortcut(hWnd, wParam,
                    [hWnd]() { rt::ResizeWindowForContent(hWnd); },
                    []() { if (auto capture = rt::RetainCompatibilityCapture()) capture->RequestScreenshot(); },
                    []() {
                        rt::g_resetPoseRequested.store(true);
                        ui::SetFitToWindow();
                        ui::SaveSettings();
                    },
                    [](int cmd) { rt::HandleUiSettingsCommand(cmd); }
                )) {
                    rt::RefreshTitleNow(hWnd);
                    return 0;
                }
            }
            break;
        case WM_MOUSEWHEEL: {
            // Wheel coordinates are on the screen, unlike every other mouse message.
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            rt::RefreshPreviewGeometry(hWnd);
            ui::HandleMouseWheel(hWnd, GET_WHEEL_DELTA_WPARAM(wParam), (float)pt.x, (float)pt.y);
            rt::RefreshTitleNow(hWnd);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace rt

// ============ Vulkan sessions (XR_KHR_vulkan_enable / XR_KHR_vulkan_enable2) ============
//
// The compositor stays D3D12. A Vulkan session's swapchain images are D3D12 committed
// resources created with D3D12_HEAP_FLAG_SHARED and imported into the app's VkDevice
// through VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, so the app renders into
// VkImages and the preview, quad, screenshot and burst-capture paths read the very same
// pixels as ID3D12Resources without knowing a second API exists.
//
// Interop needs ALLOW_SIMULTANEOUS_ACCESS on colour so the D3D12
// side is uncompressed, a typeless format for depth, an explicit
// D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, and a timeline semaphore driven by one
// strictly increasing counter rather than a two-value ping-pong.
namespace vkrt {

struct Dispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{nullptr};
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{nullptr};
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices{nullptr};
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2{nullptr};
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{nullptr};
    PFN_vkGetDeviceQueue GetDeviceQueue{nullptr};
    PFN_vkDeviceWaitIdle DeviceWaitIdle{nullptr};
    PFN_vkQueueWaitIdle QueueWaitIdle{nullptr};
    PFN_vkQueueSubmit QueueSubmit{nullptr};
    PFN_vkCreateImage CreateImage{nullptr};
    PFN_vkDestroyImage DestroyImage{nullptr};
    PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2{nullptr};
    PFN_vkBindImageMemory2 BindImageMemory2{nullptr};
    PFN_vkAllocateMemory AllocateMemory{nullptr};
    PFN_vkFreeMemory FreeMemory{nullptr};
    PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR{nullptr};
    PFN_vkCreateSemaphore CreateSemaphore{nullptr};
    PFN_vkDestroySemaphore DestroySemaphore{nullptr};
    PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR{nullptr};
    PFN_vkCreateCommandPool CreateCommandPool{nullptr};
    PFN_vkDestroyCommandPool DestroyCommandPool{nullptr};
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers{nullptr};
    PFN_vkResetCommandPool ResetCommandPool{nullptr};
    PFN_vkBeginCommandBuffer BeginCommandBuffer{nullptr};
    PFN_vkEndCommandBuffer EndCommandBuffer{nullptr};
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier{nullptr};
};

static Dispatch g_d{};

// The app hands one over with XR_KHR_vulkan_enable2; a v1 app never does, so fall back to
// whatever vulkan-1.dll the process already has. GetModuleHandle first: an app that ships
// a proxy loader next to its executable would have LoadLibrary resolve to the proxy
// anyway, and this avoids a second reference on it.
static PFN_vkGetInstanceProcAddr g_appGipa = nullptr;

// vkCreateDevice and vkEnumerateDeviceExtensionProperties are instance-level: a null
// VkInstance only resolves the four global entry points. xrCreateVulkanDeviceKHR is not
// handed an instance, so remember the one the app created or asked about.
static VkInstance g_lastInstance = VK_NULL_HANDLE;

static PFN_vkGetInstanceProcAddr Gipa() {
    if (g_appGipa) return g_appGipa;
    static PFN_vkGetInstanceProcAddr cached = []() -> PFN_vkGetInstanceProcAddr {
        HMODULE m = GetModuleHandleA("vulkan-1.dll");
        if (!m) m = LoadLibraryA("vulkan-1.dll");
        if (!m) { Log("[SimXR][VK] vulkan-1.dll is not loadable"); return nullptr; }
        char path[MAX_PATH]{};
        GetModuleFileNameA(m, path, (DWORD)sizeof(path));
        Logf("[SimXR][VK] loader module: %s", path);
        return (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr");
    }();
    return cached;
}

template <typename T>
static T InstFn(VkInstance inst, const char* name, const char* altName = nullptr) {
    PFN_vkGetInstanceProcAddr gipa = Gipa();
    if (!gipa) return nullptr;
    T fn = (T)gipa(inst, name);
    if (!fn && altName) fn = (T)gipa(inst, altName);
    return fn;
}

template <typename T>
static T DevFn(VkDevice dev, const char* name, const char* altName = nullptr) {
    if (!g_d.GetDeviceProcAddr) return nullptr;
    T fn = (T)g_d.GetDeviceProcAddr(dev, name);
    if (!fn && altName) fn = (T)g_d.GetDeviceProcAddr(dev, altName);
    return fn;
}

static bool LoadInstanceLevel(VkInstance inst) {
    g_d.GetInstanceProcAddr = Gipa();
    if (!g_d.GetInstanceProcAddr) return false;
    g_d.GetDeviceProcAddr = InstFn<PFN_vkGetDeviceProcAddr>(inst, "vkGetDeviceProcAddr");
    g_d.EnumeratePhysicalDevices = InstFn<PFN_vkEnumeratePhysicalDevices>(inst, "vkEnumeratePhysicalDevices");
    g_d.GetPhysicalDeviceProperties2 = InstFn<PFN_vkGetPhysicalDeviceProperties2>(
        inst, "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR");
    g_d.GetPhysicalDeviceMemoryProperties = InstFn<PFN_vkGetPhysicalDeviceMemoryProperties>(inst, "vkGetPhysicalDeviceMemoryProperties");
    return g_d.GetDeviceProcAddr && g_d.EnumeratePhysicalDevices && g_d.GetPhysicalDeviceProperties2;
}

static bool LoadDeviceLevel(VkDevice dev) {
    g_d.GetDeviceQueue = DevFn<PFN_vkGetDeviceQueue>(dev, "vkGetDeviceQueue");
    g_d.DeviceWaitIdle = DevFn<PFN_vkDeviceWaitIdle>(dev, "vkDeviceWaitIdle");
    g_d.QueueWaitIdle = DevFn<PFN_vkQueueWaitIdle>(dev, "vkQueueWaitIdle");
    g_d.QueueSubmit = DevFn<PFN_vkQueueSubmit>(dev, "vkQueueSubmit");
    g_d.CreateImage = DevFn<PFN_vkCreateImage>(dev, "vkCreateImage");
    g_d.DestroyImage = DevFn<PFN_vkDestroyImage>(dev, "vkDestroyImage");
    g_d.GetImageMemoryRequirements2 = DevFn<PFN_vkGetImageMemoryRequirements2>(
        dev, "vkGetImageMemoryRequirements2", "vkGetImageMemoryRequirements2KHR");
    g_d.BindImageMemory2 = DevFn<PFN_vkBindImageMemory2>(dev, "vkBindImageMemory2", "vkBindImageMemory2KHR");
    g_d.AllocateMemory = DevFn<PFN_vkAllocateMemory>(dev, "vkAllocateMemory");
    g_d.FreeMemory = DevFn<PFN_vkFreeMemory>(dev, "vkFreeMemory");
    g_d.GetMemoryWin32HandlePropertiesKHR = DevFn<PFN_vkGetMemoryWin32HandlePropertiesKHR>(dev, "vkGetMemoryWin32HandlePropertiesKHR");
    g_d.CreateSemaphore = DevFn<PFN_vkCreateSemaphore>(dev, "vkCreateSemaphore");
    g_d.DestroySemaphore = DevFn<PFN_vkDestroySemaphore>(dev, "vkDestroySemaphore");
    g_d.ImportSemaphoreWin32HandleKHR = DevFn<PFN_vkImportSemaphoreWin32HandleKHR>(dev, "vkImportSemaphoreWin32HandleKHR");
    g_d.CreateCommandPool = DevFn<PFN_vkCreateCommandPool>(dev, "vkCreateCommandPool");
    g_d.DestroyCommandPool = DevFn<PFN_vkDestroyCommandPool>(dev, "vkDestroyCommandPool");
    g_d.AllocateCommandBuffers = DevFn<PFN_vkAllocateCommandBuffers>(dev, "vkAllocateCommandBuffers");
    g_d.ResetCommandPool = DevFn<PFN_vkResetCommandPool>(dev, "vkResetCommandPool");
    g_d.BeginCommandBuffer = DevFn<PFN_vkBeginCommandBuffer>(dev, "vkBeginCommandBuffer");
    g_d.EndCommandBuffer = DevFn<PFN_vkEndCommandBuffer>(dev, "vkEndCommandBuffer");
    g_d.CmdPipelineBarrier = DevFn<PFN_vkCmdPipelineBarrier>(dev, "vkCmdPipelineBarrier");

    const void* required[] = {
        (const void*)g_d.GetDeviceQueue, (const void*)g_d.QueueSubmit, (const void*)g_d.CreateImage,
        (const void*)g_d.DestroyImage, (const void*)g_d.GetImageMemoryRequirements2,
        (const void*)g_d.BindImageMemory2, (const void*)g_d.AllocateMemory, (const void*)g_d.FreeMemory,
        (const void*)g_d.GetMemoryWin32HandlePropertiesKHR, (const void*)g_d.CreateCommandPool,
        (const void*)g_d.AllocateCommandBuffers, (const void*)g_d.BeginCommandBuffer,
        (const void*)g_d.EndCommandBuffer, (const void*)g_d.CmdPipelineBarrier,
    };
    for (const void* p : required) if (!p) return false;
    return true;
}

// --- formats ------------------------------------------------------------------------------
// The pairing is the DXGI format the shared resource is created with. Colour keeps the
// typed sRGB/UNORM format (the projection binding selects a UNORM sampling view, so
// the encoded bytes pass through unchanged); depth goes typeless, which is both what a DSV
// wants and what BetterVR's own shared depth textures use.
struct FormatPair { int64_t vk; DXGI_FORMAT typed; DXGI_FORMAT resource; bool depth; };

static const FormatPair kFormats[] = {
    { VK_FORMAT_R8G8B8A8_SRGB,            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false },
    { VK_FORMAT_B8G8R8A8_SRGB,            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false },
    { VK_FORMAT_R8G8B8A8_UNORM,           DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_R8G8B8A8_UNORM,      false },
    { VK_FORMAT_B8G8R8A8_UNORM,           DXGI_FORMAT_B8G8R8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM,      false },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_FORMAT_R10G10B10A2_UNORM,   false },
    { VK_FORMAT_R16G16B16A16_SFLOAT,      DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_FORMAT_R16G16B16A16_FLOAT,  false },
    { VK_FORMAT_D32_SFLOAT,               DXGI_FORMAT_D32_FLOAT,           DXGI_FORMAT_R32_TYPELESS,        true  },
    { VK_FORMAT_D24_UNORM_S8_UINT,        DXGI_FORMAT_D24_UNORM_S8_UINT,   DXGI_FORMAT_R24G8_TYPELESS,      true  },
    { VK_FORMAT_D32_SFLOAT_S8_UINT,       DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_R32G8X24_TYPELESS,   true  },
    { VK_FORMAT_D16_UNORM,                DXGI_FORMAT_D16_UNORM,           DXGI_FORMAT_R16_TYPELESS,        true  },
};

static const FormatPair* FindFormat(int64_t vkFormat) {
    for (const auto& f : kFormats) if (f.vk == vkFormat) return &f;
    return nullptr;
}

// --- adapter matching ---------------------------------------------------------------------

static bool PreferredAdapterLuid(LUID& out) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(f.GetAddressOf())))) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapters1(i, a.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        out = d.AdapterLuid;
        return true;
    }
    return false;
}

static bool PhysicalDeviceLuid(VkPhysicalDevice pd, LUID& out) {
    if (!g_d.GetPhysicalDeviceProperties2) return false;
    VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props.pNext = &id;
    g_d.GetPhysicalDeviceProperties2(pd, &props);
    if (!id.deviceLUIDValid) return false;
    memcpy(&out, id.deviceLUID, sizeof(LUID));
    return true;
}

// The VkPhysicalDevice for the adapter the compositor will run on. Matching the LUID is
// what makes the shared-handle import legal: a D3D12 resource can only be opened by a
// Vulkan device on the same physical adapter.
static XrResult PickPhysicalDevice(VkInstance vkInstance, VkPhysicalDevice* out) {
    if (!vkInstance || !out) return XR_ERROR_VALIDATION_FAILURE;
    g_lastInstance = vkInstance;
    if (!LoadInstanceLevel(vkInstance)) {
        Log("[SimXR][VK] could not resolve the instance-level entry points");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    uint32_t count = 0;
    if (g_d.EnumeratePhysicalDevices(vkInstance, &count, nullptr) != VK_SUCCESS || count == 0) {
        Log("[SimXR][VK] vkEnumeratePhysicalDevices returned nothing");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (g_d.EnumeratePhysicalDevices(vkInstance, &count, devices.data()) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;

    LUID want{};
    const bool haveWant = PreferredAdapterLuid(want);
    for (VkPhysicalDevice pd : devices) {
        LUID got{};
        if (!PhysicalDeviceLuid(pd, got)) continue;
        Logf("[SimXR][VK] physical device %p LUID=%08lX:%08lX", (void*)pd,
             (unsigned long)got.HighPart, (unsigned long)got.LowPart);
        if (haveWant && got.LowPart == want.LowPart && got.HighPart == want.HighPart) {
            *out = pd;
            Logf("[SimXR][VK] matched the compositor's adapter LUID=%08lX:%08lX",
                 (unsigned long)want.HighPart, (unsigned long)want.LowPart);
            return XR_SUCCESS;
        }
    }
    *out = devices[0];
    Log("[SimXR][VK] no physical device matched the compositor's adapter LUID; using the first one");
    return XR_SUCCESS;
}

// --- extension injection ------------------------------------------------------------------

static const char* kInstanceExtensions[] = {
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};

static const char* kDeviceExtensions[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
};

static std::string JoinExtensions(const char* const* names, size_t count) {
    std::string s;
    for (size_t i = 0; i < count; ++i) { if (i) s += ' '; s += names[i]; }
    return s;
}

// Merge the runtime's requirements into the app's list, skipping anything the app already
// asked for and anything the driver does not advertise -- a promoted extension is core on
// a 1.1+ instance and may be missing from the enumeration, in which case enabling it by
// name is an error rather than a no-op.
static std::vector<const char*> MergeExtensions(const char* const* wanted, size_t wantedCount,
                                                const char* const* appNames, uint32_t appCount,
                                                const std::vector<std::string>& available,
                                                std::vector<std::string>& storage) {
    std::vector<const char*> out;
    for (uint32_t i = 0; i < appCount; ++i) out.push_back(appNames[i]);
    for (size_t i = 0; i < wantedCount; ++i) {
        bool already = false;
        for (const char* n : out) if (strcmp(n, wanted[i]) == 0) { already = true; break; }
        if (already) continue;
        bool supported = false;
        for (const auto& a : available) if (a == wanted[i]) { supported = true; break; }
        if (!supported) { Logf("[SimXR][VK] %s is not advertised; not injecting it", wanted[i]); continue; }
        storage.emplace_back(wanted[i]);
        Logf("[SimXR][VK] injecting %s", wanted[i]);
    }
    for (const auto& s : storage) out.push_back(s.c_str());
    return out;
}

// --- session ------------------------------------------------------------------------------

static VkImageAspectFlags AspectOf(const FormatPair& fp) {
    if (!fp.depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    if (fp.vk == VK_FORMAT_D24_UNORM_S8_UINT || fp.vk == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT;
}

// The compositor's own D3D12 device. It has to sit on the same physical adapter as the
// app's VkPhysicalDevice or every CreateSharedHandle import fails.
static bool CreateCompositorDevice(rt::Session& s) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    LUID want{};
    const bool haveWant = PhysicalDeviceLuid(s.vk.physicalDevice, want);
    ComPtr<IDXGIAdapter1> chosen, firstHardware;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!firstHardware) firstHardware = a;
        if (haveWant && d.AdapterLuid.LowPart == want.LowPart && d.AdapterLuid.HighPart == want.HighPart) {
            chosen = a;
            break;
        }
    }
    if (!chosen) {
        chosen = firstHardware;
        if (haveWant) Log("[SimXR][VK] the app's VkPhysicalDevice LUID matched no DXGI adapter; interop may fail");
    }
    if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(s.d3d12Device.GetAddressOf())))) {
        Log("[SimXR][VK] D3D12CreateDevice for the compositor failed");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s.d3d12Device->CreateCommandQueue(&qd, IID_PPV_ARGS(s.d3d12Queue.GetAddressOf())))) {
        Log("[SimXR][VK] the compositor's D3D12 command queue could not be created");
        s.d3d12Device.Reset();
        return false;
    }
    if (chosen) {
        DXGI_ADAPTER_DESC1 d{}; chosen->GetDesc1(&d);
        char name[128]{};
        wcstombs(name, d.Description, sizeof(name) - 1);
        Logf("[SimXR][VK] compositor D3D12 device on %s (LUID %08lX:%08lX)", name,
             (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart);
    }
    return true;
}

// One shared D3D12 fence imported as a timeline VkSemaphore. Without it the session still
// works, on CPU waits instead (see FrameSyncBegin).
static void InitFrameSync(rt::Session& s) {
    s.vk.timeline = false;
    char noTimeline[8]{};
    if (GetEnvironmentVariableA("SIMXR_VK_NO_TIMELINE", noTimeline, (DWORD)sizeof(noTimeline)) > 0 && noTimeline[0] != '0') {
        Log("[SimXR][VK] SIMXR_VK_NO_TIMELINE set; preview unavailable without external timeline synchronization");
        return;
    }
    if (!g_d.CreateSemaphore || !g_d.ImportSemaphoreWin32HandleKHR || !g_d.QueueSubmit) {
        Log("[SimXR][VK] no external-semaphore entry points; preview unavailable without external timeline synchronization");
        return;
    }
    if (FAILED(s.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(s.vk.fence.GetAddressOf())))) {
        Log("[SimXR][VK] CreateFence(SHARED) failed; preview unavailable without external timeline synchronization");
        return;
    }
    if (FAILED(s.d3d12Device->CreateSharedHandle(s.vk.fence.Get(), nullptr, GENERIC_ALL, nullptr, &s.vk.fenceHandle))) {
        Log("[SimXR][VK] CreateSharedHandle(fence) failed; preview unavailable without external timeline synchronization");
        s.vk.fence.Reset();
        return;
    }

    VkSemaphoreTypeCreateInfo timelineInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semInfo.pNext = &timelineInfo;
    if (g_d.CreateSemaphore(s.vk.device, &semInfo, nullptr, &s.vk.semaphore) != VK_SUCCESS) {
        Log("[SimXR][VK] timeline VkSemaphore creation failed; preview unavailable without external timeline synchronization");
        s.vk.semaphore = VK_NULL_HANDLE;
        return;
    }
    VkImportSemaphoreWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    import.semaphore = s.vk.semaphore;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    import.handle = s.vk.fenceHandle;
    const VkResult r = g_d.ImportSemaphoreWin32HandleKHR(s.vk.device, &import);
    if (r != VK_SUCCESS) {
        Logf("[SimXR][VK] ImportSemaphoreWin32HandleKHR failed (%d); preview unavailable without external timeline synchronization", (int)r);
        g_d.DestroySemaphore(s.vk.device, s.vk.semaphore, nullptr);
        s.vk.semaphore = VK_NULL_HANDLE;
        return;
    }
    s.vk.timeline = true;
    Log("[SimXR][VK] frame sync: shared ID3D12Fence imported as a timeline VkSemaphore");
}

static bool InitSession(rt::Session& s, const XrGraphicsBindingVulkanKHR& b) {
    s.vk = rt::VulkanSession{};
    s.vk.physicalDevice = b.physicalDevice;
    s.vk.device = b.device;

    if (!LoadInstanceLevel(b.instance)) { Log("[SimXR][VK] instance-level entry points missing"); return false; }
    if (!LoadDeviceLevel(b.device)) { Log("[SimXR][VK] device-level entry points missing (external memory not enabled?)"); return false; }
    g_d.GetDeviceQueue(b.device, b.queueFamilyIndex, b.queueIndex, &s.vk.queue);
    if (!s.vk.queue) { Log("[SimXR][VK] vkGetDeviceQueue returned nothing"); return false; }

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = b.queueFamilyIndex;
    if (g_d.CreateCommandPool(b.device, &pci, nullptr, &s.vk.cmdPool) != VK_SUCCESS) {
        Log("[SimXR][VK] vkCreateCommandPool failed");
        return false;
    }
    VkCommandBufferAllocateInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s.vk.cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (g_d.AllocateCommandBuffers(b.device, &cbi, &s.vk.cmdBuffer) != VK_SUCCESS) {
        Log("[SimXR][VK] vkAllocateCommandBuffers failed");
        return false;
    }
    if (!CreateCompositorDevice(s)) return false;
    InitFrameSync(s);
    Logf("[SimXR][VK] session bound to VkDevice %p, queue family %u index %u",
         (void*)b.device, b.queueFamilyIndex, b.queueIndex);
    return true;
}

static void ShutdownSession(rt::Session& s) {
    if (s.vk.device) {
        if (g_d.DeviceWaitIdle) g_d.DeviceWaitIdle(s.vk.device);
        if (s.vk.semaphore && g_d.DestroySemaphore) g_d.DestroySemaphore(s.vk.device, s.vk.semaphore, nullptr);
        if (s.vk.cmdPool && g_d.DestroyCommandPool) g_d.DestroyCommandPool(s.vk.device, s.vk.cmdPool, nullptr);
    }
    if (s.vk.fenceHandle) CloseHandle(s.vk.fenceHandle);
    s.vk = rt::VulkanSession{};
    s.usesVulkan = false;
}

// --- swapchain images ---------------------------------------------------------------------

static void DestroySwapchainImages(rt::Session& s, rt::Swapchain& chain) {
    if (s.vk.device && g_d.DestroyImage && g_d.FreeMemory) {
        if (g_d.DeviceWaitIdle) g_d.DeviceWaitIdle(s.vk.device);
        for (VkImage img : chain.imagesVk) if (img) g_d.DestroyImage(s.vk.device, img, nullptr);
        for (VkDeviceMemory mem : chain.memoryVk) if (mem) g_d.FreeMemory(s.vk.device, mem, nullptr);
    }
    for (HANDLE h : chain.sharedHandles) if (h) CloseHandle(h);
    chain.imagesVk.clear();
    chain.memoryVk.clear();
    chain.sharedHandles.clear();
}

// Hand the images over in the layouts XR_KHR_vulkan_enable mandates -- colour in
// COLOR_ATTACHMENT_OPTIMAL, depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL -- and leave them
// there for the rest of the session. A conforming app renders straight into an acquired
// image with no barrier of its own, so anything else is a layout the app never corrects.
static bool TransitionToRequiredLayout(rt::Session& s, rt::Swapchain& chain, const FormatPair& fp) {
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_d.ResetCommandPool) g_d.ResetCommandPool(s.vk.device, s.vk.cmdPool, 0);
    if (g_d.BeginCommandBuffer(s.vk.cmdBuffer, &bi) != VK_SUCCESS) return false;

    std::vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(chain.imagesVk.size());
    for (VkImage img : chain.imagesVk) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0;
        b.dstAccessMask = fp.depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = fp.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange.aspectMask = AspectOf(fp);
        b.subresourceRange.levelCount = chain.mipCount ? chain.mipCount : 1;
        b.subresourceRange.layerCount = chain.arraySize ? chain.arraySize : 1;
        barriers.push_back(b);
    }
    g_d.CmdPipelineBarrier(s.vk.cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           fp.depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                    : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, 0, nullptr, 0, nullptr, (uint32_t)barriers.size(), barriers.data());
    if (g_d.EndCommandBuffer(s.vk.cmdBuffer) != VK_SUCCESS) return false;

    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.vk.cmdBuffer;
    if (g_d.QueueSubmit(s.vk.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    if (g_d.QueueWaitIdle) g_d.QueueWaitIdle(s.vk.queue);
    return true;
}

static XrResult CreateSwapchainImages(rt::Session& s, rt::Swapchain& chain, const XrSwapchainCreateInfo& ci) {
    const FormatPair* fp = FindFormat(ci.format);
    if (!fp) {
        Logf("[SimXR][VK] xrCreateSwapchain: VkFormat %lld is not one this runtime offers", (long long)ci.format);
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }

    const uint32_t arraySize = chain.arraySize ? chain.arraySize : 1;
    const uint32_t mips = ci.mipCount ? ci.mipCount : 1;
    const uint32_t samples = ci.sampleCount ? ci.sampleCount : 1;
    chain.isVulkan = true;
    chain.format = fp->typed;
    chain.backend = rt::Swapchain::Backend::D3D12;
    chain.mipCount = mips;
    chain.imageCount = (ci.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : 3;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // Some drivers need this spelled out on a shared committed resource; 0 (let the runtime pick)
    // produces a resource whose Vulkan import fails.
    rd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    rd.Width = chain.width;
    rd.Height = chain.height;
    rd.DepthOrArraySize = (UINT16)arraySize;
    rd.MipLevels = (UINT16)mips;
    rd.Format = fp->resource;
    rd.SampleDesc.Count = samples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (fp->depth) {
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    } else {
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // ALLOW_SIMULTANEOUS_ACCESS turns off DCC, which is what makes the bytes Vulkan
        // wrote through a COLOR_ATTACHMENT_OPTIMAL layout readable by a D3D12 SRV. It is
        // illegal on depth and on MSAA resources.
        if (samples == 1) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ci.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ci.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (ci.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ci.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ci.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        usage |= fp->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    VkPhysicalDeviceMemoryProperties memProps{};
    if (g_d.GetPhysicalDeviceMemoryProperties) g_d.GetPhysicalDeviceMemoryProperties(s.vk.physicalDevice, &memProps);

    for (uint32_t i = 0; i < chain.imageCount; ++i) {
        ComPtr<ID3D12Resource> res;
        HRESULT hr = s.d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(res.GetAddressOf()));
        if (FAILED(hr)) {
            Logf("[SimXR][VK] CreateCommittedResource(SHARED)[%u] failed 0x%08X", i, (unsigned)hr);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        HANDLE shared = nullptr;
        hr = s.d3d12Device->CreateSharedHandle(res.Get(), nullptr, GENERIC_ALL, nullptr, &shared);
        if (FAILED(hr)) {
            Logf("[SimXR][VK] CreateSharedHandle[%u] failed 0x%08X", i, (unsigned)hr);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        chain.images12.push_back(res);
        chain.imageStates12.push_back(D3D12_RESOURCE_STATE_COMMON);
        chain.sharedHandles.push_back(shared);

        VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.pNext = &external;
        ici.flags = (ci.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = (VkFormat)ci.format;
        ici.extent = { chain.width, chain.height, 1 };
        ici.mipLevels = mips;
        ici.arrayLayers = arraySize;
        ici.samples = (VkSampleCountFlagBits)samples;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        VkResult vr = g_d.CreateImage(s.vk.device, &ici, nullptr, &image);
        if (vr != VK_SUCCESS) {
            Logf("[SimXR][VK] vkCreateImage[%u] failed (%d)", i, (int)vr);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        chain.imagesVk.push_back(image);

        VkImageMemoryRequirementsInfo2 reqInfo{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
        reqInfo.image = image;
        VkMemoryRequirements2 req{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        g_d.GetImageMemoryRequirements2(s.vk.device, &reqInfo, &req);

        VkMemoryWin32HandlePropertiesKHR handleProps{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        vr = g_d.GetMemoryWin32HandlePropertiesKHR(s.vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                                   shared, &handleProps);
        if (vr != VK_SUCCESS) {
            Logf("[SimXR][VK] vkGetMemoryWin32HandlePropertiesKHR[%u] failed (%d)", i, (int)vr);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        uint32_t typeIndex = UINT32_MAX;
        const uint32_t bits = handleProps.memoryTypeBits & req.memoryRequirements.memoryTypeBits;
        for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
            if (!(bits & (1u << t))) continue;
            if (memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { typeIndex = t; break; }
        }
        if (typeIndex == UINT32_MAX) {
            for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
                if (handleProps.memoryTypeBits & (1u << t)) { typeIndex = t; break; }
            }
        }
        if (typeIndex == UINT32_MAX) {
            Logf("[SimXR][VK] no importable memory type for image %u", i);
            return XR_ERROR_RUNTIME_FAILURE;
        }

        VkImportMemoryWin32HandleInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        importInfo.handle = shared;
        VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.pNext = &importInfo;
        dedicated.image = image;
        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext = &dedicated;
        alloc.allocationSize = req.memoryRequirements.size;
        alloc.memoryTypeIndex = typeIndex;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        vr = g_d.AllocateMemory(s.vk.device, &alloc, nullptr, &memory);
        if (vr != VK_SUCCESS) {
            Logf("[SimXR][VK] vkAllocateMemory(import)[%u] failed (%d)", i, (int)vr);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        chain.memoryVk.push_back(memory);

        VkBindImageMemoryInfo bind{ VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO };
        bind.image = image;
        bind.memory = memory;
        vr = g_d.BindImageMemory2(s.vk.device, 1, &bind);
        if (vr != VK_SUCCESS) {
            Logf("[SimXR][VK] vkBindImageMemory2[%u] failed (%d)", i, (int)vr);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

    // COMMON is where the D3D12 half of a shared resource lives between accesses, and the
    // preview transitions out of and back into it around every read.
    chain.releaseState12 = D3D12_RESOURCE_STATE_COMMON;
    if (!TransitionToRequiredLayout(s, chain, *fp)) {
        Log("[SimXR][VK] could not put the swapchain images in their required layout");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    Logf("[SimXR][VK] swapchain: %u shared images %ux%u array=%u VkFormat=%lld -> DXGI %d, layout %s",
         chain.imageCount, chain.width, chain.height, arraySize, (long long)ci.format, (int)fp->resource,
         fp->depth ? "DEPTH_STENCIL_ATTACHMENT_OPTIMAL" : "COLOR_ATTACHMENT_OPTIMAL");
    return XR_SUCCESS;
}

// --- frame sync ---------------------------------------------------------------------------
// Runs at the top of xrEndFrame: everything the app submitted for this frame is ahead of
// this signal on its queue, so the preview queue waiting on it is waiting on the frame.
static void FrameSyncBegin(rt::Session& s) {
    if (!s.usesVulkan || !s.vk.queue) return;
    if (s.vk.timeline) {
        const uint64_t value = ++s.vk.counter;
        VkTimelineSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        tsi.signalSemaphoreValueCount = 1;
        tsi.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.pNext = &tsi;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s.vk.semaphore;
        if (g_d.QueueSubmit(s.vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) s.vk.appSignalled = value;
        LogVf("[SimXR][VK] sync: app signals %llu (fence at %llu)", (unsigned long long)value,
              (unsigned long long)(s.vk.fence ? s.vk.fence->GetCompletedValue() : 0));
        return;
    }
    // Without a shared timeline the private compositor cannot order source
    // reuse. This optional preview is disabled rather than serializing the app
    // through CPU queue-idle waits or presenting an unsafe source.
    return;
}

// Runs after the composite has been submitted: the app's queue is made to wait for the
// preview's read before anything it submits next can overwrite those images.
static void FrameSyncEnd(rt::Session& s) {
    if (!s.usesVulkan || !s.vk.timeline || !s.vk.queue) return;
    if (s.vk.previewSignalled == s.vk.previewWaited) return;
    const uint64_t value = s.vk.previewSignalled;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tsi.waitSemaphoreValueCount = 1;
    tsi.pWaitSemaphoreValues = &value;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &tsi;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &s.vk.semaphore;
    si.pWaitDstStageMask = &stage;
    if (g_d.QueueSubmit(s.vk.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) s.vk.previewWaited = value;
    LogVf("[SimXR][VK] sync: app waits for %llu (fence at %llu)", (unsigned long long)value,
          (unsigned long long)(s.vk.fence ? s.vk.fence->GetCompletedValue() : 0));
}

} // namespace vkrt

static constexpr XrVersion SupportedApiVersion = XR_MAKE_VERSION(1, 0, 27);

static const simxr::ExtensionRegistry& Extensions()
{
	static constexpr std::array<std::string_view, 1> d3d11Commands{"xrGetD3D11GraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 1> d3d12Commands{"xrGetD3D12GraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 1> openGLCommands{"xrGetOpenGLGraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 4> vulkanCommands{
		"xrGetVulkanInstanceExtensionsKHR", "xrGetVulkanDeviceExtensionsKHR",
		"xrGetVulkanGraphicsDeviceKHR", "xrGetVulkanGraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 4> vulkan2Commands{
		"xrCreateVulkanInstanceKHR", "xrCreateVulkanDeviceKHR",
		"xrGetVulkanGraphicsDevice2KHR", "xrGetVulkanGraphicsRequirements2KHR"};
	static constexpr std::array<std::string_view, 2> timeCommands{
		"xrConvertWin32PerformanceCounterToTimeKHR", "xrConvertTimeToWin32PerformanceCounterKHR"};
	static const std::vector<simxr::ExtensionDefinition> definitions = [] {
        std::vector<simxr::ExtensionDefinition> result{
		{XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D11_enable_SPEC_VERSION, d3d11Commands},
		{XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_enable_SPEC_VERSION, d3d12Commands},
		{XR_KHR_OPENGL_ENABLE_EXTENSION_NAME, XR_KHR_opengl_enable_SPEC_VERSION, openGLCommands},
		{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION, vulkanCommands},
		{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION, vulkan2Commands},
		{XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION, {}},
		{XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
			XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION, timeCommands}};
        const auto simulation = simxr::SimulationExtensions();
        result.insert(result.end(), simulation.begin(), simulation.end());
        return result;
    }();
	static const simxr::ExtensionRegistry registry(definitions);
	return registry;
}

static simxr::Runtime& RuntimeState()
{
    static simxr::Runtime runtime(Extensions());
    return runtime;
}

struct NativeControlEndpoint
{
    std::mutex mutex;
    std::unique_ptr<simxr::control::ControlService> service;
    std::unique_ptr<simxr::control::LocalControlServer> server;
    bool permanentlyClosed = false;
    ~NativeControlEndpoint() { Close(); }
    bool Start()
    {
        std::lock_guard lock(mutex);
        if (permanentlyClosed) return false;
        if (server) return true;
        try {
            const auto* selected = std::getenv("SIMXR_CONTROL_SOCKET");
            const auto path = selected && *selected ? std::string(selected)
                : std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId());
            auto nextService = std::make_unique<simxr::control::ControlService>(RuntimeState(), 16);
            auto nextServer = std::make_unique<simxr::control::LocalControlServer>(*nextService, path, 16);
            service = std::move(nextService);
            server = std::move(nextServer);
            Logf("[SimXR] Local control endpoint: %s (CPU control/inspection; native GPU transport unavailable)", path.c_str());
            return true;
        }
        catch (const std::exception& error) { Logf("[SimXR] Local control endpoint unavailable: %s", error.what()); return false; }
    }
    void Close(bool permanent = false) noexcept
    {
        std::unique_ptr<simxr::control::ControlService> previousService;
        std::unique_ptr<simxr::control::LocalControlServer> previousServer;
        {
            std::lock_guard lock(mutex);
            permanentlyClosed = permanentlyClosed || permanent;
            previousServer = std::move(server); previousService = std::move(service);
        }
        // No runtime/native lock spans transport joins or lease/input cleanup.
        if (previousServer) previousServer->Close();
        if (previousService) previousService->Close();
    }
};
static NativeControlEndpoint& ControlEndpoint()
{
    // Construct after RuntimeState so endpoint workers are joined first.
    (void)RuntimeState();
    static NativeControlEndpoint endpoint;
    return endpoint;
}
extern "C" __declspec(dllexport) int simxrGetGameBridgeApi(std::uint32_t version, SimxrGameBridgeApi* api) noexcept
{
    if (!api || version != SIMXR_GAME_BRIDGE_VERSION || api->size < sizeof(*api)) return 0;
    try {
        if (!ControlEndpoint().Start()) return 0;
        if (!RuntimeState().Game().FillApi(version, *api)) return 0;
        api->shutdown = [](void*) -> int {
            try {
                if (!RuntimeState().Instances().empty()) return 0;
                // The caller stops XR initialization before explicit shutdown.
                // Runtime.Close permanently rejects new instances; native joins
                // run here, outside DLL detach and outside runtime graph locks.
                RuntimeState().Close();
                ControlEndpoint().Close(true);
                return 1;
            }
            catch (...) { return 0; }
        };
        return 1;
    }
    catch (...) { return 0; }
}

static XrResult RequireExtension(XrInstance instance, std::string_view extension)
{
	if (!RuntimeState().FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (!rt::g_instance.core || !rt::g_instance.core->Extensions().Contains(extension))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return XR_SUCCESS;
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
static XrResult DestroyNativeSession(XrSession session);
static void ConsumeCompletedPreviewFrame(rt::Session& session);
static XrResult XRAPI_PTR xrDestroySwapchain_runtime(XrSwapchain swapchain);

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                            XrNegotiateRuntimeRequest* runtimeRequest) {
    try {
        Log("\n[SimXR] ========== OpenXR Simulator Runtime Starting ==========\n");
        if (!loaderInfo || !runtimeRequest) {
            Log("[SimXR] xrNegotiateLoaderRuntimeInterface: ERROR - null parameters");
            return XR_ERROR_INITIALIZATION_FAILED;
        }
        
        // The loader FreeLibrary's the runtime after xrDestroyInstance. That drops the last
        // reference this process may hold on d3d12/dxgi/opengl32, and unloading those out
        // from under a live Vulkan driver - the same driver, in a Vulkan session - hangs the
        // process. Pinning also settles the older hazard of a preview window
        // outliving its WndProc.
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCWSTR)&xrNegotiateLoaderRuntimeInterface, &self);
        }

        Logf("[SimXR] xrNegotiateLoaderRuntimeInterface: loaderInfo=%p, runtimeRequest=%p", loaderInfo, runtimeRequest);
        Logf("[SimXR]   Loader minInterfaceVersion=%u, maxInterfaceVersion=%u, minApiVersion=0x%X, maxApiVersion=0x%X",
             loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion,
             loaderInfo->minApiVersion, loaderInfo->maxApiVersion);
        
        if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
            loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loaderInfo->structSize != sizeof(*loaderInfo) ||
            runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
            runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || runtimeRequest->structSize != sizeof(*runtimeRequest) ||
            loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
            loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION ||
            loaderInfo->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loaderInfo->minApiVersion > SupportedApiVersion)
            return XR_ERROR_INITIALIZATION_FAILED;

        runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        runtimeRequest->getInstanceProcAddr = simxr::AbiBoundary<xrGetInstanceProcAddr_runtime>::Invoke;
        runtimeRequest->runtimeApiVersion = std::min(loaderInfo->maxApiVersion, SupportedApiVersion);
        
        Logf("[SimXR] xrNegotiateLoaderRuntimeInterface: SUCCESS - runtimeApiVersion=0x%X (%u)", 
             runtimeRequest->runtimeApiVersion, runtimeRequest->runtimeApiVersion);
        return XR_SUCCESS;
    } catch (...) {
        Log("[SimXR] xrNegotiateLoaderRuntimeInterface: EXCEPTION caught!");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
}

// xrGetD3D12GraphicsRequirementsKHR (XR_KHR_D3D12_enable)
static XrResult XRAPI_PTR xrGetD3D12GraphicsRequirementsKHR_runtime(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* req) {
    auto result = RequireGraphicsExtension(instance, systemId, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;

    Logf("[SimXR] xrGetD3D12GraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p",
         instance, (unsigned long long)systemId, req);
    if (!req) return XR_ERROR_VALIDATION_FAILURE;

    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;

    req->adapterLuid = {};
    Microsoft::WRL::ComPtr<IDXGIFactory1> f;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(f.GetAddressOf()));
    if (FAILED(hr)) return XR_ERROR_RUNTIME_FAILURE;

    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapters1(i, a.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        req->adapterLuid = d.AdapterLuid;
        break;
    }
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    Log("[SimXR] xrGetD3D12GraphicsRequirementsKHR: SUCCESS");
    return XR_SUCCESS;
}
// xrGetD3D11GraphicsRequirementsKHR (XR_KHR_D3D11_enable)
static XrResult XRAPI_PTR xrGetD3D11GraphicsRequirementsKHR_runtime(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D11KHR* req) {
    auto result = RequireGraphicsExtension(instance, systemId, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;

    Logf("[SimXR] xrGetD3D11GraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p",
         instance, (unsigned long long)systemId, req);
    if (!req) {
        Log("[SimXR] xrGetD3D11GraphicsRequirementsKHR: ERROR - null req");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    
    Logf("[SimXR] xrGetD3D11GraphicsRequirementsKHR: req struct size = %zu, expected = %zu",
         sizeof(*req), sizeof(XrGraphicsRequirementsD3D11KHR));
    
    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    
    req->adapterLuid = {};
    Microsoft::WRL::ComPtr<IDXGIFactory1> f;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(f.GetAddressOf()));
    if (FAILED(hr)) { 
        Logf("[SimXR] CreateDXGIFactory1 failed: 0x%08X", hr); 
        return XR_ERROR_RUNTIME_FAILURE; 
    }
    
    Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
    DXGI_ADAPTER_DESC1 bestDesc{};
    bool foundHardware = false;
    
    // Find the best hardware adapter
    for (UINT i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapt;
        if (f->EnumAdapters1(i, adapt.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;
            
        DXGI_ADAPTER_DESC1 d{}; 
        adapt->GetDesc1(&d);
        
        // Skip software adapters
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) 
            continue;
            
        // Use the first hardware adapter we find
        if (!foundHardware) {
            bestAdapter = adapt;
            bestDesc = d;
            foundHardware = true;
            
            wchar_t* descStr = d.Description;
            char descAscii[128];
            wcstombs(descAscii, descStr, sizeof(descAscii));
            descAscii[sizeof(descAscii)-1] = '\0';
            Logf("[SimXR] Found hardware adapter: %s", descAscii);
            Logf("[SimXR]   LUID: High=%ld, Low=%lu", 
                 (long)d.AdapterLuid.HighPart, 
                 (unsigned long)d.AdapterLuid.LowPart);
            Logf("[SimXR]   Dedicated Video Memory: %llu MB", 
                 (unsigned long long)(d.DedicatedVideoMemory / (1024*1024)));
        }
    }
    
    if (foundHardware) {
        req->adapterLuid = bestDesc.AdapterLuid;
        req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        
        // Save this LUID for later validation
        rt::g_adapterLuid = bestDesc.AdapterLuid;
        rt::g_adapterLuidSet = true;
        
        Logf("[SimXR] xrGetD3D11GraphicsRequirementsKHR: Returning:");
        Logf("[SimXR]   type = %d (expected %d)", req->type, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR);
        Logf("[SimXR]   next = %p", req->next);
        Logf("[SimXR]   adapterLuid.HighPart = %ld (0x%08X)", 
             (long)req->adapterLuid.HighPart, (unsigned)req->adapterLuid.HighPart);
        Logf("[SimXR]   adapterLuid.LowPart = %lu (0x%08X)", 
             (unsigned long)req->adapterLuid.LowPart, (unsigned)req->adapterLuid.LowPart);
        Logf("[SimXR]   minFeatureLevel = 0x%X (D3D_FEATURE_LEVEL_11_0 = 0x%X)", 
             req->minFeatureLevel, D3D_FEATURE_LEVEL_11_0);
        
        Log("[SimXR] xrGetD3D11GraphicsRequirementsKHR: SUCCESS - Returning XR_SUCCESS");
        return XR_SUCCESS;
    }
    
    // No hardware adapter found, this is an error for VR
    Log("[SimXR] xrGetD3D11GraphicsRequirementsKHR: ERROR - No hardware graphics adapter found");
    return XR_ERROR_SYSTEM_INVALID;
}

// xrGetOpenGLGraphicsRequirementsKHR (XR_KHR_opengl_enable)
static XrResult XRAPI_PTR xrGetOpenGLGraphicsRequirementsKHR_runtime(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLKHR* req) {
    auto result = RequireGraphicsExtension(instance, systemId, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR) return XR_ERROR_VALIDATION_FAILURE;

    Logf("[SimXR] xrGetOpenGLGraphicsRequirementsKHR called: instance=%p, systemId=%llu, req=%p",
         instance, (unsigned long long)systemId, req);
    if (!req) {
        Log("[SimXR] xrGetOpenGLGraphicsRequirementsKHR: ERROR - null req");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    req->type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;

    // Minimum OpenGL version: 4.0.0 (good compatibility)
    // Maximum: 4.6.0 (latest)
    req->minApiVersionSupported = XR_MAKE_VERSION(4, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);

    Logf("[SimXR] xrGetOpenGLGraphicsRequirementsKHR: min=%d.%d.%d, max=%d.%d.%d",
         XR_VERSION_MAJOR(req->minApiVersionSupported),
         XR_VERSION_MINOR(req->minApiVersionSupported),
         XR_VERSION_PATCH(req->minApiVersionSupported),
         XR_VERSION_MAJOR(req->maxApiVersionSupported),
         XR_VERSION_MINOR(req->maxApiVersionSupported),
         XR_VERSION_PATCH(req->maxApiVersionSupported));

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
	if (!requirements || requirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
	requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsRequirementsKHR_runtime(
	XrInstance instance, XrSystemId system, XrGraphicsRequirementsVulkanKHR* requirements)
{
	return GetVulkanRequirements(instance, system, requirements, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsRequirements2KHR_runtime(
	XrInstance instance, XrSystemId system, XrGraphicsRequirementsVulkan2KHR* requirements)
{
	return GetVulkanRequirements(instance, system, requirements, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
}

static XrResult CopyExtensionString(const std::string& s, uint32_t capacity, uint32_t* countOutput, char* buffer) {
    const uint32_t needed = (uint32_t)s.size() + 1;
    if (!countOutput || (capacity && !buffer)) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = needed;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < needed) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, s.c_str(), needed);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetVulkanInstanceExtensionsKHR_runtime(
    XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* countOutput, char* buffer) {
    auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;

    const std::string s = vkrt::JoinExtensions(vkrt::kInstanceExtensions, std::size(vkrt::kInstanceExtensions));
    if (capacity) Logf("[SimXR] xrGetVulkanInstanceExtensionsKHR: %s", s.c_str());
    return CopyExtensionString(s, capacity, countOutput, buffer);
}

static XrResult XRAPI_PTR xrGetVulkanDeviceExtensionsKHR_runtime(
    XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* countOutput, char* buffer) {
    auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;

    const std::string s = vkrt::JoinExtensions(vkrt::kDeviceExtensions, std::size(vkrt::kDeviceExtensions));
    if (capacity) Logf("[SimXR] xrGetVulkanDeviceExtensionsKHR: %s", s.c_str());
    return CopyExtensionString(s, capacity, countOutput, buffer);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsDeviceKHR_runtime(
    XrInstance instance, XrSystemId system, VkInstance vkInstance, VkPhysicalDevice* out) {
    auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    return vkrt::PickPhysicalDevice(vkInstance, out);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsDevice2KHR_runtime(
    XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* getInfo, VkPhysicalDevice* out) {
    auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    if (!getInfo || getInfo->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR) return XR_ERROR_VALIDATION_FAILURE;
    if (getInfo->systemId != 1) return XR_ERROR_SYSTEM_INVALID;
    return vkrt::PickPhysicalDevice(getInfo->vulkanInstance, out);
}

// Thin passthrough: the app's own vkCreateInstance runs, with the interop extensions the
// runtime needs appended to whatever it asked for.
static XrResult XRAPI_PTR xrCreateVulkanInstanceKHR_runtime(
    XrInstance instance, const XrVulkanInstanceCreateInfoKHR* createInfo, VkInstance* vulkanInstance, VkResult* vulkanResult) {
    auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    if (!createInfo || createInfo->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR) return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->systemId != 1) return XR_ERROR_SYSTEM_INVALID;

    if (!createInfo || !createInfo->pfnGetInstanceProcAddr || !createInfo->vulkanCreateInfo ||
        !vulkanInstance || !vulkanResult) return XR_ERROR_VALIDATION_FAILURE;

    vkrt::g_appGipa = createInfo->pfnGetInstanceProcAddr;
    auto enumerate = (PFN_vkEnumerateInstanceExtensionProperties)vkrt::g_appGipa(
        VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    auto create = (PFN_vkCreateInstance)vkrt::g_appGipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) { Log("[SimXR][VK] xrCreateVulkanInstanceKHR: no vkCreateInstance"); return XR_ERROR_RUNTIME_FAILURE; }

    std::vector<std::string> available;
    if (enumerate) {
        uint32_t n = 0;
        enumerate(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        if (n) enumerate(nullptr, &n, props.data());
        for (const auto& p : props) available.emplace_back(p.extensionName);
    }

    std::vector<std::string> storage;
    std::vector<const char*> names = vkrt::MergeExtensions(
        vkrt::kInstanceExtensions, std::size(vkrt::kInstanceExtensions),
        createInfo->vulkanCreateInfo->ppEnabledExtensionNames,
        createInfo->vulkanCreateInfo->enabledExtensionCount, available, storage);

    VkInstanceCreateInfo ici = *createInfo->vulkanCreateInfo;
    ici.enabledExtensionCount = (uint32_t)names.size();
    ici.ppEnabledExtensionNames = names.data();

    *vulkanResult = create(&ici, createInfo->vulkanAllocator, vulkanInstance);
    if (*vulkanResult == VK_SUCCESS) vkrt::g_lastInstance = *vulkanInstance;
    Logf("[SimXR] xrCreateVulkanInstanceKHR: %u extensions, VkResult=%d",
         ici.enabledExtensionCount, (int)*vulkanResult);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateVulkanDeviceKHR_runtime(
    XrInstance instance, const XrVulkanDeviceCreateInfoKHR* createInfo, VkDevice* vulkanDevice, VkResult* vulkanResult) {
    auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;
    if (!createInfo || createInfo->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR) return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->systemId != 1) return XR_ERROR_SYSTEM_INVALID;

    if (!createInfo || !createInfo->pfnGetInstanceProcAddr || !createInfo->vulkanCreateInfo ||
        !createInfo->vulkanPhysicalDevice || !vulkanDevice || !vulkanResult) return XR_ERROR_VALIDATION_FAILURE;

    vkrt::g_appGipa = createInfo->pfnGetInstanceProcAddr;
    // Both are instance-level, and the only VkInstance in reach is the one the app made
    // through xrCreateVulkanInstanceKHR or named in xrGetVulkanGraphicsDevice2KHR.
    const VkInstance inst = vkrt::g_lastInstance;
    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)vkrt::g_appGipa(
        inst, "vkEnumerateDeviceExtensionProperties");
    auto create = (PFN_vkCreateDevice)vkrt::g_appGipa(inst, "vkCreateDevice");
    if (!create) { Log("[SimXR][VK] xrCreateVulkanDeviceKHR: no vkCreateDevice"); return XR_ERROR_RUNTIME_FAILURE; }

    std::vector<std::string> available;
    if (enumerate) {
        uint32_t n = 0;
        enumerate(createInfo->vulkanPhysicalDevice, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        if (n) enumerate(createInfo->vulkanPhysicalDevice, nullptr, &n, props.data());
        for (const auto& p : props) available.emplace_back(p.extensionName);
    }

    std::vector<std::string> storage;
    std::vector<const char*> names = vkrt::MergeExtensions(
        vkrt::kDeviceExtensions, std::size(vkrt::kDeviceExtensions),
        createInfo->vulkanCreateInfo->ppEnabledExtensionNames,
        createInfo->vulkanCreateInfo->enabledExtensionCount, available, storage);

    VkDeviceCreateInfo dci = *createInfo->vulkanCreateInfo;
    dci.enabledExtensionCount = (uint32_t)names.size();
    dci.ppEnabledExtensionNames = names.data();

    *vulkanResult = create(createInfo->vulkanPhysicalDevice, &dci, createInfo->vulkanAllocator, vulkanDevice);
    Logf("[SimXR] xrCreateVulkanDeviceKHR: %u extensions, VkResult=%d",
         dci.enabledExtensionCount, (int)*vulkanResult);
    return XR_SUCCESS;
}

// --- Minimal implementations ---

static XrResult XRAPI_PTR xrEnumerateApiLayerProperties_runtime(uint32_t propertyCapacityInput,
                                                                uint32_t* propertyCountOutput,
                                                                XrApiLayerProperties* properties) {
    Log("[SimXR] xrEnumerateApiLayerProperties called");
    if (!propertyCountOutput || (propertyCapacityInput && !properties)) return XR_ERROR_VALIDATION_FAILURE;
    *propertyCountOutput = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrEnumerateInstanceExtensionProperties_runtime(const char* layerName,
	uint32_t capacity, uint32_t* count, XrExtensionProperties* properties)
{
	if (layerName)
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	return Extensions().Enumerate(capacity, count, properties);
}

static XrResult XRAPI_PTR xrCreateInstance_runtime(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
    std::lock_guard lifecycleLock(rt::g_instanceLifecycleMutex);
    if (!instance) return XR_ERROR_VALIDATION_FAILURE;
    simxr::Runtime::InstanceRef core;
    auto result = RuntimeState().CreateInstance(createInfo, core);
    if (XR_FAILED(result)) return result;
    struct InstanceCreation
    {
        simxr::Runtime::InstanceRef core;
        bool committed = false;
        ~InstanceCreation() { if (!committed) RuntimeState().DestroyInstance(core); }
    } creation{core};
    const std::span<const char* const> requested(createInfo->enabledExtensionNames, createInfo->enabledExtensionCount);
    rt::Instance newInstance;
    newInstance.handle = core;
    newInstance.core = core.Shared();
    newInstance.controllerMotion = std::make_shared<simxr::legacy::LegacyControllerMotion>();
    // applicationName may not be null-terminated
    char appName[XR_MAX_APPLICATION_NAME_SIZE + 1] = {0};
    memcpy(appName, createInfo->applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE);
    Logf("[SimXR] xrCreateInstance: app=%s version=%u", 
         appName,
         createInfo->applicationInfo.applicationVersion);

    // Restore the saved UI settings here rather than at window creation: apps ask
    // for view configurations, whose panel resolution and FOV both come from the
    // saved headset profile, long before a preview window exists.
    static bool s_settingsLoaded = false;
    if (!s_settingsLoaded) {
        s_settingsLoaded = true;
        ui::LoadSettings(mcp::GetSimulatorDataDirectory());
        char zoomDesc[32] = "fit";
        if (!ui::g_uiState.fitToWindow) {
            snprintf(zoomDesc, sizeof(zoomDesc), "%d%%", (int)(ui::g_uiState.zoomLevel * 100));
        }
        Logf("[SimXR] Settings restored: profile=%ls ipd=%dmm asymmetric=%d zoom=%s",
             ui::GetHeadsetProfileShortName(), ui::GetIpdMillimeters(),
             (int)ui::g_uiState.useAsymmetricFov, zoomDesc);
    }

    if (const auto* profile = std::getenv("SIMXR_HEADSET_PROFILE")) {
        const auto index = ui::FindHeadsetSpec(profile);
        if (index < 0) return XR_ERROR_INITIALIZATION_FAILED;
        ui::SetHeadsetProfile(static_cast<ui::HeadsetProfile>(index));
    }

    for (auto extension : requested)
        Logf("[SimXR] enabledExtension=%s", extension);
    const auto directory = mcp::GetSimulatorDataDirectory();
    newInstance.diagnostics = std::make_shared<flicker::Diagnostics>(directory);
    newInstance.compatibilityFiles = std::make_shared<mcp::WindowsLegacyChannel>(directory);
    newInstance.compatibilityCapture = std::make_shared<mcp::WindowsLegacyCapture>(newInstance.compatibilityFiles);
    ControlEndpoint().Start();
    rt::Instance previousInstance;
    {
        std::lock_guard lock(rt::g_coreMutex);
        previousInstance = std::move(rt::g_instance);
        rt::g_instance = std::move(newInstance);
    }
    { std::lock_guard lock(g_LogMutex); g_logChannel = rt::g_instance.compatibilityFiles; }
    *instance = core;
    creation.committed = true;
    Log("[SimXR] xrCreateInstance: SUCCESS");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroyInstance_runtime(XrInstance instance) {
    std::lock_guard lifecycleLock(rt::g_instanceLifecycleMutex);
    Logf("[SimXR] xrDestroyInstance called: instance=%p", instance);

    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (instance == rt::g_instance.handle && rt::g_session.usesD3D12 && !rt::WaitForPreviewIdle(rt::g_session))
        return XR_ERROR_RUNTIME_FAILURE;
    if (!RuntimeState().Game().Snapshot().game) ControlEndpoint().Close();
    RuntimeState().DestroyInstance(instance);
    if (instance == rt::g_instance.handle) {
        if (rt::g_session.core) rt::g_session.core->Close();
        if (rt::g_session.handle != XR_NULL_HANDLE) {
            const auto result = DestroyNativeSession(rt::g_session.handle);
            if (XR_FAILED(result)) return result;
        }

        // MUST destroy the window before DLL unloads!
        // The OpenXR loader may unload our DLL after this call.
        // If the window stays alive, its WndProc points to unloaded code = crash.
        {
            std::lock_guard<std::mutex> lock(rt::g_windowMutex);
            rt::g_persistentWindow = nullptr;
            rt::g_persistentSwapchain.Reset();
        }
        // DestroyWindow only works from the owning thread. Join before returning: the loader
        // may unload us immediately, and a WndProc left in freed code takes the host down.
        if (rt::g_previewUi.thread.joinable()) {
            // A thread has no message queue until its first message call, and a WM_QUIT
            // posted before then is discarded -- which would leave the join below waiting
            // forever. finished means CreateWindowExW has run, one way or the other.
            for (int i = 0; i < 2000 && !rt::g_previewUi.finished.load(); ++i) Sleep(1);
            if (const DWORD uiTid = rt::g_previewUi.threadId.load()) {
                PostThreadMessageW(uiTid, WM_QUIT, 0, 0);
            }
            rt::g_previewUi.thread.join();
            Log("[SimXR] xrDestroyInstance: Preview UI thread joined");
        }
        rt::g_previewUi.finished.store(false);
        rt::g_previewUi.threadId.store(0);
        {
            std::lock_guard<std::mutex> lock(rt::g_windowMutex);
            rt::g_persistentWindow = nullptr;
            rt::g_persistentSwapchain.Reset();
            rt::g_session.previewSwapchain.Reset();
        }
        rt::Instance retiredInstance;
        {
            std::lock_guard lock(rt::g_coreMutex);
            if (rt::g_instance.handle == instance) {
                retiredInstance = std::move(rt::g_instance);
                rt::g_instance = {};
            }
        }
        if (retiredInstance.compatibilityCapture) retiredInstance.compatibilityCapture->Close();
        if (retiredInstance.compatibilityFiles) retiredInstance.compatibilityFiles->Close();
        if (retiredInstance.diagnostics) retiredInstance.diagnostics->Close();
        // Also clear session window reference
        if (rt::g_session.hwnd) {
            rt::g_session.hwnd = nullptr;
        }

        // Unregister window class so it doesn't have dangling WndProc
        if (rt::g_windowClassRegistered) {
            UnregisterClassW(L"OpenXR Simulator", GetModuleHandleW(nullptr));
            rt::g_windowClassRegistered = false;
            Log("[SimXR] xrDestroyInstance: Window class unregistered");
        }
        Log("[SimXR] xrDestroyInstance: Window destroyed for safe DLL unload");
    }

    Log("[SimXR] xrDestroyInstance: SUCCESS - Returning XR_SUCCESS");
    Log("[SimXR] ========== Instance Destroyed - Waiting for new instance ==========");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInstanceProperties_runtime(XrInstance, XrInstanceProperties* props) {
    if (!props) return XR_ERROR_VALIDATION_FAILURE;
    props->type = XR_TYPE_INSTANCE_PROPERTIES;
    props->next = nullptr;
    props->runtimeVersion = SupportedApiVersion;
    strncpy(props->runtimeName, "OpenXR Simulator Runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
    props->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
    Log("[SimXR] xrGetInstanceProperties: returning OpenXR Simulator Runtime");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystem_runtime(XrInstance, const XrSystemGetInfo* info, XrSystemId* systemId) {
    if (!info || !systemId) return XR_ERROR_VALIDATION_FAILURE;
    Logf("[SimXR] xrGetSystem: formFactor=%d", info->formFactor);
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        Log("[SimXR] xrGetSystem: ERROR - form factor not HMD");
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
    *systemId = (XrSystemId)1; 
    Log("[SimXR] xrGetSystem: SUCCESS -> systemId=1"); 
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystemProperties_runtime(XrInstance instance, XrSystemId system, XrSystemProperties* props)
{
    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (system != 1) return XR_ERROR_SYSTEM_INVALID;
    auto result = simxr::FillSimulationProperties(rt::g_instance.core->Extensions(), props);
    if (XR_FAILED(result)) return result;
    std::strcpy(props->systemName, "OpenXR Simulator");
    props->systemId = 1;
    props->vendorId = 0;
    props->graphicsProperties = {4096, 4096, 16};
    props->trackingProperties = {XR_TRUE, XR_TRUE};
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurations_runtime(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count, XrViewConfigurationType* types) {
    Logf("[SimXR] xrEnumerateViewConfigurations called: capacity=%u", capacity);
    if (count) *count = 1;
    if (capacity >= 1 && types) {
        types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        Log("[SimXR] xrEnumerateViewConfigurations: Returning PRIMARY_STEREO");
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurationViews_runtime(XrInstance, XrSystemId, XrViewConfigurationType viewType, uint32_t capacity, uint32_t* count, XrViewConfigurationView* views) {
    Logf("[SimXR] xrEnumerateViewConfigurationViews called: viewType=%d, capacity=%u", (int)viewType, capacity);
    if (count) *count = 2;
    if (capacity >= 2 && views) {
        for (uint32_t i = 0; i < 2; ++i) {
            views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
            views[i].next = nullptr;
            uint32_t renderW = 0, renderH = 0;
            ui::GetRenderResolution(renderW, renderH);
            views[i].recommendedImageRectWidth = renderW;
            views[i].recommendedImageRectHeight = renderH;
            views[i].recommendedSwapchainSampleCount = 1;
            views[i].maxImageRectWidth = ui::kMaxRenderExtent;
            views[i].maxImageRectHeight = ui::kMaxRenderExtent;
            views[i].maxSwapchainSampleCount = 1;
        }
        Logf("[SimXR] xrEnumerateViewConfigurationViews: Returned 2 views (%ux%u recommended; headset geometry %s)",
             views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
             ui::HeadsetProfileName(ui::g_uiState.headsetProfile));
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateEnvironmentBlendModes_runtime(
    XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* modes) {
    if (count) *count = 1;
    if (capacity >= 1 && modes) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSession_runtime(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
    static int sessionCount = 0;
    sessionCount++;
    Log("[SimXR] ============================================");
    Logf("[SimXR] xrCreateSession called (call #%d, instance=%llu)", sessionCount, (unsigned long long)instance);
    Log("[SimXR] ============================================");
    if (!info || !session || info->type != XR_TYPE_SESSION_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (info->systemId != 1) return XR_ERROR_SYSTEM_INVALID;
    
    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> controllerMotion;
    {
        std::lock_guard lock(rt::g_coreMutex);
        if (rt::g_instance.handle != instance) return XR_ERROR_HANDLE_INVALID;
        controllerMotion = rt::g_instance.controllerMotion;
    }
    if (!controllerMotion) return XR_ERROR_HANDLE_INVALID;
    if (rt::g_session.handle != XR_NULL_HANDLE) return XR_ERROR_LIMIT_REACHED;
    if (rt::g_session.usesVulkan) vkrt::ShutdownSession(rt::g_session);
    struct SessionCreation
    {
        simxr::Runtime::SessionRef core;
        bool committed = false;
        ~SessionCreation()
        {
            if (committed) return;
            if (core) RuntimeState().DestroySession(core);
            DestroyNativeSession(rt::g_session.handle);
        }
    } creation;
    auto publish = [&]() -> XrResult
    {
        auto result = RuntimeState().CreateSession(instance, creation.core, std::make_shared<simxr::WindowsClock>());
        if (XR_FAILED(result)) return result;
        {
            std::lock_guard lock(rt::g_coreMutex);
            if (rt::g_instance.handle != instance || rt::g_instance.controllerMotion != controllerMotion)
                return XR_ERROR_HANDLE_INVALID;
            rt::g_session.core = creation.core.Shared();
            rt::g_session.controllerMotion = controllerMotion;
            rt::g_session.compatibilityFiles = rt::g_instance.compatibilityFiles;
            rt::g_session.compatibilityCapture = rt::g_instance.compatibilityCapture;
            rt::g_session.diagnostics = rt::g_instance.diagnostics;
            rt::g_session.handle = creation.core;
        }
        const auto time = simxr::RuntimeTime{std::chrono::duration_cast<simxr::Duration>(simxr::SteadyClock{}.Now().time_since_epoch())};
        result = creation.core->Ready(time);
        if (XR_FAILED(result)) return result;
        rt::g_session.state = creation.core->Snapshot().state;
        *session = creation.core;
        creation.committed = true;
        Logf("[SimXR] xrCreateSession: SUCCESS handle=%llu", (unsigned long long)*session);
        return XR_SUCCESS;
    };
    const simxr::StructureChain chain(info->next);
    const std::array bindings{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, XR_TYPE_GRAPHICS_BINDING_D3D12_KHR,
        XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR, XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    if (XR_FAILED(chain.Validate(bindings))) return XR_ERROR_VALIDATION_FAILURE;
    std::size_t bindingCount = 0;
    for (auto type : bindings) if (chain.Find<XrBaseInStructure>(type)) ++bindingCount;
    if (bindingCount != 1) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    // Accept D3D11 and D3D12
    const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(info->next);
    while (entry) {
        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            auto enabled = RequireExtension(instance, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
            if (XR_FAILED(enabled)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            const auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
            if (!b->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            
            // Log the device details
            ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(b->device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc;
                    adapter->GetDesc(&desc);
                    Logf("[SimXR] xrCreateSession: App D3D11 device LUID=%llu/%llu", 
                         (unsigned long long)desc.AdapterLuid.HighPart,
                         (unsigned long long)desc.AdapterLuid.LowPart);
                }
            }
            
            rt::g_session.preview11.reset();
            rt::g_session.d3d11Device = b->device;
            rt::g_session.usesD3D12 = false;
            rt::g_session.d3d12Device.Reset();
            rt::g_session.d3d12Queue.Reset();
            rt::ResetD3D12PreviewResources(rt::g_session);
            rt::g_session.state = XR_SESSION_STATE_IDLE;
            b->device->GetImmediateContext(rt::g_session.d3d11Context.GetAddressOf());
            // Window will be created lazily on first frame
            return publish();
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            auto enabled = RequireExtension(instance, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
            if (XR_FAILED(enabled)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            const auto* b12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
            if (!b12->device || !b12->queue) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            rt::g_session.usesD3D12 = true;
            rt::g_session.d3d12Device = b12->device;
            rt::g_session.d3d12Queue = b12->queue;
            rt::g_session.preview11.reset();
            rt::g_session.d3d11Device.Reset();
            rt::g_session.d3d11Context.Reset();
            rt::g_session.previewSwapchain.Reset();
            return publish();
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            if (!rt::g_instance.core ||
                (!rt::g_instance.core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
                 !rt::g_instance.core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)))
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            // XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR is an alias of this, so one branch serves
            // XR_KHR_vulkan_enable and XR_KHR_vulkan_enable2 alike.
            const auto* bVk = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(entry);
            rt::g_session.preview11.reset();
            rt::g_session.d3d11Device.Reset();
            rt::g_session.d3d11Context.Reset();
            rt::g_session.previewSwapchain.Reset();
            rt::g_session.usesOpenGL = false;
            rt::g_session.d3d12Device.Reset();
            rt::g_session.d3d12Queue.Reset();
            rt::ResetD3D12PreviewResources(rt::g_session);
            if (!vkrt::InitSession(rt::g_session, *bVk)) {
                vkrt::ShutdownSession(rt::g_session);
                Log("[SimXR] xrCreateSession: ERROR - the Vulkan binding could not be set up");
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }
            // The compositor is the D3D12 one either way; usesVulkan only changes what the
            // app is handed and how the two queues are ordered.
            rt::g_session.usesVulkan = true;
            rt::g_session.usesD3D12 = true;
            return publish();
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
            auto enabled = RequireExtension(instance, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
            if (XR_FAILED(enabled)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            const auto* bGL = reinterpret_cast<const XrGraphicsBindingOpenGLWin32KHR*>(entry);
            if (!bGL->hDC || !bGL->hGLRC) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            rt::g_session.usesOpenGL = true;
            rt::g_session.usesD3D12 = false;
            rt::g_session.glDC = bGL->hDC;
            rt::g_session.glRC = bGL->hGLRC;
            rt::g_session.preview11.reset();
            rt::g_session.d3d11Device.Reset();
            rt::g_session.d3d11Context.Reset();
            rt::g_session.d3d12Device.Reset();
            rt::g_session.d3d12Queue.Reset();
            rt::g_session.previewSwapchain.Reset();
            return publish();
        }
        entry = entry->next;
    }
    Log("[SimXR] xrCreateSession: ERROR - No supported graphics binding found (D3D11/D3D12/OpenGL)");
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}


static XrResult DestroyNativeSession(XrSession s) {
    Logf("[SimXR] xrDestroySession called (handle=%llu)", (unsigned long long)s);
    if (s != rt::g_session.handle) {
        Logf("[SimXR] xrDestroySession: ERROR - Invalid handle (expected %llu)", 
             (unsigned long long)rt::g_session.handle);
        return XR_ERROR_HANDLE_INVALID;
    }

    if (rt::g_session.usesD3D12 && !rt::WaitForPreviewIdle(rt::g_session))
        return XR_ERROR_RUNTIME_FAILURE;
    if (rt::g_session.usesD3D12) ConsumeCompletedPreviewFrame(rt::g_session);
    if (rt::g_session.core) rt::g_session.core->Close();

    while (!rt::g_swapchains.empty()) {
        const auto result = xrDestroySwapchain_runtime(rt::g_swapchains.begin()->first);
        if (XR_FAILED(result)) return result;
    }
    
    // Transfer window and swapchain to global persistent storage
    // Unity likes to create/destroy sessions rapidly for compatibility checks
    {
        std::lock_guard<std::mutex> lock(rt::g_windowMutex);
        if (rt::g_session.hwnd && !rt::g_persistentWindow) {
            rt::g_persistentWindow = rt::g_session.hwnd;
            rt::g_persistentSwapchain = rt::g_session.previewSwapchain;
            rt::g_persistentWidth = rt::g_session.previewWidth;
            rt::g_persistentHeight = rt::g_session.previewHeight;
            Log("[SimXR] xrDestroySession: Preserving window and swapchain for next session");
        } else if (rt::g_session.hwnd == rt::g_persistentWindow) {
            // Already using persistent window, just update the swapchain
            rt::g_persistentSwapchain = rt::g_session.previewSwapchain;
            rt::g_persistentWidth = rt::g_session.previewWidth;
            rt::g_persistentHeight = rt::g_session.previewHeight;
            Log("[SimXR] xrDestroySession: Updating persistent swapchain");
        }
    }
    
    // Reset session but don't destroy the window
    std::shared_ptr<simxr::Session> retiredCore;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> retiredMotion;
    std::shared_ptr<mcp::WindowsLegacyChannel> retiredFiles;
    std::shared_ptr<mcp::WindowsLegacyCapture> retiredCapture;
    std::shared_ptr<flicker::Diagnostics> retiredDiagnostics;
    {
        std::lock_guard lock(rt::g_coreMutex);
        retiredCore = std::move(rt::g_session.core);
        retiredMotion = std::move(rt::g_session.controllerMotion);
        retiredFiles = std::move(rt::g_session.compatibilityFiles);
        retiredCapture = std::move(rt::g_session.compatibilityCapture);
        retiredDiagnostics = std::move(rt::g_session.diagnostics);
        rt::g_session.handle = XR_NULL_HANDLE;
    }
    rt::g_session.state = XR_SESSION_STATE_IDLE;
	if (rt::g_session.usesOpenGL && rt::g_session.d3d11Context)
		rt::g_session.d3d11Context->ClearState();
	rt::g_session.preview11.reset();
    rt::g_session.d3d11Device.Reset();
    rt::g_session.d3d11Context.Reset();
    rt::g_session.usesD3D12 = false;
    rt::ResetD3D12PreviewResources(rt::g_session);
    if (rt::g_session.usesVulkan) {
        // The images live on the app's VkDevice, which goes away right after this call, so
        // they cannot wait for an xrDestroySwapchain the app may never make.
        for (auto& sc : rt::g_swapchains) {
            if (sc.second.isVulkan) vkrt::DestroySwapchainImages(rt::g_session, sc.second);
        }
        vkrt::ShutdownSession(rt::g_session);
    }
    rt::g_session.d3d12Device.Reset();
    rt::g_session.d3d12Queue.Reset();
    // Reset OpenGL state
    rt::g_session.usesOpenGL = false;
    rt::g_session.glDC = nullptr;
    rt::g_session.glRC = nullptr;
    rt::g_session.hwnd = nullptr;  // Clear from session but window still exists
    rt::g_session.previewWidth = 1920;
    rt::g_session.previewHeight = 540;
    rt::g_session.isFocused = false;
    Log("[SimXR] xrDestroySession: SUCCESS");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroySession_runtime(XrSession handle)
{
    if (handle == rt::g_session.handle && rt::g_session.usesD3D12 && !rt::WaitForPreviewIdle(rt::g_session))
        return XR_ERROR_RUNTIME_FAILURE;
    auto result = RuntimeState().DestroySession(handle);
    if (XR_FAILED(result)) return result;
    return DestroyNativeSession(handle);
}


static XrResult XRAPI_PTR xrEnumerateSwapchainFormats_runtime(XrSession session, uint32_t capacity, uint32_t* count, int64_t* formats) {
    // Vulkan path - VkFormat values, in the order an app should prefer them
    if (rt::g_session.usesVulkan) {
        const uint32_t formatCount = (uint32_t)std::size(vkrt::kFormats);
        if (count) *count = formatCount;
        if (capacity > 0 && formats) {
            const uint32_t copyCount = (capacity < formatCount) ? capacity : formatCount;
            for (uint32_t i = 0; i < copyCount; ++i) formats[i] = vkrt::kFormats[i].vk;
            Logf("[SimXR] xrEnumerateSwapchainFormats(Vulkan): %u formats (first: VkFormat %lld)",
                 copyCount, (long long)formats[0]);
        }
        return XR_SUCCESS;
    }

	if (rt::g_session.usesOpenGL)
	{
		if (!session || session != rt::g_session.handle)
			return XR_ERROR_HANDLE_INVALID;
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

    // D3D11/D3D12 path - return DXGI formats
    const int64_t supportedFormats[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  // Unity often prefers sRGB
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  // UEVR uses this
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,   // HDR format
        DXGI_FORMAT_R32G32B32A32_FLOAT,   // High precision
        DXGI_FORMAT_R10G10B10A2_UNORM,    // HDR10 format
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT, // UEVR's preferred depth format
        DXGI_FORMAT_D32_FLOAT,            // Depth buffer
        DXGI_FORMAT_D24_UNORM_S8_UINT,    // Depth + stencil
        DXGI_FORMAT_D16_UNORM             // 16-bit depth
    };
    const uint32_t formatCount = sizeof(supportedFormats) / sizeof(supportedFormats[0]);

    if (count) *count = formatCount;
    if (capacity > 0 && formats) {
        uint32_t copyCount = (capacity < formatCount) ? capacity : formatCount;
        for (uint32_t i = 0; i < copyCount; ++i) {
            formats[i] = supportedFormats[i];
        }
        Logf("[SimXR] xrEnumerateSwapchainFormats: Returned %u formats (first: %d)", copyCount, (int)formats[0]);
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSwapchain_runtime(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* sc) {
    Log("[SimXR] ============================================");
    Logf("[SimXR] xrCreateSwapchain called: format=%d, size=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, usageFlags=0x%X",
         ci ? (int)ci->format : -1, 
         ci ? ci->width : 0, 
         ci ? ci->height : 0, 
         ci ? ci->arraySize : 0, 
         ci ? ci->mipCount : 0, 
         ci ? ci->sampleCount : 0,
         ci ? ci->usageFlags : 0);
    
    // Log specific usage flags
    if (ci && ci->usageFlags) {
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) 
            Log("[SimXR]   - COLOR_ATTACHMENT");
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) 
            Log("[SimXR]   - DEPTH_STENCIL_ATTACHMENT");
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) 
            Log("[SimXR]   - UNORDERED_ACCESS");
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) 
            Log("[SimXR]   - TRANSFER_SRC");
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) 
            Log("[SimXR]   - TRANSFER_DST");
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) 
            Log("[SimXR]   - SAMPLED");
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) 
            Log("[SimXR]   - MUTABLE_FORMAT");
    }
    
    Log("[SimXR] ============================================");
    if (session == XR_NULL_HANDLE || session != rt::g_session.handle) return XR_ERROR_HANDLE_INVALID;
    if (!ci || !sc) return XR_ERROR_VALIDATION_FAILURE;
    rt::Swapchain chain{}; 
    chain.handle = simxr::AllocateHandle<XrSwapchain>();
    chain.format = (DXGI_FORMAT)ci->format;  // Store the original requested format
    chain.width = ci->width; 
    chain.height = ci->height; 
    chain.arraySize = ci->arraySize ? ci->arraySize : 1;
    chain.lastAcquired = UINT32_MAX;  // No image acquired yet
    chain.lastReleased = UINT32_MAX;  // No image released yet
    // Create textures on appropriate backend
    if (rt::g_session.usesVulkan) {
        const XrResult r = vkrt::CreateSwapchainImages(rt::g_session, chain, *ci);
        if (XR_FAILED(r)) {
            vkrt::DestroySwapchainImages(rt::g_session, chain);
            return r;
        }
        chain.state = std::make_unique<simxr::Swapchain>(chain.imageCount, (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0);
        rt::g_swapchains.emplace(chain.handle, std::move(chain));
        *sc = chain.handle;
        Logf("[SimXR] xrCreateSwapchain(Vulkan): sc=%p fmt=%lld %ux%u array=%u samples=%u",
             *sc, (long long)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
        return XR_SUCCESS;
    }
    if (rt::g_session.usesD3D12) {
        chain.backend = rt::Swapchain::Backend::D3D12;
        chain.imageCount = (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : 3;
        for (uint32_t i = 0; i < chain.imageCount; ++i) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Alignment = 0;
            rd.Width = chain.width;
            rd.Height = chain.height;
            rd.DepthOrArraySize = (UINT)chain.arraySize;
            rd.MipLevels = (UINT)(ci->mipCount ? ci->mipCount : 1);
            chain.mipCount = rd.MipLevels;
            rd.Format = chain.format;
            rd.SampleDesc.Count = (UINT)(ci->sampleCount ? ci->sampleCount : 1);
            rd.SampleDesc.Quality = 0;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_NONE;
            bool isDepth = (chain.format == DXGI_FORMAT_D32_FLOAT ||
                            chain.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                            chain.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                            chain.format == DXGI_FORMAT_D16_UNORM);
            if (isDepth) {
                // Depth textures need ALLOW_DEPTH_STENCIL to be usable as DSV
                rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                // Use typeless format so UEVR can create typed views
                switch (chain.format) {
                    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS; break;
                    case DXGI_FORMAT_D32_FLOAT:            rd.Format = DXGI_FORMAT_R32_TYPELESS; break;
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:    rd.Format = DXGI_FORMAT_R24G8_TYPELESS; break;
                    case DXGI_FORMAT_D16_UNORM:            rd.Format = DXGI_FORMAT_R16_TYPELESS; break;
                    default: break;
                }
            } else {
                if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
                    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
                    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                // Use typeless format for mutable format swapchains so UEVR can create
                // both sRGB and non-sRGB views of the same texture
                if (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) {
                    rd.Format = ToTypeless(chain.format);
                }
            }
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_STATES init = D3D12_RESOURCE_STATE_COMMON;
            if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
                init = D3D12_RESOURCE_STATE_RENDER_TARGET;
            else if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                init = D3D12_RESOURCE_STATE_DEPTH_WRITE;

            // Provide optimized clear value for render targets and depth buffers
            D3D12_CLEAR_VALUE clearValue = {};
            D3D12_CLEAR_VALUE* pClearValue = nullptr;
            if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
                clearValue.Format = chain.format;
                clearValue.Color[0] = 0.0f; clearValue.Color[1] = 0.0f;
                clearValue.Color[2] = 0.0f; clearValue.Color[3] = 1.0f;
                pClearValue = &clearValue;
            } else if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
                clearValue.Format = chain.format;
                clearValue.DepthStencil.Depth = 1.0f;
                clearValue.DepthStencil.Stencil = 0;
                pClearValue = &clearValue;
            }

            ComPtr<ID3D12Resource> res;
            HRESULT hr = rt::g_session.d3d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init, pClearValue, IID_PPV_ARGS(res.GetAddressOf()));
            if (FAILED(hr)) {
                Logf("[SimXR] CreateCommittedResource(D3D12)[%u] FAILED: 0x%08X", i, (unsigned)hr);
                return XR_ERROR_RUNTIME_FAILURE;
            }
            chain.images12.push_back(res);
            chain.imageStates12.push_back(init);
            chain.releaseState12 = init;
        }
        chain.state = std::make_unique<simxr::Swapchain>(chain.imageCount, (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0);
        rt::g_swapchains.emplace(chain.handle, std::move(chain));
        *sc = chain.handle;
        Logf("[SimXR] xrCreateSwapchain(D3D12): sc=%p fmt=%d %ux%u array=%u samples=%u", *sc, (int)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
        return XR_SUCCESS;
    }
	if (rt::g_session.usesOpenGL)
	{
		const bool staticImage = (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
		chain.backend = rt::Swapchain::Backend::OpenGL;
		chain.imageCount = staticImage ? 1 : 3;
		chain.mipCount = ci->mipCount;
		chain.state = std::make_unique<simxr::Swapchain>(chain.imageCount, staticImage);
		const auto result = simxr::windows::OpenGLSwapchain::Create(
			rt::g_session.glDC, rt::g_session.glRC, *ci, chain.glImages);
		if (XR_FAILED(result))
			return result;
		const auto handle = chain.handle;
		rt::g_swapchains.emplace(handle, std::move(chain));
		*sc = handle;
		Logf("[SimXR] xrCreateSwapchain(OpenGL): sc=%p fmt=%lld %ux%u array=%u",
			 handle, static_cast<long long>(ci->format), ci->width, ci->height, ci->arraySize);
		return XR_SUCCESS;
	}
    // D3D11 path
    D3D11_TEXTURE2D_DESC td{};

    // Determine if this is a depth format
    bool isDepthFormat = (chain.format == DXGI_FORMAT_D32_FLOAT ||
                          chain.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                          chain.format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                          chain.format == DXGI_FORMAT_D16_UNORM);

    // For depth formats that need to be sampled, we must use typeless format
    bool needsTypelessDepth = isDepthFormat && (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT);

    // Convert depth format to typeless when sampling is needed
    auto ToTypelessDepth = [](DXGI_FORMAT fmt) -> DXGI_FORMAT {
        switch (fmt) {
            case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
            case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
            default: return fmt;
        }
    };

    // Use typeless format for color textures to allow both UNORM and SRGB views
    // Use typeless format for depth textures when sampling is needed
    if (isDepthFormat) {
        td.Format = needsTypelessDepth ? ToTypelessDepth(chain.format) : chain.format;
    } else {
        td.Format = ToTypeless(chain.format);
    }

    td.Width = chain.width;
    td.Height = chain.height;
    td.ArraySize = chain.arraySize ? chain.arraySize : 1;  // Ensure at least 1
    td.MipLevels = ci->mipCount ? ci->mipCount : 1;  // Ensure at least 1
    chain.mipCount = td.MipLevels;
    td.SampleDesc.Count = ci->sampleCount ? ci->sampleCount : 1;
    td.SampleDesc.Quality = 0;  // Must be 0 for non-MSAA
    // Set bind flags based on format type

    if (isDepthFormat) {
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        // Typeless depth formats can also be shader resources
        if (needsTypelessDepth) {
            td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        }
    } else {
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        
        // Add unordered access if requested
        if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
            td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }
    }
    
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    
    // Set MiscFlags based on what Unity might need
    td.MiscFlags = 0;
    
    // Log the texture description for debugging
    Logf("[SimXR] Creating swapchain textures: Format=%d, %ux%u, Array=%u, Mips=%u, Samples=%u",
         td.Format, td.Width, td.Height, td.ArraySize, td.MipLevels, td.SampleDesc.Count);
    chain.imageCount = (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : 3;
    for (uint32_t i = 0; i < chain.imageCount; ++i) {
        ComPtr<ID3D11Texture2D> tex; 
        HRESULT hr = rt::g_session.d3d11Device->CreateTexture2D(&td, nullptr, tex.GetAddressOf());
        if (FAILED(hr)) { 
            Logf("[SimXR] CreateTexture2D[%u] FAILED: hr=0x%08X", i, (unsigned)hr);
            Logf("[SimXR]   Format=%d, Size=%ux%u, Array=%u, Mips=%u, Samples=%u, BindFlags=0x%X",
                 td.Format, td.Width, td.Height, td.ArraySize, td.MipLevels, td.SampleDesc.Count, td.BindFlags);
            
            // Try to provide more specific error info
            if (hr == E_INVALIDARG) {
                Log("[SimXR]   ERROR: E_INVALIDARG - Invalid texture parameters");
                // Check common issues
                if (td.ArraySize == 0) Log("[SimXR]   - ArraySize is 0");
                if (td.Width == 0 || td.Height == 0) Log("[SimXR]   - Invalid dimensions");
                if (td.MipLevels == 0) Log("[SimXR]   - MipLevels is 0");
            }
            return XR_ERROR_RUNTIME_FAILURE; 
        }
        Logf("[SimXR] Created swapchain texture[%u]: %p", i, tex.Get());
        chain.images.push_back(std::move(tex));
    }
    chain.state = std::make_unique<simxr::Swapchain>(chain.imageCount, (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0);
        rt::g_swapchains.emplace(chain.handle, std::move(chain));
    *sc = chain.handle;
    Logf("[SimXR] xrCreateSwapchain: sc=%p fmt=%d %ux%u array=%u samples=%u", *sc, (int)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateSwapchainImages_runtime(XrSwapchain sc, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    auto it = rt::g_swapchains.find(sc); if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    if (it->second.isVulkan) {
        // XrSwapchainImageVulkan2KHR is a typedef of XrSwapchainImageVulkanKHR, so this is
        // the right shape for both extensions.
        const uint32_t n = (uint32_t)it->second.imagesVk.size();
        if (count) *count = n;
        if (capacity >= n && images) {
            auto* arr = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
            for (uint32_t i = 0; i < n; ++i) { arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR; arr[i].image = it->second.imagesVk[i]; }
        }
        Logf("[SimXR] xrEnumerateSwapchainImages(Vulkan): sc=%p count=%u", sc, n);
        return XR_SUCCESS;
    }
    if (it->second.backend == rt::Swapchain::Backend::D3D12) {
        const uint32_t n = (uint32_t)it->second.images12.size();
        if (count) *count = n;
        if (capacity >= n && images) {
            auto* arr = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
            for (uint32_t i = 0; i < n; ++i) { arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR; arr[i].texture = it->second.images12[i].Get(); }
        }
        Logf("[SimXR] xrEnumerateSwapchainImages(D3D12): sc=%p count=%u", sc, n);
        return XR_SUCCESS;
	}
	else if (it->second.backend == rt::Swapchain::Backend::OpenGL)
	{
		if (!count)
			return XR_ERROR_VALIDATION_FAILURE;
		const auto textures = it->second.glImages->Images();
		*count = static_cast<std::uint32_t>(textures.size());
		if (capacity == 0)
			return XR_SUCCESS;
		if (!images)
			return XR_ERROR_VALIDATION_FAILURE;
		if (capacity < textures.size())
			return XR_ERROR_SIZE_INSUFFICIENT;
		auto* output = reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images);
		for (std::size_t image = 0; image < textures.size(); ++image)
		{
			if (output[image].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR)
				return XR_ERROR_VALIDATION_FAILURE;
		}
		for (std::size_t image = 0; image < textures.size(); ++image)
			output[image].image = textures[image];
		return XR_SUCCESS;
    } else {
        const uint32_t n = (uint32_t)it->second.images.size();
        if (count) *count = n;
        if (capacity >= n && images) {
            auto* arr = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            for (uint32_t i = 0; i < n; ++i) { arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR; arr[i].texture = it->second.images[i].Get(); }
        }
        Logf("[SimXR] xrEnumerateSwapchainImages(D3D11): sc=%p count=%u", sc, n);
        return XR_SUCCESS;
    }
}

static XrResult XRAPI_PTR xrAcquireSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    auto it = rt::g_swapchains.find(sc); if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    auto& ch = it->second;
    if (!index) return XR_ERROR_VALIDATION_FAILURE;
    uint32_t i;
    auto result = ch.state->Acquire(i);
    if (XR_FAILED(result)) return result;
    ch.lastAcquired = i;
    *index = i;
    
    static int acquireCount = 0;
    ++acquireCount;
    // First few calls give startup context; the recurring line is verbose-only,
    // since Log() is deliberately expensive (see its comment).
    if (acquireCount <= 4 || (g_logVerbose && acquireCount % 60 == 1)) {
        Logf("[SimXR] xrAcquireSwapchainImage: sc=%p idx=%u (format=%d, %ux%u)",
             sc, i, (int)ch.format, ch.width, ch.height);
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR xrWaitSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageWaitInfo* info) {
    auto it = rt::g_swapchains.find(sc);
    if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return it->second.state->Wait(info->timeout, true);
}
static XrResult XRAPI_PTR xrReleaseSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageReleaseInfo*) {
    auto it = rt::g_swapchains.find(sc);
    if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    auto& ch = it->second;
    auto result = ch.state->Release(ch.lastReleased);
    if (XR_FAILED(result)) return result;

    // For D3D12: the app has finished using this image, reset our tracked state to COMMON.
    // D3D12 implicit state promotion/decay means COMMON is always safe after a GPU sync point.
    if (ch.backend == rt::Swapchain::Backend::D3D12 && ch.lastReleased < ch.imageStates12.size()) {
        ch.imageStates12[ch.lastReleased] = ch.releaseState12;
    }

    static int releaseCount = 0;
    bool shouldLog = (++releaseCount <= 10);
    if (shouldLog || (g_logVerbose && releaseCount % 60 == 1)) {
        Logf("[SimXR] xrReleaseSwapchainImage: sc=%p released=%u", sc, ch.lastReleased);

    }
    return XR_SUCCESS;
}


namespace rt {
    void PushState(XrSession session, XrSessionState state) {
        if (session == XR_NULL_HANDLE) return;
        auto core = RetainSession(session);
        if (!core) return;
        const auto time = simxr::RuntimeTime{simxr::SteadyClock{}.Now().time_since_epoch()};
        if (state == XR_SESSION_STATE_READY) core->Ready(time);
        else if (state == XR_SESSION_STATE_EXITING) core->RequestExit(time);
        else core->SetState(state, time);
        g_session.state = core->Snapshot().state;
    }
}
static XrResult XRAPI_PTR xrPollEvent_runtime(XrInstance instance, XrEventDataBuffer* buffer) {
    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    return rt::g_instance.core->Events().Poll(*buffer);
}
// Defined further down, with the rest of the preview plumbing.
static void ensurePreviewWithoutProjection(rt::Session& s);

static XrResult XRAPI_PTR xrBeginSession_runtime(XrSession handle, const XrSessionBeginInfo* info) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    const auto controllerMotion = rt::RetainControllerMotion(core);
    if (!controllerMotion) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    simxr::FrameSchedulerConfig config;
    config.epoch = {simxr::Duration{(counter.QuadPart / frequency.QuadPart) * 1000000000LL +
        (counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart}};
    std::uint64_t seed = 0;
    if (const auto* text = std::getenv("SIMXR_SEED")) {
        const std::string_view value{text};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return XR_ERROR_VALIDATION_FAILURE;
    }
    if (const auto* deterministic = std::getenv("SIMXR_DETERMINISTIC"); deterministic && std::strcmp(deterministic, "0") != 0) {
        config.mode = simxr::ExecutionMode::Deterministic;
        config.epoch = {simxr::Duration{1000000000LL}};
    }
    simxr::CommandConfiguration baseline;
    {
        float yaw, pitch, roll;
        rt::GetEffectiveHeadAngles(yaw, pitch, roll);
        baseline.tracking.head.pose = {rt::QuatFromYawPitchRoll(yaw, pitch, roll), rt::g_headPos};
        const auto controllers = controllerMotion->Snapshot({rt::g_headPos, rt::g_headYaw, rt::g_headPitch});
        for (std::size_t hand = 0; hand < controllers.hands.size(); ++hand)
            baseline.tracking.controllers[hand].pose.pose = controllers.hands[hand].worldPose;
        baseline.input = core->Input().Snapshot();
        baseline.views.ipd = rt::g_useCustomIpd ? rt::g_customIpd : rt::GetUiIpdMeters();
        baseline.views.outerFov = {rt::GetViewFov(0), rt::GetViewFov(1)};
        ui::GetRenderResolution(baseline.views.outerWidth, baseline.views.outerHeight);
    }
    auto configured = core->ConfigureCommands(seed, baseline, false);
    if (XR_FAILED(configured)) return configured;
    auto result = core->Begin(*info, config);
    rt::g_session.state = core->Snapshot().state;
    if (XR_SUCCEEDED(result)) rt::g_session.isFocused = true;
    return result;
}
static XrResult XRAPI_PTR xrEndSession_runtime(XrSession handle) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    auto result = core->End({simxr::SteadyClock{}.Now().time_since_epoch()});
    rt::g_session.state = core->Snapshot().state;
    return result;
}
static XrResult XRAPI_PTR xrRequestExitSession_runtime(XrSession handle) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    auto result = core->RequestExit({simxr::SteadyClock{}.Now().time_since_epoch()});
    rt::g_session.state = core->Snapshot().state;
    return result;
}
static XrResult XRAPI_PTR xrWaitFrame_runtime(XrSession session, const XrFrameWaitInfo* info, XrFrameState* s) {
    if (simxr::windows::D3D12Preview::IsPresentationThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (session == XR_NULL_HANDLE || session != rt::g_session.handle) return XR_ERROR_HANDLE_INVALID;
    if (!s || s->type != XR_TYPE_FRAME_STATE || (info && info->type != XR_TYPE_FRAME_WAIT_INFO))
        return XR_ERROR_VALIDATION_FAILURE;
    simxr::FramePrediction prediction;
    std::shared_ptr<simxr::Session> core;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> controllerMotion;
    std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
    {
        std::lock_guard lock(rt::g_coreMutex);
        if (session != rt::g_session.handle) return XR_ERROR_HANDLE_INVALID;
        core = rt::g_session.core;
        controllerMotion = rt::g_session.controllerMotion;
        compatibilityFiles = rt::g_session.compatibilityFiles;
    }
    if (!core || !controllerMotion || !compatibilityFiles) return XR_ERROR_HANDLE_INVALID;
	const auto publishNative = [&](const simxr::FramePrediction& admitted) -> XrResult
	{
		{
			std::lock_guard lock(rt::g_coreMutex);
			if (core != rt::g_session.core || session != rt::g_session.handle)
				return XR_ERROR_HANDLE_INVALID;
		}

		if (!rt::g_session.hwnd)
		{
			ensurePreviewWithoutProjection(rt::g_session);
			if (rt::g_session.hwnd)
			{
				Logf("[SimXR] Preview window on UI thread %lu, frame loop on %lu",
					 GetWindowThreadProcessId(rt::g_session.hwnd, nullptr), GetCurrentThreadId());
			}
		}

		// Not the preview window -- that has its own thread. This drains whatever host windows
		// happen to live on the render thread, which the runtime has always done from here.
		PumpMessages(nullptr);
		const double periodSec = std::chrono::duration<double>(admitted.displayPeriod).count();

		// Handle WASD keyboard input for movement (relative to head orientation).
		//
		// The PID check is what makes AppForeground the default: the runtime is in-process with
		// the host, so the game's own window counts as ours. PreviewFocused was once the only
		// behaviour and left WASD dead in any game that takes the foreground on launch.
		// Note: a game binding W/S/A/D itself will also act on them while it is focused.
		if (rt::g_resetPoseRequested.exchange(false))
		{
			rt::g_headPos = {0, 1.7f, 0};
			rt::g_headYaw = rt::g_headPitch = rt::g_headRoll = 0.0f;
		}
		if (const int dx = rt::g_lookDeltaX.exchange(0))
			rt::g_headYaw -= dx * 0.002f;
		if (const int dy = rt::g_lookDeltaY.exchange(0))
			rt::g_headPitch -= dy * 0.002f;
		const float maxPitch = 1.5f; // ~85 degrees, short of gimbal lock
		rt::g_headPitch = (std::max)(-maxPitch, (std::min)(maxPitch, rt::g_headPitch));

		const ui::InputPolling inputPolling = ui::g_uiState.inputPolling;
		bool inputActive = (inputPolling == ui::InputPolling::Background);
		const HWND foregroundWindow = inputActive ? nullptr : GetForegroundWindow();
		if (foregroundWindow != nullptr)
		{
			if (foregroundWindow == rt::g_session.hwnd)
			{
				inputActive = rt::g_session.isFocused.load();
			}
			else if (inputPolling == ui::InputPolling::AppForeground)
			{
				DWORD fgPid = 0;
				GetWindowThreadProcessId(foregroundWindow, &fgPid);
				inputActive = (fgPid != 0 && fgPid == GetCurrentProcessId());
			}
		}
		simxr::legacy::ControllerMotionFrame controllerFrame;
		controllerFrame.glideSeconds = rt::InputClockSeconds();
		if (inputActive)
		{
			// Shift is the sprint modifier; both it and the base speed are set from
			// Tools > Movement Speed. GetAsyncKeyState rather than the WM_KEYDOWN
			// path because this reads a held state, and the host window may be the
			// one with focus (see above).
			const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			const float moveSpeed = ui::GetMoveSpeed(shiftHeld); // meters per second
			float deltaTime = (float)periodSec;

			XrQuaternionf headQ = rt::QuatFromYawPitch(rt::g_headYaw, rt::g_headPitch);
			XrVector3f fwd = rt::RotateVectorByQuaternion(headQ, XrVector3f{0.0f, 0.0f, -1.0f});
			XrVector3f right = rt::RotateVectorByQuaternion(headQ, XrVector3f{1.0f, 0.0f, 0.0f});

			if (GetAsyncKeyState('W') & 0x8000)
			{
				rt::g_headPos.x += fwd.x * moveSpeed * deltaTime;
				rt::g_headPos.y += fwd.y * moveSpeed * deltaTime;
				rt::g_headPos.z += fwd.z * moveSpeed * deltaTime;
			}
			if (GetAsyncKeyState('S') & 0x8000)
			{
				rt::g_headPos.x -= fwd.x * moveSpeed * deltaTime;
				rt::g_headPos.y -= fwd.y * moveSpeed * deltaTime;
				rt::g_headPos.z -= fwd.z * moveSpeed * deltaTime;
			}
			if (GetAsyncKeyState('A') & 0x8000)
			{
				rt::g_headPos.x -= right.x * moveSpeed * deltaTime;
				rt::g_headPos.y -= right.y * moveSpeed * deltaTime;
				rt::g_headPos.z -= right.z * moveSpeed * deltaTime;
			}
			if (GetAsyncKeyState('D') & 0x8000)
			{
				rt::g_headPos.x += right.x * moveSpeed * deltaTime;
				rt::g_headPos.y += right.y * moveSpeed * deltaTime;
				rt::g_headPos.z += right.z * moveSpeed * deltaTime;
			}
			if (GetAsyncKeyState('Q') & 0x8000)
			{
				rt::g_headPos.y -= moveSpeed * deltaTime;
			}
			if (GetAsyncKeyState('E') & 0x8000)
			{
				rt::g_headPos.y += moveSpeed * deltaTime;
			}

			controllerFrame.keyboard = rt::PollControllerKeyboard(deltaTime);
		}

		if (auto written = compatibilityFiles->TakeWrittenScreenshot())
			ui::NotifyScreenshotSaved((mcp::GetSimulatorDataDirectory() / *written).wstring());

		// MCP commands work regardless of window focus (file-based IPC).
		// The joined worker already decoded and staged copied values.
		{
			const auto commands = compatibilityFiles->TakeCommands();
			mcp::HeadPoseCommand cmd = commands.head;
			if (cmd.valid)
			{
				rt::g_headPos.x = cmd.x;
				rt::g_headPos.y = cmd.y;
				rt::g_headPos.z = cmd.z;
				rt::g_headYaw = cmd.yaw;
				rt::g_headPitch = cmd.pitch;
				if (cmd.hasRoll)
					rt::g_headRoll = cmd.roll;
				compatibilityFiles->Acknowledge("head_pose", true);
			}

			// Per-eye FOV override (asymmetric headset profile testing).
			mcp::FovCommand fov = commands.fov;
			if (fov.valid)
			{
				if (fov.clear)
				{
					rt::g_useCustomFov = false;
				}
				else
				{
					rt::g_useCustomFov = true;
					for (int i = 0; i < 2; ++i)
					{
						rt::g_eyeFovL[i] = fov.angleLeft[i];
						rt::g_eyeFovR[i] = fov.angleRight[i];
						rt::g_eyeFovU[i] = fov.angleUp[i];
						rt::g_eyeFovD[i] = fov.angleDown[i];
					}
				}
				compatibilityFiles->Acknowledge("fov", true);
			}

			// Settable IPD (test app's response to IPD ranges; 0mm = no parallax).
			mcp::IpdCommand ipdCmd = commands.ipd;
			if (ipdCmd.valid)
			{
				if (ipdCmd.clear)
				{
					rt::g_useCustomIpd = false;
				}
				else
				{
					rt::g_useCustomIpd = true;
					rt::g_customIpd = ipdCmd.ipdMeters;
				}
				compatibilityFiles->Acknowledge("ipd", true);
			}

			// Headset profile preset: applies both FOV and IPD at once.
			mcp::HeadsetProfileCommand prof = commands.profile;
			if (prof.valid)
			{
				const int idx = ui::FindHeadsetSpec(prof.name);
				// "generic" carries no headset geometry, so it means the same as clearing.
				const bool clear = idx == 0 || strcmp(prof.name, "default") == 0 || strcmp(prof.name, "clear") == 0;
				bool applied = true;
				if (clear)
				{
					rt::g_useCustomFov = false;
					rt::g_useCustomIpd = false;
				}
				else if (idx > 0)
				{
					const float DEG2RAD = 3.14159265f / 180.0f;
					const ui::HeadsetSpec& spec = ui::kHeadsetSpecs[idx];
					rt::g_useCustomFov = true;
					for (int i = 0; i < 2; ++i)
					{
						const ui::EyeFov& e = spec.eye[i];
						rt::g_eyeFovL[i] = e.angleLeft * DEG2RAD;
						rt::g_eyeFovR[i] = e.angleRight * DEG2RAD;
						rt::g_eyeFovU[i] = e.angleUp * DEG2RAD;
						rt::g_eyeFovD[i] = e.angleDown * DEG2RAD;
					}
					rt::g_useCustomIpd = true;
					rt::g_customIpd = spec.ipdMm * 0.001f;
				}
				else
				{
					applied = false;
				}
				compatibilityFiles->Acknowledge("headset_profile", applied);
			}

			// Anaglyph preview overlay toggle. The simulator already renders
			// a red/cyan anaglyph composite when ui::g_uiState.displayLayout
			// == DisplayLayout::Anaglyph (using channel-masked preview blending
			// blend states), so we just flip the enum. Remember the previous
			// layout so we can restore it on disable.
			mcp::AnaglyphCommand ana = commands.anaglyph;
			if (ana.valid)
			{
				static ui::DisplayLayout s_savedLayout = ui::DisplayLayout::SideBySide;
				static bool s_haveSaved = false;
				if (ana.enabled)
				{
					if (!s_haveSaved)
					{
						s_savedLayout = ui::g_uiState.displayLayout;
						s_haveSaved = true;
					}
					ui::g_uiState.displayLayout = ui::DisplayLayout::Anaglyph;
				}
				else
				{
					if (s_haveSaved)
					{
						ui::g_uiState.displayLayout = s_savedLayout;
						s_haveSaved = false;
					}
				}
				rt::g_anaglyphPreview = ana.enabled;
				compatibilityFiles->Acknowledge("anaglyph", true);
			}

			// Pose sweep: oscillate yaw/pitch/roll on a sine wave each frame.
			mcp::PoseSweepCommand sweep = commands.sweep;
			if (sweep.valid)
			{
				const float DEG2RAD = 3.14159265f / 180.0f;
				rt::g_poseSweepEnabled = sweep.enabled;
				rt::g_poseSweepYawAmp = sweep.yawAmpDeg * DEG2RAD;
				rt::g_poseSweepPitchAmp = sweep.pitchAmpDeg * DEG2RAD;
				rt::g_poseSweepRollAmp = sweep.rollAmpDeg * DEG2RAD;
				rt::g_poseSweepFreq = sweep.freqHz;
				rt::g_poseSweepStartT = (float)GetTickCount64() * 0.001f;
				compatibilityFiles->Acknowledge("pose_sweep", true);
			}

			const auto pose = commands.controllerPose;
			if (pose.valid)
			{
				simxr::legacy::ControllerPoseEdit edit;
				edit.hand = pose.hand == 0 ? simxr::legacy::ControllerHand::Left : simxr::legacy::ControllerHand::Right;
				edit.position = {pose.posX, pose.posY, pose.posZ};
				edit.yaw = pose.yaw;
				edit.pitch = pose.pitch;
				edit.duration = std::chrono::duration<double, std::milli>{pose.durationMs};
				edit.receivedSeconds = rt::InputClockSeconds();
				if (pose.triggerSet)
					edit.trigger = pose.trigger;
				if (pose.buttonA >= 0)
					edit.primary = pose.buttonA != 0;
				controllerFrame.pose = edit;
				compatibilityFiles->Acknowledge("controller_pose", true);
			}

			const auto hold = commands.controllerHold;
			if (hold.valid)
			{
				// Legacy selectors outside [0,1] target no hand; negative means both.
				if (hold.hand < 2)
					controllerFrame.hold = rt::DecodeControllerHold(hold);
				compatibilityFiles->Acknowledge("controller_hold", true);
			}

			const auto wheel = commands.wheel;
			if (wheel.valid)
			{
				constexpr float radians = 0.01745329252f;
				simxr::legacy::ControllerWheelSweep edit;
				edit.enabled = wheel.enabled;
				edit.hub = {wheel.hubX, wheel.hubY, wheel.hubZ};
				edit.radius = wheel.radius;
				edit.tilt = wheel.tiltDeg * radians;
				edit.amplitude = wheel.amplitudeDeg * radians;
				edit.frequencyHz = wheel.freqHz;
				edit.grip = wheel.grip;
				edit.spread = wheel.spreadDeg * radians;
				edit.receivedSeconds = static_cast<float>(GetTickCount64()) * 0.001f;
				controllerFrame.wheel = edit;
				compatibilityFiles->Acknowledge("wheel_sweep", true);
			}
		}

		// Evaluation is atomic for action readers: keyboard -> file pose -> wheel
		// -> held overrides. Shared pose history owns observable space velocities.
		controllerFrame.head = {rt::g_headPos, rt::g_headYaw, rt::g_headPitch};
		controllerFrame.wheelSeconds = static_cast<float>(GetTickCount64()) * 0.001f;
		const auto controllers = controllerMotion->Update(controllerFrame);
		if (controllers.animationToggle)
			Logf("[SimXR] Auto motion %s", *controllers.animationToggle ? "ENABLED" : "DISABLED");
		simxr::TrackingState tracking;
		float effectiveYaw, effectivePitch, effectiveRoll;
		rt::GetEffectiveHeadAngles(effectiveYaw, effectivePitch, effectiveRoll);
		tracking.head.pose = {rt::QuatFromYawPitchRoll(effectiveYaw, effectivePitch, effectiveRoll), rt::g_headPos};
		for (std::size_t hand = 0; hand < controllers.snapshot.hands.size(); ++hand)
			tracking.controllers[hand].pose.pose = controllers.snapshot.hands[hand].worldPose;
		simxr::ViewSettings settings;
		ui::GetRenderResolution(settings.outerWidth, settings.outerHeight);
		settings.ipd = rt::g_useCustomIpd ? rt::g_customIpd : rt::GetUiIpdMeters();
		settings.outerFov = {rt::GetViewFov(0), rt::GetViewFov(1)};
		core->PublishTracking(admitted.displayTime, tracking, settings, admitted.id);

		s->predictedDisplayPeriod = admitted.displayPeriod.count();
		s->predictedDisplayTime = admitted.displayTime.value.count();
		s->shouldRender =
			rt::g_session.state == XR_SESSION_STATE_VISIBLE || rt::g_session.state == XR_SESSION_STATE_FOCUSED;
		return XR_SUCCESS;
	};
	return core->WaitFrame(prediction, publishNative);
}
static XrResult XRAPI_PTR xrBeginFrame_runtime(XrSession session, const XrFrameBeginInfo* info) {
    if (simxr::windows::D3D12Preview::IsPresentationThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (session == XR_NULL_HANDLE) return XR_ERROR_HANDLE_INVALID;
    auto core = rt::RetainSession(session);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return core->BeginFrame();
}

static void previewUiThreadMain(UINT initialClientW, UINT initialClientH) {
    rt::g_previewUi.threadId.store(GetCurrentThreadId());

    if (!rt::g_windowClassRegistered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = rt::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OpenXR Simulator";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassW(&wc);
        rt::g_windowClassRegistered = true;
    }

    RECT rc = { 0, 0, (LONG)initialClientW, (LONG)initialClientH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"OpenXR Simulator", L"OpenXR Simulator (Mouse Look + WASD)",
                                WS_OVERLAPPEDWINDOW, 100, 100,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        Log("[SimXR] Failed to create preview window!");
        rt::g_previewUi.finished.store(true);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ui::ApplyDarkTheme(hwnd);

    // The menu ApplyDarkTheme just attached eats into a client area sized without one, and
    // WM_SIZE saves the shortfall for next launch -- 20px a run, compounding. Measuring
    // beats AdjustWindowRect's bMenu flag, which cannot see a menu that wrapped to two rows.
    RECT client{};
    if (GetClientRect(hwnd, &client)) {
        const int deficitW = (int)initialClientW - (client.right - client.left);
        const int deficitH = (int)initialClientH - (client.bottom - client.top);
        RECT frame{};
        if ((deficitW || deficitH) && GetWindowRect(hwnd, &frame)) {
            SetWindowPos(hwnd, nullptr, 0, 0,
                         (frame.right - frame.left) + deficitW,
                         (frame.bottom - frame.top) + deficitH,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    Logf("[SimXR] Created preview window on its own UI thread: hwnd=%p clientSize=%ux%u thread=%lu",
         hwnd, initialClientW, initialClientH, GetCurrentThreadId());

    // Published only once the window is fully set up, so the frame thread never sees a
    // half-built window.
    rt::g_previewUi.hwnd.store(hwnd);
    rt::g_previewUi.finished.store(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    rt::g_previewUi.hwnd.store(nullptr);
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    Log("[SimXR] Preview UI thread exited and destroyed its window");
}

// Ensure the preview window exists (adopt the persistent one, or ask the UI thread for a
// fresh one). The window size is NOT touched on subsequent calls — once it exists, the
// user (or the menu resize callback) owns its size.
static void ensurePreviewWindow(rt::Session& s, UINT initialClientW, UINT initialClientH) {
    if (s.hwnd) return;

    // Try to adopt a persistent window from a previous session
    {
        std::lock_guard<std::mutex> lock(rt::g_windowMutex);
        if (rt::g_persistentWindow && IsWindow(rt::g_persistentWindow)) {
            s.hwnd = rt::g_persistentWindow;
            // Only for a window adopted across a DLL reload; one our UI thread still owns is
            // already dressed, and this would be reaching into its window from outside.
            if (rt::g_previewUi.hwnd.load() != s.hwnd) {
                // After DLL reload, the old WndProc pointer is invalid
                SetWindowLongPtrW(s.hwnd, GWLP_WNDPROC, (LONG_PTR)rt::WndProc);
                Log("[SimXR] Updated window WndProc to new DLL address");
                ui::ApplyDarkTheme(s.hwnd);
                ShowWindow(s.hwnd, SW_SHOW);
                UpdateWindow(s.hwnd);
            }
            // Seed clientWidth/Height from the actual current client area
            RECT cr{};
            if (GetClientRect(s.hwnd, &cr)) {
                UINT cw = (UINT)(cr.right - cr.left);
                UINT ch = (UINT)(cr.bottom - cr.top);
                if (cw > 0 && ch > 0) {
                    s.clientWidth.store(cw);
                    s.clientHeight.store(ch);
                }
            }
            return;
        }
    }

    // No persistent window — hand creation to the UI thread and wait for it to publish.
    // Bounded: missing the window costs this frame's preview and is retried on the next.
    if (!rt::g_previewUi.thread.joinable()) {
        rt::g_previewUi.thread = std::thread(previewUiThreadMain, initialClientW, initialClientH);
    }
    for (int i = 0; i < 500 && !rt::g_previewUi.finished.load(); ++i) Sleep(1);
    s.hwnd = rt::g_previewUi.hwnd.load();
    if (!s.hwnd) return;

    // windowWidth/Height are left to the WM_SIZE the UI thread has already handled.
    s.clientWidth.store(initialClientW);
    s.clientHeight.store(initialClientH);

    {
        std::lock_guard<std::mutex> lock(rt::g_windowMutex);
        rt::g_persistentWindow = s.hwnd;
        Log("[SimXR] Saved new window to persistent storage");
    }
}

static void ensurePreviewSized(rt::Session& s, UINT width, UINT height, DXGI_FORMAT format) {
    if (width == 0) width = 1;
    if (height == 0) height = 1;

    // Make sure we have a window first
    ensurePreviewWindow(s, width, height);
    if (!s.hwnd) return;

	if (!s.usesD3D12 && s.d3d11Device)
	{
		if (!s.previewSwapchain)
		{
			std::lock_guard lock(rt::g_windowMutex);
			s.previewSwapchain = rt::g_persistentSwapchain;
		}
		if (s.previewSwapchain)
		{
			ComPtr<ID3D11Device> swapchainDevice;
			s.previewSwapchain->GetDevice(IID_PPV_ARGS(swapchainDevice.GetAddressOf()));
			if (swapchainDevice.Get() != s.d3d11Device.Get())
			{
				auto retiredSwapchain = std::move(s.previewSwapchain);
				ComPtr<IDXGISwapChain1> retiredPersistent;
				{
					std::lock_guard lock(rt::g_windowMutex);
					if (rt::g_persistentSwapchain.Get() == retiredSwapchain.Get())
						retiredPersistent.Swap(rt::g_persistentSwapchain);
				}
			}
			else
			{
				DXGI_SWAP_CHAIN_DESC1 description{};
				if (SUCCEEDED(s.previewSwapchain->GetDesc1(&description)))
				{
					s.previewWidth = description.Width;
					s.previewHeight = description.Height;
					s.previewFormat = description.Format;
				}
			}
		}
	}

    if (!s.usesD3D12) {
        if (s.previewSwapchain && s.previewWidth == width && s.previewHeight == height && s.previewFormat == format) return;
    } else {
        // The D3D12 RT is kPreviewRTFormat whatever the app submits - the scaling pass
        // converts - so only a size change can force a rebuild.
        if (s.preview12 && s.preview12->Target() && s.previewPipelines12 &&
            s.previewWidth == width && s.previewHeight == height) return;
    }

    if (!s.usesD3D12) {
        // D3D11/GL path: try ResizeBuffers if the format hasn't changed and we
        // already have a swapchain. Otherwise recreate.
        if (s.previewSwapchain && s.previewFormat == format) {
            HRESULT hr = s.previewSwapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(hr)) {
                s.previewWidth = width;
                s.previewHeight = height;
                {
                    std::lock_guard<std::mutex> lock(rt::g_windowMutex);
                    rt::g_persistentSwapchain = s.previewSwapchain;
                    rt::g_persistentWidth = width;
                    rt::g_persistentHeight = height;
                }
                return;
            }
            Logf("[SimXR] ensurePreviewSized: ResizeBuffers failed 0x%08X, recreating swapchain", (unsigned)hr);
        }

        // Recreate from scratch (format changed, first call, or ResizeBuffers failed)
        s.previewSwapchain.Reset();
        {
            std::lock_guard<std::mutex> lock(rt::g_windowMutex);
            rt::g_persistentSwapchain.Reset();
        }
        s.previewWidth = width;
        s.previewHeight = height;
        s.previewFormat = format;

        ComPtr<IDXGIDevice> dxgiDev; s.d3d11Device.As(&dxgiDev);
        ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(adapter.GetAddressOf());
        ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = format;
        desc.Width = width;
        desc.Height = height;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SampleDesc.Count = 1;
        HRESULT hr = factory->CreateSwapChainForHwnd(s.d3d11Device.Get(), s.hwnd, &desc, nullptr, nullptr, s.previewSwapchain.GetAddressOf());
        Logf("[SimXR] ensurePreviewSized(DX11): hr=0x%08X swapchain=%p size=%ux%u format=%d",
             (unsigned)hr, s.previewSwapchain.Get(), width, height, format);
        if (FAILED(hr)) {
            Logf("[SimXR] ERROR: Failed to create DX11 preview swapchain with format %d", format);
        } else {
            std::lock_guard<std::mutex> lock(rt::g_windowMutex);
            rt::g_persistentSwapchain = s.previewSwapchain;
            rt::g_persistentWidth = width;
            rt::g_persistentHeight = height;
        }
        return;
    } else {
        try {
            if (!s.preview12) {
                ComPtr<IDXGISwapChain1> previousSwapchain;
                { std::lock_guard lock(rt::g_windowMutex); previousSwapchain.Swap(rt::g_persistentSwapchain); }
                previousSwapchain.Reset();
                s.preview12 = std::make_shared<simxr::windows::D3D12Preview>(s.d3d12Device.Get(), s.hwnd);
            }
            // Resize is deferred while an admitted slot still owns native work or
            // an explicit CPU readback. No frame-path fence wait is introduced.
            if (!s.preview12->Resize(width, height)) return;
			if (!s.previewPipelines12)
				s.previewPipelines12 =
					std::make_shared<const simxr::windows::D3D12PreviewPipelines>(s.d3d12Device.Get());
            s.previewWidth = width;
            s.previewHeight = height;
            s.previewFormat = rt::kPreviewRTFormat;
        } catch (const std::exception& error) {
            Logf("[SimXR] DX12 GPU preview initialization failed: %s", error.what());
        }
    }
}

// Bring the preview window (and its backing store) up with no projection layer
// to take a size from. Every other route into ensurePreviewSized hangs off
// RenderProjection, so an app showing only 2D - BetterVR sits on quad-only
// frames for its whole boot and title sequence - would never get a window at
// all. Sizes from the last projection frame if there was one, otherwise from
// the resolution xrEnumerateViewConfigurationViews recommends.
static void ensurePreviewWithoutProjection(rt::Session& s) {
    std::lock_guard<std::mutex> lock(s.previewMutex);

    int srcW = (int)rt::g_sourceWidth.load();
    int srcH = (int)rt::g_sourceHeight.load();
    if (srcW <= 0 || srcH <= 0) {
        srcW = 1280;
        srcH = 720;
        rt::g_sourceWidth.store((uint32_t)srcW);
        rt::g_sourceHeight.store((uint32_t)srcH);
    }

    // Reopen at the size the window was last left at. With nothing saved -- first run, or
    // a settings file from before this was persisted -- fall back to the panel's shape so
    // the first frame is already the right proportions. Either way clamp to the desktop:
    // two 2064x2208 eyes side by side is wider than any monitor, and the saved size may
    // have come from a larger one. A profile or layout change since makes the restored
    // aspect wrong, which the snap on the first WM_SIZE corrects.
    int windowW = ui::g_uiState.windowWidth;
    int windowH = ui::g_uiState.windowHeight;
    // Too small to be a size anyone chose: a file left behind by the menu-bar shrink above.
    constexpr int kMinRestoredClient = 240;
    if (windowW < kMinRestoredClient || windowH < kMinRestoredClient) {
        uint32_t eyeW = 0, eyeH = 0;
        ui::GetDisplayEyeSize(eyeW, eyeH);
        ui::CalculateWindowSize((int)eyeW, (int)eyeH, windowW, windowH);
    }
    rt::ClampToWorkArea(windowW, windowH);
    ensurePreviewWindow(s, (UINT)windowW, (UINT)windowH);
    if (!s.hwnd) return;

    // OpenGL sessions create their D3D11 preview device lazily inside
    // RenderProjection; until that happens there is nothing to build a
    // swapchain on, so leave the backing store to the first 3D frame.
    if (s.usesD3D12 ? !s.d3d12Device : !s.d3d11Device) return;

    int targetW = srcW, targetH = srcH;
    if (!s.usesD3D12) {
        rt::GetPreviewClientSize(s, srcW, srcH, targetW, targetH);
    } else {
        rt::ComputePreviewRTSize(s, targetW, targetH);
    }
    const DXGI_FORMAT format = (s.previewFormat != DXGI_FORMAT_UNKNOWN) ? s.previewFormat : DXGI_FORMAT_R8G8B8A8_UNORM;
    ensurePreviewSized(s, (UINT)targetW, (UINT)targetH, format);
}

struct BorrowedProjectionImage
{
	rt::Swapchain* swapchain = nullptr;
	std::uint32_t index = UINT32_MAX;
};
using ProjectionImages = std::array<BorrowedProjectionImage, 2>;

static ProjectionImages ResolveProjectionImages(const simxr::PreviewProjection& projection)
{
	ProjectionImages images;
	for (std::size_t eye = 0; eye < projection.eyes.size(); ++eye)
	{
		if (!projection.eyes[eye])
			continue;
		const auto found = rt::g_swapchains.find(projection.eyes[eye]->image.source);
		if (found == rt::g_swapchains.end())
			continue;
		auto& swapchain = found->second;
		if (swapchain.lastReleased == UINT32_MAX || swapchain.lastReleased >= swapchain.imageCount)
			continue;
		images[eye] = {&swapchain, swapchain.lastReleased};
	}
	return images;
}

static bool IsPreviewDepthFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_R16_TYPELESS:
		return true;
	default:
		return false;
	}
}

static simxr::windows::PreviewChannels NativePreviewChannels(simxr::PreviewColorChannels channels)
{
	switch (channels)
	{
	case simxr::PreviewColorChannels::Red:
		return simxr::windows::PreviewChannels::Red;
	case simxr::PreviewColorChannels::Cyan:
		return simxr::windows::PreviewChannels::Cyan;
	default:
		return simxr::windows::PreviewChannels::All;
	}
}

static void DrawProjectionD3D11(rt::Session& session, simxr::windows::D3D11PreviewContext& context,
								const BorrowedProjectionImage& image, const simxr::PreviewProjectionSource& source,
								const simxr::PreviewProjectionDraw& draw, ID3D11RenderTargetView* target)
{
	if (!target || !image.swapchain || image.index >= image.swapchain->images.size())
		throw std::invalid_argument("D3D11 projection source or target is unavailable");
	const auto& texture = image.swapchain->images[image.index];
	if (!texture)
		throw std::invalid_argument("D3D11 projection source texture is unavailable");
	D3D11_TEXTURE2D_DESC description{};
	texture->GetDesc(&description);
	if (IsPreviewDepthFormat(description.Format))
		throw std::invalid_argument("D3D11 projection preview cannot sample a depth source");
	auto* preview = rt::EnsureD3D11Preview(session);
	if (!preview)
		throw std::runtime_error("D3D11 projection preview pipeline is unavailable");
	const auto& rectangle = source.image.rectangle;
	const auto& viewport = draw.target.rectangle;
	const D3D11_VIEWPORT nativeViewport{viewport.x, viewport.y, viewport.width, viewport.height, 0, 1};

	const auto copied =
		preview->Copy(context, simxr::windows::PreviewSurface::Projection, texture.Get(), source.image.arraySlice,
					  static_cast<DXGI_FORMAT>(image.swapchain->format),
					  simxr::windows::PreviewRegion{static_cast<std::uint32_t>(rectangle.offset.x),
													static_cast<std::uint32_t>(rectangle.offset.y),
													static_cast<std::uint32_t>(rectangle.extent.width),
													static_cast<std::uint32_t>(rectangle.extent.height)});
	if (!copied ||
		!preview->DrawProjection(context, copied, target, nativeViewport, NativePreviewChannels(draw.channels)))
		throw std::runtime_error("D3D11 projection preview source or draw failed");
}

// Latched once per xrEndFrame so projection and overlays share one update.
static bool g_previewDueThisFrame = true;

// Admit one complete composition. The owner never waits for a free slot.
static rt::PreviewFrameMetadata12* BeginPreviewSlot(rt::Session& session)
{
	if (!g_previewDueThisFrame || !session.preview12)
		return nullptr;
	const bool alreadyRecording = session.preview12->Recording();
	const bool pixelsRequested =
		session.compatibilityCapture && ((session.compatibilityCapture->ScreenshotRequested() &&
										  session.compatibilityCapture->ScreenshotLayer() != "quad") ||
										 session.compatibilityCapture->BurstActive());
	const bool diagnosticsRequested = session.diagnostics && session.diagnostics->NeedsPixels();
	const auto slot = session.preview12->Begin(pixelsRequested || diagnosticsRequested);
	if (!slot)
		return nullptr;
	auto& metadata = session.previewMetadata12[*slot];
	if (!alreadyRecording)
		metadata = rt::PreviewFrameMetadata12{};
	return &metadata;
}

static void ClosePreviewSlot(rt::Session& session, uint32_t frameCount, bool hasProjection)
{
	if (!session.preview12 || !session.preview12->Recording())
		return;
	auto& metadata = session.previewMetadata12[session.preview12->CurrentSlot()];
	metadata.frame = frameCount;
	metadata.hasProjection = hasProjection;
	metadata.headYaw = rt::g_headYaw;
	metadata.headPitch = rt::g_headPitch;
	metadata.headRoll = rt::g_headRoll;
	metadata.headX = rt::g_headPos.x;
	metadata.headY = rt::g_headPos.y;
	metadata.headZ = rt::g_headPos.z;
	if (session.compatibilityCapture)
		metadata.proj = session.compatibilityCapture->LastProjection();
	simxr::windows::PreviewFencePoint ready;
	simxr::windows::PreviewFencePoint released;
	if (session.usesVulkan && session.vk.timeline && session.vk.fence)
	{
		ready = {session.vk.fence.Get(), session.vk.appSignalled};
		released = {session.vk.fence.Get(), ++session.vk.counter};
	}
	if (session.preview12->Submit(session.usesVulkan ? nullptr : session.d3d12Queue.Get(), ready, released))
	{
		if (released.fence)
			session.vk.previewSignalled = released.value;
	}
	else
	{
		Logf("[SimXR] DX12 GPU preview submission rejected: 0x%08X",
			 static_cast<unsigned>(session.preview12->Failure()));
		if (session.compatibilityCapture)
			session.compatibilityCapture->FailScreenshot("D3D12 preview submission failed", frameCount);
	}
}

static void ConsumePreviewFrame(rt::Session& session, const rt::PreviewFrameMetadata12& metadata,
								const simxr::windows::PreviewCompletion& completion,
								const simxr::windows::PreviewReadback& readback)
{
	const auto capture = session.compatibilityCapture;
	if (readback.kind == simxr::windows::PreviewReadbackKind::Quad)
	{
		if (readback.pixels && capture && readback.layout)
		{
			const auto rgba = readback.layout->Rgba(
				{readback.pixels, static_cast<std::size_t>(readback.layout->Extent().readbackBytes)}, readback.bgra);
			capture->StoreQuad(rgba.data(), readback.width, readback.height, metadata.frame);
			if (capture->ScreenshotRequested() && capture->ScreenshotLayer() == "quad")
				capture->SubmitQuad(true, metadata.frame);
		}
		else if (capture)
		{
			capture->FailScreenshot("D3D12 isolated quad readback did not complete successfully", metadata.frame);
		}
		return;
	}
	if (readback.kind == simxr::windows::PreviewReadbackKind::Composite && !readback.pixels && capture)
		capture->FailScreenshot("D3D12 composite readback did not complete successfully", metadata.frame);
	if (readback.pixels && capture)
	{
		if (capture->ScreenshotRequested() && capture->ScreenshotLayer() != "quad")
			capture->SubmitPixels(readback.pixels, readback.width, readback.height, readback.rowPitch, true,
								  metadata.frame);
		if (capture->BurstActive() && metadata.hasProjection)
		{
			mcp::BurstFrameMeta burst;
			burst.frame = metadata.frame;
			burst.headYaw = metadata.headYaw;
			burst.headPitch = metadata.headPitch;
			burst.headRoll = metadata.headRoll;
			burst.headX = metadata.headX;
			burst.headY = metadata.headY;
			burst.headZ = metadata.headZ;
			burst.proj = metadata.proj;
			burst.tMs = static_cast<double>(GetTickCount64());
			capture->BurstOnFrame(readback.pixels, readback.width, readback.height, readback.rowPitch, burst);
		}
	}
	const auto diagnostics = session.diagnostics;
	if (!diagnostics)
		return;
	if (readback.pixels && metadata.hasProjection)
		diagnostics->ObservePreview(readback.pixels, readback.width, readback.height, readback.rowPitch,
									completion.sequence, metadata.frame);
	flicker::UiFrameInfo uiInfo;
	uiInfo.quadLayers = metadata.quadLayers;
	uiInfo.projectionRefreshed = metadata.hasProjection && completion.copied;
	uiInfo.freshReadback = readback.pixels != nullptr;
	uiInfo.cacheValid = metadata.quadComposed && completion.copied;
	uiInfo.composed = metadata.quadComposed && completion.copied;
	memcpy(uiInfo.rects, metadata.quadRects, sizeof(uiInfo.rects));
	uiInfo.sourceAlphaCoverage = metadata.quadSourceAlphaCoverage;
	diagnostics->ObserveUi(readback.pixels, readback.width, readback.height, readback.rowPitch, completion.sequence,
						   metadata.frame, uiInfo);
	diagnostics->ObservePaint(completion.sequence, completion.presentAccepted);
}
// Record projection into the private GPU target. The presentation owner submits
// all layers together, copies to a DXGI buffer and presents on its own worker.
static void RenderProjectionD3D12(rt::Session& session, const simxr::PreviewProjection& projection,
								  const ProjectionImages& images, const simxr::PreviewProjectionDraws& draws)
{
	if (!session.preview12 || !session.preview12->Target() || !session.previewPipelines12)
		return;
	if (!BeginPreviewSlot(session))
		return;
	const auto targetDescription = session.preview12->Target()->GetDesc();
	const auto targetWidth = static_cast<int>(targetDescription.Width);
	const auto targetHeight = static_cast<int>(targetDescription.Height);
	const auto target = session.preview12->RenderTarget();
	auto& commands = *session.preview12->Commands();
	commands.OMSetRenderTargets(1, &target, FALSE, nullptr);
	for (std::size_t index = 0; index < draws.count; ++index)
	{
		const auto& draw = draws.draws[index];
		const auto& image = images[draw.sourceEye];
		if (!image.swapchain || !projection.eyes[draw.sourceEye])
			continue;
		auto& swapchain = *image.swapchain;
		const auto& source = *projection.eyes[draw.sourceEye];
		if (image.index >= swapchain.images12.size() || !swapchain.images12[image.index] ||
			image.index >= swapchain.imageStates12.size())
			continue;
		const auto& viewport = draw.target.rectangle;
		if (viewport.x + viewport.width <= 0 || viewport.y + viewport.height <= 0 || viewport.x >= targetWidth ||
			viewport.y >= targetHeight)
			continue;
		auto& texture = *swapchain.images12[image.index].Get();
		const auto description = texture.GetDesc();
		if (description.SampleDesc.Count > 1 || IsPreviewDepthFormat(description.Format))
			continue;
		const auto binding = session.previewPipelines12->Blit(NativePreviewChannels(draw.channels));
		const auto sourceView = binding.SourceView(description, source.image.arraySlice);
		if (!sourceView)
			continue;
		const bool encodeSrgb = sourceView->Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
								sourceView->Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		const auto constants = simxr::PreviewProjectionConstants(source, viewport, encodeSrgb);
		if (!constants)
			continue;
		const auto sampled = session.preview12->Sample(&texture, *sourceView);
		if (!sampled || !binding.Retain(*session.preview12))
			continue;
		auto& state = swapchain.imageStates12[image.index];
		const auto transition = [&](D3D12_RESOURCE_STATES next)
		{
			if (state == next)
				return;
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = &texture;
			barrier.Transition.StateBefore = state;
			barrier.Transition.StateAfter = next;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commands.ResourceBarrier(1, &barrier);
			state = next;
		};
		binding.Bind(commands);
		transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commands.SetGraphicsRoot32BitConstants(1, simxr::windows::D3D12PreviewPipelines::BlitConstantCount,
											   constants->data(), 0);
		commands.SetGraphicsRootDescriptorTable(0, *sampled);
		const D3D12_VIEWPORT nativeViewport{viewport.x, viewport.y, viewport.width, viewport.height, 0, 1};
		const D3D12_RECT scissor{
			(std::max)(0L, static_cast<LONG>(std::floor(viewport.x))),
			(std::max)(0L, static_cast<LONG>(std::floor(viewport.y))),
			(std::min)(static_cast<LONG>(targetWidth), static_cast<LONG>(std::ceil(viewport.x + viewport.width))),
			(std::min)(static_cast<LONG>(targetHeight), static_cast<LONG>(std::ceil(viewport.y + viewport.height)))};
		commands.RSSetViewports(1, &nativeViewport);
		commands.RSSetScissorRects(1, &scissor);
		commands.DrawInstanced(3, 1, 0, 0);
		transition(swapchain.releaseState12);
	}
}

// Flag to track if Present should be called (deferred until all layers rendered)
static bool g_presentPending = false;

// Wipe the preview to black. Used on frames that carry no projection layer so
// overlays are not composited over the stale - and by then wrong - stereo image
// the last 3D frame left behind. D3D11 and OpenGL only: a D3D12 session gets the
// same wipe from the clear that opens its render target.
static void clearPreviewToBlack(rt::Session& s) {
    if (!s.hwnd) return;

    if (!s.previewSwapchain || !s.d3d11Device || !s.d3d11Context) return;
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(s.previewSwapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) return;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(s.d3d11Device->CreateRenderTargetView(backbuffer.Get(), nullptr, rtv.GetAddressOf()))) return;
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    s.d3d11Context->ClearRenderTargetView(rtv.Get(), black);
    // RenderProjection normally owns the Present; with no projection layer
    // nothing else would flip this frame.
    g_presentPending = true;
}

// Frames per second in the title bar. Counted once per submitted frame, ahead of anything
// that can skip the composite, so the rate shown is the application's and not the mirror's.
static void updatePreviewTitle(rt::Session& s) {
    if (!s.hwnd) return;
    static int frames = 0;
    static auto lastUpdate = std::chrono::high_resolution_clock::now();
    ++frames;
    const auto now = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();
    if (elapsed >= 500) {
        ui::g_lastFps = (int)(frames * 1000 / elapsed);
        frames = 0;
        lastUpdate = now;
    } else if (ui::g_lastScreenshotTickMs == 0) {
        // Otherwise refresh every frame while a screenshot notice is up, so it appears
        // immediately rather than at the next 500ms tick.
        return;
    }
    ui::StatsInfo si = rt::BuildStatsInfo(s);
    ui::UpdateWindowTitle(s.hwnd, &si);
}

static void ReportPreviewFailure(rt::Session& session, const char* operation, const std::exception& error) noexcept
{
	try
	{
		Logf("[SimXR] %s failed: %s", operation, error.what());
		if (session.compatibilityCapture->ScreenshotRequested())
			session.compatibilityCapture->FailScreenshot(error.what());
	}
	catch (...)
	{
		OutputDebugStringA("[SimXR] Preview capture failure could not be published");
	}
}

static bool EnsureOpenGLPreviewDevice(rt::Session& session)
{
	if (session.d3d11Device)
		return true;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL featureLevel;
	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	const auto result =
		D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
						  device.GetAddressOf(), &featureLevel, context.GetAddressOf());
	if (FAILED(result))
	{
		Logf("[SimXR] OpenGL preview device creation failed: 0x%08X", result);
		return false;
	}
	session.preview11.reset();
	session.d3d11Device = std::move(device);
	session.d3d11Context = std::move(context);
	return true;
}

static void ReadProjectionOpenGL(rt::Session& session, const simxr::PreviewProjection& projection,
								 const ProjectionImages& images)
{
	simxr::windows::OpenGLPreviewReadback readback(session.glDC, session.glRC);
	if (!readback.Valid())
		throw std::runtime_error("OpenGL projection preview could not acquire its context");
	for (std::size_t eye = 0; eye < images.size(); ++eye)
	{
		auto& pixels = session.glEyePixels[eye];
		pixels.clear();
		const auto& image = images[eye];
		if (!projection.eyes[eye] || !image.swapchain || image.index >= image.swapchain->glImages->Images().size())
			continue;
		pixels = readback.Read(image.swapchain->glImages->Images()[image.index], image.swapchain->arraySize,
							   projection.eyes[eye]->image, projection.eyes[eye]->textureExtent);
	}
}

static void CaptureProjectionOpenGL(rt::Session& session, const simxr::PreviewProjection& projection)
{
	if (!session.compatibilityCapture->ScreenshotRequested())
		return;
	if (session.compatibilityCapture->ScreenshotLayer() == "quad")
	{
		session.compatibilityCapture->SubmitQuad();
		return;
	}
	const auto width = static_cast<std::uint32_t>(projection.displayExtent.width);
	const auto height = static_cast<std::uint32_t>(projection.displayExtent.height);
	std::array<std::vector<std::uint8_t>, 2> padded;
	for (std::size_t eye = 0; eye < padded.size(); ++eye)
	{
		if (!projection.eyes[eye] || session.glEyePixels[eye].empty())
			continue;
		const auto& rectangle = projection.eyes[eye]->image.rectangle;
		padded[eye] =
			simxr::windows::PadPreviewRgba(session.glEyePixels[eye], static_cast<std::uint32_t>(rectangle.extent.width),
										   static_cast<std::uint32_t>(rectangle.extent.height), width, height);
	}
	session.compatibilityCapture->SubmitStereo(padded[0].empty() ? nullptr : padded[0].data(),
											   padded[1].empty() ? nullptr : padded[1].data(), width, height);
}

static void RenderProjection(rt::Session& session, const simxr::ProjectionLayerDescription& accepted, bool deferPresent)
{
	const auto crop =
		ui::g_uiState.showFullRender ? simxr::PreviewCropSelection::WholeImage : simxr::PreviewCropSelection::Submitted;
	const auto projection = simxr::PreparePreviewProjection(
		accepted,
		[](XrSwapchain handle) -> std::optional<XrExtent2Di>
		{
			const auto found = rt::g_swapchains.find(handle);
			if (found == rt::g_swapchains.end())
				return {};
			return XrExtent2Di{static_cast<std::int32_t>(found->second.width),
							   static_cast<std::int32_t>(found->second.height)};
		},
		crop);
	if (!projection)
		return;
	if (!projection->displayExtent.width || !projection->displayExtent.height)
	{
		if (session.usesD3D12)
			BeginPreviewSlot(session);
		else if (g_previewDueThisFrame)
			clearPreviewToBlack(session);
		return;
	}
	const auto width = static_cast<UINT>(projection->displayExtent.width);
	const auto height = static_cast<UINT>(projection->displayExtent.height);
	const auto previousWidth = rt::g_sourceWidth.exchange(width);
	const auto previousHeight = rt::g_sourceHeight.exchange(height);
	if (session.hwnd && (previousWidth != width || previousHeight != height))
		rt::FitWindowToContentAspect(session.hwnd);
	updatePreviewTitle(session);
	if (!g_previewDueThisFrame)
		return;
	const auto images = ResolveProjectionImages(*projection);
	const auto viewMode = ui::g_uiState.viewMode;
	const auto packing = ui::g_uiState.displayLayout;
	std::lock_guard previewLock(session.previewMutex);
	try
	{
		if (session.usesOpenGL)
		{
			ReadProjectionOpenGL(session, *projection, images);
			CaptureProjectionOpenGL(session, *projection);
			if (!EnsureOpenGLPreviewDevice(session))
				return;
		}
		int targetWidth = static_cast<int>(width);
		int targetHeight = static_cast<int>(height);
		if (session.usesD3D12)
			rt::ComputePreviewRTSize(session, targetWidth, targetHeight);
		else
			rt::GetPreviewClientSize(session, targetWidth, targetHeight, targetWidth, targetHeight);
		ensurePreviewSized(session, static_cast<UINT>(targetWidth), static_cast<UINT>(targetHeight),
						   DXGI_FORMAT_R8G8B8A8_UNORM);
		auto present =
			rt::ComputePresentRect(static_cast<int>(session.previewWidth), static_cast<int>(session.previewHeight));
		auto alignment = simxr::PreviewPixelAlignment::Fractional;
		if (session.usesD3D12)
		{
			if (!session.preview12 || !session.preview12->Target())
				return;
			const auto description = session.preview12->Target()->GetDesc();
			present = rt::ComputePresentRect(static_cast<int>(description.Width), static_cast<int>(description.Height));
			present = {static_cast<float>(std::lround(present.x)), static_cast<float>(std::lround(present.y)),
					   static_cast<float>((std::max)(1L, std::lround(present.w))),
					   static_cast<float>((std::max)(1L, std::lround(present.h)))};
			alignment = simxr::PreviewPixelAlignment::WholePixels;
		}
		const auto eyeLayout = rt::PrepareNativeEyeLayout(present, viewMode, packing, alignment);
		const auto draws = simxr::PreparePreviewProjectionDraws(*projection, eyeLayout, rt::PreviewPacking(packing));
		if (session.usesD3D12)
		{
			RenderProjectionD3D12(session, *projection, images, draws);
			return;
		}
		if (!session.previewSwapchain)
			return;
		simxr::windows::D3D11PreviewContext context(session.d3d11Context.Get());
		ComPtr<ID3D11Texture2D> backbuffer;
		if (FAILED(session.previewSwapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()))))
			return;
		ComPtr<ID3D11RenderTargetView> target;
		if (session.usesOpenGL)
		{
			if (FAILED(session.d3d11Device->CreateRenderTargetView(backbuffer.Get(), nullptr, target.GetAddressOf())))
				return;
		}
		else if (!rt::CreatePreviewRtv(session, backbuffer.Get(), target))
			return;
		const float black[]{0, 0, 0, 1};
		session.d3d11Context->ClearRenderTargetView(target.Get(), black);
		if (session.usesOpenGL)
		{
			auto* preview = rt::EnsureD3D11Preview(session);
			if (!preview)
				return;
			for (std::size_t index = 0; index < draws.count; ++index)
			{
				const auto& draw = draws.draws[index];
				const auto eye = draw.sourceEye;
				if (session.glEyePixels[eye].empty())
					continue;
				const auto& rectangle = projection->eyes[eye]->image.rectangle;
				const auto copied =
					preview->Upload(context, simxr::windows::PreviewSurface::Projection,
									static_cast<std::uint32_t>(rectangle.extent.width),
									static_cast<std::uint32_t>(rectangle.extent.height), DXGI_FORMAT_R8G8B8A8_UNORM,
									session.glEyePixels[eye], static_cast<std::uint32_t>(rectangle.extent.width) * 4);
				const auto& viewport = draw.target.rectangle;
				const D3D11_VIEWPORT nativeViewport{viewport.x, viewport.y, viewport.width, viewport.height, 0, 1};
				if (!copied || !preview->DrawProjection(context, copied, target.Get(), nativeViewport,
														NativePreviewChannels(draw.channels)))
					throw std::runtime_error("OpenGL projection preview upload or draw failed");
			}
		}
		else
		{
			for (std::size_t index = 0; index < draws.count; ++index)
			{
				const auto& draw = draws.draws[index];
				DrawProjectionD3D11(session, context, images[draw.sourceEye], *projection->eyes[draw.sourceEye], draw,
									target.Get());
			}
		}
		if (deferPresent)
			g_presentPending = true;
		else
			session.previewSwapchain->Present(0, 0);
		if (!session.usesOpenGL && session.compatibilityCapture->ScreenshotRequested())
			mcp::CaptureScreenshot(*session.compatibilityCapture, session.d3d11Device.Get(), session.d3d11Context.Get(),
								   session.previewSwapchain.Get());
	}
	catch (const std::exception& error)
	{
		ReportPreviewFailure(session, "Projection preview", error);
	}
}

static void RenderQuadLayer(rt::Session& session, const simxr::QuadLayerDescription& quad,
							const simxr::LocatedViews& views)
{
	// D3D12 sessions use previewRT12, not previewSwapchain
	if (!session.previewSwapchain && !(session.preview12 && session.preview12->Target()))
		return;
	// A layer of a frame the mirror is skipping. RenderProjection skipped the eyes under
	// it and will not present, so drawing this would be work nothing ever shows.
	if (!g_previewDueThisFrame)
		return;

	auto swapchainEntry = rt::g_swapchains.find(quad.color.source);
	if (swapchainEntry == rt::g_swapchains.end())
		return;

	auto& swapchain = swapchainEntry->second;
	const auto geometry = simxr::PreparePreviewQuad(
		quad, views, {static_cast<std::int32_t>(swapchain.width), static_cast<std::int32_t>(swapchain.height)});

	// Get texture dimensions from the quad subImage
	uint32_t sourceWidth = quad.color.rectangle.extent.width;
	uint32_t sourceHeight = quad.color.rectangle.extent.height;

	const auto imageIndex = swapchain.lastReleased;
	if (imageIndex == UINT32_MAX)
		return;

	const auto blendMode = rt::BlendForLayerFlags(quad.space.flags);
	auto alpha = simxr::windows::PreviewAlpha::Opaque;
	const char* blendName = "opaque";
	switch (blendMode)
	{
	case rt::LayerBlend::Opaque:
		break;
	case rt::LayerBlend::Premultiplied:
		alpha = simxr::windows::PreviewAlpha::Premultiplied;
		blendName = "premultiplied";
		break;
	case rt::LayerBlend::Unpremultiplied:
		alpha = simxr::windows::PreviewAlpha::Straight;
		blendName = "unpremultiplied";
		break;
	}

	// --- D3D12 quad compositing -----------------------------------------------------------------------
	// Rasterised into the shared private target and presented on the GPU. Explicit capture
	// requests may additionally copy the completed target. The corners are projected into each eye's
	// view space here; the vertex shader applies that eye's asymmetric projection so the
	// interpolation is perspective-correct and a quad crossing behind the eye is clipped
	// rather than mis-drawn.
	if (session.usesD3D12)
	{
		if (imageIndex >= swapchain.images12.size() || !swapchain.images12[imageIndex])
			return;
		if (swapchain.imageStates12.size() <= imageIndex)
			return;
		if (!session.preview12 || !session.hwnd)
			return;
		if (!session.previewPipelines12)
			return;

		const uint32_t qArraySize = swapchain.arraySize ? swapchain.arraySize : 1;
		if (quad.color.arraySlice >= qArraySize)
		{
			static int badSlice = 0;
			if (++badSlice % 120 == 1)
			{
				Logf("[SimXR] quad: imageArrayIndex %u out of range (arraySize=%u)", quad.color.arraySlice, qArraySize);
			}
			return;
		}

		ID3D12Resource* sourceTexture = swapchain.images12[imageIndex].Get();
		const D3D12_RESOURCE_DESC sourceDescription = sourceTexture->GetDesc();
		if (sourceDescription.SampleDesc.Count > 1)
			return; // needs a resolve first; not worth it for a mirror

		// Share this frame's slot and its open render target with the eye pass. On a
		// quad-only frame nothing has opened the RT yet, so this is what clears it - which
		// is also what replaces the old "wipe the back buffer to black" step.
		rt::PreviewFrameMetadata12* slot = BeginPreviewSlot(session);
		if (!slot)
			return;
		++slot->quadLayers;

		const D3D12_RESOURCE_DESC targetDescription = session.preview12->Target()->GetDesc();
		const auto targetWidth = static_cast<int>(targetDescription.Width);
		const auto targetHeight = static_cast<int>(targetDescription.Height);

		// Only the region the app declared valid is sampled.
		const auto& sourceRectangle = quad.color.rectangle;
		const auto fullBinding = session.previewPipelines12->Quad(alpha);
		const auto redBinding = session.previewPipelines12->Quad(alpha, simxr::windows::PreviewChannels::Red);
		const auto cyanBinding = session.previewPipelines12->Quad(alpha, simxr::windows::PreviewChannels::Cyan);
		const auto sourceView = fullBinding.SourceView(sourceDescription, quad.color.arraySlice);
		if (!sourceView)
			return;
		const auto descriptor = session.preview12->Sample(sourceTexture, *sourceView);
		if (!descriptor || !fullBinding.Retain(*session.preview12) || !redBinding.Retain(*session.preview12) ||
			!cyanBinding.Retain(*session.preview12))
			return;
		const auto sourceDescriptor = *descriptor;
		if (session.compatibilityCapture && session.compatibilityCapture->ScreenshotRequested())
		{
			const auto requestedLayer = session.compatibilityCapture->ScreenshotLayer();
			if (requestedLayer == "quad" || requestedLayer == "all")
			{
				const auto& sourceRect = quad.color.rectangle;
				const simxr::windows::PreviewCrop sourceCrop{
					static_cast<uint32_t>(sourceRect.offset.x), static_cast<uint32_t>(sourceRect.offset.y),
					static_cast<uint32_t>(sourceRect.extent.width), static_cast<uint32_t>(sourceRect.extent.height)};
				const auto result = session.preview12->CaptureQuad(sourceTexture, quad.color.arraySlice, sourceCrop,
																   swapchain.imageStates12[imageIndex]);
				if (result == simxr::windows::PreviewQuadResult::Unsupported)
					session.compatibilityCapture->FailScreenshot(
						"D3D12 isolated quad capture requires single-sample RGBA8 or BGRA8");
				else if (result == simxr::windows::PreviewQuadResult::Invalid)
					session.compatibilityCapture->FailScreenshot(
						"D3D12 isolated quad capture crop/slice is outside the bounded source image");
				else if (result == simxr::windows::PreviewQuadResult::Capacity)
					session.compatibilityCapture->FailScreenshot(
						"D3D12 isolated quad capture admission capacity unavailable");
			}
		}
		auto& quadState = swapchain.imageStates12[imageIndex];
		auto barrier12 = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			if (before == after)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = sourceTexture;
			b.Transition.StateBefore = before;
			b.Transition.StateAfter = after;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			session.preview12->Commands()->ResourceBarrier(1, &b);
		};
		const D3D12_RESOURCE_STATES sourceState = quadState;
		barrier12(sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		// The sRGB view: blending decodes the destination to linear and re-encodes swapchainEntry.
		const D3D12_CPU_DESCRIPTOR_HANDLE targetView = session.preview12->RenderTarget(true);
		slot->quadSourceAlphaCoverage = blendMode == rt::LayerBlend::Opaque ? 1.0f : slot->quadSourceAlphaCoverage;
		fullBinding.Bind(*session.preview12->Commands());
		session.preview12->Commands()->OMSetRenderTargets(1, &targetView, FALSE, nullptr);
		session.preview12->Commands()->SetGraphicsRootDescriptorTable(0, sourceDescriptor);

		// Same split of the RT the eye pass used, so the quad lands over the right eye.
		const rt::FitRect present = rt::ComputePresentRect(targetWidth, targetHeight);
		const auto presentX = static_cast<int>(std::lround(present.x));
		const auto presentY = static_cast<int>(std::lround(present.y));
		const int presentW = (std::max)(1, static_cast<int>(std::lround(present.w)));
		const int presentH = (std::max)(1, static_cast<int>(std::lround(present.h)));
		const ui::ViewMode viewMode = ui::g_uiState.viewMode;
		const ui::DisplayLayout layout = ui::g_uiState.displayLayout;

		const rt::FitRect roundedPresent{static_cast<float>(presentX), static_cast<float>(presentY),
										 static_cast<float>(presentW), static_cast<float>(presentH)};
		const auto eyeLayout =
			rt::PrepareNativeEyeLayout(roundedPresent, viewMode, layout, simxr::PreviewPixelAlignment::WholePixels);
		for (std::size_t eyeIndex = 0; eyeIndex < eyeLayout.count; ++eyeIndex)
		{
			const auto& eyeViewport = eyeLayout.eyes[eyeIndex];
			const auto eye = eyeViewport.eye;
			const auto& viewport = eyeViewport.rectangle;
			if (!geometry || !geometry->eyes[eye])
				continue;

			if (layout == ui::DisplayLayout::Anaglyph && viewMode == ui::ViewMode::BothEyes)
			{
				if (eye == 0)
					redBinding.Bind(*session.preview12->Commands());
				else
					cyanBinding.Bind(*session.preview12->Commands());
			}
			else
			{
				fullBinding.Bind(*session.preview12->Commands());
			}
			const auto constants = simxr::PreviewQuadConstants(*geometry, eye, blendMode == rt::LayerBlend::Opaque);
			session.preview12->Commands()->SetGraphicsRoot32BitConstants(1, rt::kQuadConstantCount, constants.data(),
																		 0);
			const auto bounds = simxr::PreviewQuadBounds(*geometry->eyes[eye], viewport, {targetWidth, targetHeight});
			if (bounds)
			{
				slot->quadRects[eye][0] = bounds->offset.x;
				slot->quadRects[eye][1] = bounds->offset.y;
				slot->quadRects[eye][2] = bounds->extent.width;
				slot->quadRects[eye][3] = bounds->extent.height;
			}
			const D3D12_VIEWPORT nativeViewport{viewport.x, viewport.y, viewport.width, viewport.height, 0, 1};
			const D3D12_RECT scissor{
				(std::max)(0L, static_cast<LONG>(std::floor(viewport.x))),
				(std::max)(0L, static_cast<LONG>(std::floor(viewport.y))),
				(std::min)(static_cast<LONG>(targetWidth), static_cast<LONG>(std::ceil(viewport.x + viewport.width))),
				(std::min)(static_cast<LONG>(targetHeight),
						   static_cast<LONG>(std::ceil(viewport.y + viewport.height)))};
			session.preview12->Commands()->RSSetViewports(1, &nativeViewport);
			session.preview12->Commands()->RSSetScissorRects(1, &scissor);
			session.preview12->Commands()->DrawInstanced(6, 1, 0, 0);
			slot->quadComposed = true;
		}

		// Back to the state the app released it in, so its own barriers still line up.
		barrier12(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sourceState);

		static int s_qlog = 0;
		if (g_logVerbose && ++s_qlog % 120 == 1)
		{
			Logf("[SimXR] D3D12 quad drawn: tex=%ux%u blend=%d space=%s eyeVis=%d size=(%.2f,%.2f)",
				 sourceRectangle.extent.width, sourceRectangle.extent.height, static_cast<int>(blendMode), "RESOLVED",
				 static_cast<int>(quad.eyes), quad.size.width, quad.size.height);
		}
		return;
	}

	static int quadLogCount = 0;
	bool shouldLog = g_logVerbose && (++quadLogCount % 60 == 1);

	if (shouldLog)
	{
		Logf("[SimXR] Quad swapchain: handle=%llu, lastReleased=%u, lastAcquired=%u, imageIndex=%u, imageCount=%u",
			 reinterpret_cast<unsigned long long>(quad.color.source), swapchain.lastReleased, swapchain.lastAcquired,
			 imageIndex, swapchain.imageCount);
	}

	auto* preview = rt::EnsureD3D11Preview(session);
	if (!preview)
		return;
	simxr::windows::D3D11PreviewContext previewContext(session.d3d11Context.Get());
	simxr::windows::D3D11PreviewImage quadImage;

	// Check if using OpenGL
	if (swapchain.backend == rt::Swapchain::Backend::OpenGL && !swapchain.glImages->Images().empty())
	{
		if (imageIndex >= swapchain.glImages->Images().size())
			return;
		const auto texture = swapchain.glImages->Images()[imageIndex];
		try
		{
			{
				simxr::windows::OpenGLPreviewReadback readback(session.glDC, session.glRC);
				session.glQuadPixels = readback.Read(
					texture, swapchain.arraySize, quad.color,
					{static_cast<std::int32_t>(swapchain.width), static_cast<std::int32_t>(swapchain.height)});
			}
			if (session.compatibilityCapture->ScreenshotRequested())
			{
				session.compatibilityCapture->StoreQuad(session.glQuadPixels.data(), sourceWidth, sourceHeight);
				if (session.compatibilityCapture->ScreenshotLayer() == "quad")
					session.compatibilityCapture->SubmitQuad();
			}
			const auto format = swapchain.glImages->Format() == GL_SRGB8_ALPHA8 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
																			  : DXGI_FORMAT_R8G8B8A8_UNORM;
			quadImage = preview->Upload(previewContext, simxr::windows::PreviewSurface::Quad, sourceWidth, sourceHeight,
										format, session.glQuadPixels, sourceWidth * 4);
		}
		catch (const std::exception& error)
		{
			ReportPreviewFailure(session, "OpenGL quad preview", error);
			return;
		}
	}
	else if (!swapchain.images.empty())
	{
		// D3D11 path
		if (imageIndex >= swapchain.images.size() || !swapchain.images[imageIndex])
			return;

		// Skip depth formats
		D3D11_TEXTURE2D_DESC sourceDescription;
		swapchain.images[imageIndex]->GetDesc(&sourceDescription);
		if (sourceDescription.Format == DXGI_FORMAT_D32_FLOAT ||
			sourceDescription.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
			sourceDescription.Format == DXGI_FORMAT_D16_UNORM ||
			sourceDescription.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
		{
			return;
		}

		const uint32_t arraySize = swapchain.arraySize ? swapchain.arraySize : 1;
		const uint32_t arraySlice = quad.color.arraySlice;
		if (arraySlice >= arraySize)
		{
			if (shouldLog)
			{
				Logf("[SimXR] quad: imageArrayIndex %u out of range (arraySize=%u)", arraySlice, arraySize);
			}
			return;
		}

		const auto& rectangle = quad.color.rectangle;

		try
		{
			quadImage =
				preview->Copy(previewContext, simxr::windows::PreviewSurface::Quad, swapchain.images[imageIndex].Get(),
							  arraySlice, static_cast<DXGI_FORMAT>(swapchain.format),
							  simxr::windows::PreviewRegion{static_cast<std::uint32_t>(rectangle.offset.x),
															static_cast<std::uint32_t>(rectangle.offset.y),
															static_cast<std::uint32_t>(rectangle.extent.width),
															static_cast<std::uint32_t>(rectangle.extent.height)});
		}
		catch (const std::exception& error)
		{
			Logf("[SimXR] D3D11 quad source copy failed: %s", error.what());
			return;
		}
	}
	else
	{
		if (shouldLog)
			Log("[SimXR] renderQuadLayer: No valid images in swapchain");
		return;
	}

	// The guard at the top of the function only proves one of the two preview targets exists,
	// and the D3D12 one has already returned by here.
	if (!session.previewSwapchain)
		return;
	ComPtr<ID3D11Texture2D> backbuffer;
	if (FAILED(session.previewSwapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()))))
		return;

	ComPtr<ID3D11RenderTargetView> targetView;
	if (!rt::CreatePreviewRtv(session, backbuffer.Get(), targetView))
		return;

	if (!quadImage)
		return;

	// Calculate viewports matching the projection layer layout (letterboxed)
	const auto layout = ui::g_uiState.displayLayout;
	const auto viewMode = ui::g_uiState.viewMode;
	const bool singleEye = (viewMode != ui::ViewMode::BothEyes);

	// Mirror the rect the projection layer lands in, zoom and pan included.
	rt::FitRect presentRectangle =
		rt::ComputePresentRect(static_cast<int>(session.previewWidth), static_cast<int>(session.previewHeight));

	const auto eyeLayout = rt::PrepareNativeEyeLayout(presentRectangle, viewMode, layout);

	const bool anaglyph = (!singleEye && layout == ui::DisplayLayout::Anaglyph);

	for (std::size_t eyeIndex = 0; eyeIndex < eyeLayout.count; ++eyeIndex)
	{
		const auto& eyeViewport = eyeLayout.eyes[eyeIndex];
		const auto eye = eyeViewport.eye;
		if (!geometry || !geometry->eyes[eye])
			continue;
		const auto& rectangle = eyeViewport.rectangle;
		const D3D11_VIEWPORT viewport{rectangle.x, rectangle.y, rectangle.width, rectangle.height, 0, 1};
		const auto& clip = geometry->eyes[eye]->stripClipCorners;

		auto channels = simxr::windows::PreviewChannels::All;
		if (anaglyph)
		{
			channels = simxr::windows::PreviewChannels::Red;
			if (eye != 0)
				channels = simxr::windows::PreviewChannels::Cyan;
		}
		if (!preview->DrawQuad(previewContext, quadImage, targetView.Get(), viewport, clip, alpha, channels))
		{
			const std::runtime_error error("D3D11 quad preview draw failed");
			ReportPreviewFailure(session, "D3D11 quad preview", error);
			return;
		}
	}

	if (shouldLog)
	{
		Logf("[SimXR] Quad layer: blend=%s space=%s eyeVis=%d pose=(%.2f,%.2f,%.2f) size=(%.2f,%.2f)", blendName,
			 "RESOLVED", static_cast<int>(quad.eyes), quad.poseInSpace.position.x, quad.poseInSpace.position.y,
			 quad.poseInSpace.position.z, quad.size.width, quad.size.height);
	}
}

// Normal completion observes the real fence and retires source readers without
// mapping anything. Only explicitly requested readbacks supply CPU pixels.

static void ConsumeCompletedPreviewFrame(rt::Session& session)
{
	if (!session.preview12)
		return;
	session.preview12->Consume(
		[&](const auto& completion, const auto& readback)
		{
			const auto& metadata = session.previewMetadata12[completion.slot];
			try
			{
				ConsumePreviewFrame(session, metadata, completion, readback);
			}
			catch (const std::exception& error)
			{
				// Optional CPU capture preparation must not unwind an accepted XR
				// frame. The output worker owns only admitted copied CPU bytes.
				if (session.compatibilityCapture)
				{
					try
					{
						session.compatibilityCapture->FailScreenshot(error.what(), metadata.frame);
					}
					catch (...)
					{
						Log("[SimXR] DX12 capture failure status could not be admitted");
					}
				}
				Logf("[SimXR] DX12 explicit capture processing failed: %s", error.what());
			}
		});
}
static XrResult XRAPI_PTR xrEndFrame_runtime(XrSession session, const XrFrameEndInfo* info) {
    if (simxr::windows::D3D12Preview::IsPresentationThread()) return XR_ERROR_CALL_ORDER_INVALID;
    std::shared_ptr<simxr::Session> core;
    std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
    std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
    std::shared_ptr<flicker::Diagnostics> diagnostics;
    {
        std::lock_guard lock(rt::g_coreMutex);
        if (session == XR_NULL_HANDLE || session != rt::g_session.handle) return XR_ERROR_HANDLE_INVALID;
        core = rt::g_session.core;
        compatibilityFiles = rt::g_session.compatibilityFiles;
        compatibilityCapture = rt::g_session.compatibilityCapture;
        diagnostics = rt::g_session.diagnostics;
    }
    if (!core || !compatibilityFiles || !compatibilityCapture || !diagnostics) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_FRAME_END_INFO || (info->layerCount && !info->layers))
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->displayTime <= 0) return XR_ERROR_TIME_INVALID;
    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
        return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    simxr::LayerValidationContext context;
    context.depthEnabled = core->Extensions().Contains(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    context.validSpace = [core](XrSpace space) { return core->Spaces().Owns(space); };
    context.swapchain = [](XrSwapchain handle) -> std::optional<simxr::SwapchainDescription> {
        auto found = rt::g_swapchains.find(handle);
        if (found == rt::g_swapchains.end()) return {};
        const auto& chain = found->second;
        return simxr::SwapchainDescription{chain.width, chain.height, chain.arraySize, chain.lastReleased != UINT32_MAX};
    };
    std::shared_ptr<const simxr::FrameLayers> prepared;
    auto validation = simxr::BuildLayers(*info, context,
        [core](XrSpace space, simxr::RuntimeTime time, simxr::SpaceSample& pose) {
            return core->Spaces().Sample(space, time, pose);
        }, prepared);
    if (XR_FAILED(validation)) return validation;
    // Reentrance guard: when our preview swapchain calls Present(), Steam's overlay
    // (gameoverlayrenderer64) hooks it, which can trigger UEVR to re-submit frames
    // through the OpenXR API layer chain, re-entering this function and causing
    // infinite recursion → EXCEPTION_STACK_OVERFLOW.
    static std::atomic_flag inEndFrame = ATOMIC_FLAG_INIT;
    if (inEndFrame.test_and_set(std::memory_order_acquire)) {
        static std::atomic<int> reentrantCount{0};
        const int count = reentrantCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5) {
            Logf("[SimXR] xrEndFrame: BLOCKED concurrent/reentrant call #%d", count);
        }
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    struct EndFrameCall
    {
        std::atomic_flag& active;
        ~EndFrameCall() { active.clear(std::memory_order_release); }
    } endFrameCall{inEndFrame};

    simxr::FramePrediction submitted;
    const auto previewViews = core->ViewsAt(prepared->displayTime);
    auto frameResult = core->EndFrame(submitted, info, prepared);
    if (XR_FAILED(frameResult)) {
        return frameResult;
    }
    LARGE_INTEGER endFrameStart{};
    QueryPerformanceCounter(&endFrameStart);

    static int frameCount = 0;
    frameCount++;
    compatibilityCapture->SetFrame(static_cast<uint64_t>(frameCount));

    // Log every frame for first 10 frames, then every 60 frames
    bool shouldLog = g_logVerbose && ((frameCount <= 10) || (frameCount % 60 == 1));

    if (shouldLog) {
        Logf("[SimXR] xrEndFrame called (frame #%d)", frameCount);
    }

    // Check D3D12 device status
    if (rt::g_session.usesD3D12 && rt::g_session.d3d12Device && shouldLog) {
        HRESULT reason = rt::g_session.d3d12Device->GetDeviceRemovedReason();
        if (reason != S_OK) {
            Logf("[SimXR] xrEndFrame: D3D12 DEVICE REMOVED! reason=0x%08X", (unsigned)reason);
        }
    }

    if (!info) {
        Log("[SimXR] xrEndFrame: ERROR - info is null");
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Drain the requests that decide whether this frame has to reach the mirror, before
    // deciding it. Every backend's screenshot path then just tests g_screenshotRequested.
    const auto requests = compatibilityFiles->TakeRequests();
    if (requests.screenshot) compatibilityCapture->RequestScreenshot(requests.capture);

    // Frame-burst capture: the optional pose step lands at this frame boundary,
    // so the frame submitted right now (rendered with the OLD pose) is the
    // burst's baseline and every later frame shows the app catching up.
    {
        const auto bc = requests.burst;
        if (bc.valid && !rt::g_session.usesD3D12) {
            // A burst is recorded out of the D3D12 preview's DIB back buffer, the only place
            // a composited frame sits in CPU memory. The D3D11 and OpenGL previews go
            // straight to a swapchain, so say so rather than leaving a poller waiting on a
            // burst_done.json that is never coming.
            Log("[SimXR] burst: only D3D12 and Vulkan sessions can be recorded");
            compatibilityFiles->Acknowledge("burst", false);
        } else if (bc.valid) {
            const bool burstAccepted = (!rt::g_session.usesVulkan || rt::g_session.vk.timeline) && compatibilityCapture->BeginBurst(bc.frames);
            if (burstAccepted && bc.pose.valid) {
                rt::g_headPos.x = bc.pose.x;
                rt::g_headPos.y = bc.pose.y;
                rt::g_headPos.z = bc.pose.z;
                rt::g_headYaw = bc.pose.yaw;
                rt::g_headPitch = bc.pose.pitch;
                if (bc.pose.hasRoll) rt::g_headRoll = bc.pose.roll;
            }
            if (!burstAccepted) compatibilityFiles->Acknowledge("burst", false);
        }
    }

    // Decide once, here, whether the mirror updates this frame; every preview path below
    // hangs off it (see g_previewDueThisFrame). A pending screenshot or a running burst
    // overrides the cap - both asked for a particular frame, not for the next one the rate
    // happens to allow, and with the mirror off there would be no next one.
    g_previewDueThisFrame = ui::PreviewFrameDue() || compatibilityCapture->ScreenshotRequested() || compatibilityCapture->BurstActive();
    if (rt::g_session.usesVulkan && !rt::g_session.vk.timeline) {
        g_previewDueThisFrame = false;
        if (compatibilityCapture->ScreenshotRequested())
            compatibilityCapture->FailScreenshot("Windows Vulkan preview requires external timeline synchronization", frameCount);
        if (frameCount <= 1 || frameCount % 300 == 0)
            Log("[SimXR] Windows Vulkan preview unavailable: external timeline synchronization is required");
    }

    // Order the app's Vulkan queue against the compositor's D3D12 queue. Skipped when the
    // mirror is not due this frame: nothing reads the app's images then.
    if (g_previewDueThisFrame) vkrt::FrameSyncBegin(rt::g_session);

    if (shouldLog) {
        Logf("[SimXR] xrEndFrame: layers=%u", info->layerCount);
    }

    // First pass: count layer types to know if we need to defer Present
    int projectionCount = 0, quadCount = 0, cylinderCount = 0, otherCount = 0;
    bool hasStereoProjection = false;
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (!base) continue;
        switch (base->type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                projectionCount++;
                const auto* projection =
                    reinterpret_cast<const XrCompositionLayerProjection*>(base);
                if (projection->views && projection->viewCount >= 2 &&
                    projection->views[0].subImage.imageRect.extent.width > 0 &&
                    projection->views[0].subImage.imageRect.extent.height > 0 &&
                    projection->views[1].subImage.imageRect.extent.width > 0 &&
                    projection->views[1].subImage.imageRect.extent.height > 0) {
                    hasStereoProjection = true;
                }
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_QUAD: quadCount++; break;
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: cylinderCount++; break;
            default: otherCount++; break;
        }
    }
    if (rt::g_session.usesD3D12 && compatibilityCapture->ScreenshotRequested() &&
        compatibilityCapture->ScreenshotLayer() == "quad" && quadCount == 0)
        compatibilityCapture->FailScreenshot("No quad layer was submitted for the requested D3D12 capture", frameCount);
    const auto projectionTimingNow = perf::ProjectionTimingTracker::Clock::now();
    rt::g_projectionTiming.Observe(hasStereoProjection, projectionTimingNow);
    diagnostics->ObserveSubmission((uint64_t)frameCount, (uint32_t)projectionCount,
                               info->layerCount);

    // Determine if we need to defer Present for overlay layers
    bool hasOverlays = (quadCount > 0 || cylinderCount > 0);
    g_presentPending = false;

    // Second pass: render projection layers (background)
    // If there are overlays, skip Present until after they're rendered
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (!base) continue;

        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
			const auto* projection = std::get_if<simxr::ProjectionLayerDescription>(&prepared->layers[i]);
			if (!projection)
				continue;
			if (!projection->views.empty())
			{
				const auto& pose = projection->views.front().poseInSpace;
				mcp::ProjLogEntry entry{};
				entry.frame = frameCount;
				entry.poseQx = pose.orientation.x;
				entry.poseQy = pose.orientation.y;
				entry.poseQz = pose.orientation.z;
				entry.poseQw = pose.orientation.w;
				entry.posX = pose.position.x;
				entry.posY = pose.position.y;
				entry.posZ = pose.position.z;
				const auto count = (std::min)(projection->views.size(), std::size_t{2});
				for (std::size_t view = 0; view < count; ++view)
				{
					const auto& source = projection->views[view];
					entry.aL[view] = source.fov.angleLeft;
					entry.aR[view] = source.fov.angleRight;
					entry.aU[view] = source.fov.angleUp;
					entry.aD[view] = source.fov.angleDown;
					entry.rectX[view] = source.color.rectangle.offset.x;
					entry.rectY[view] = source.color.rectangle.offset.y;
					entry.rectW[view] = source.color.rectangle.extent.width;
					entry.rectH[view] = source.color.rectangle.extent.height;
				}
				compatibilityCapture->RecordProjection(entry);
			}
			RenderProjection(rt::g_session, *projection, hasOverlays);
        }
    }
    LARGE_INTEGER afterProjection{};
    QueryPerformanceCounter(&afterProjection);

    // No projection layer this frame - the app is on a 2D-only screen. Bring the
    // preview up ourselves (nothing else will) and clear it, so the overlay pass
    // below has somewhere to composite instead of bailing out at its
    // !previewSwapchain && !previewRT12 guard.
    if (projectionCount == 0) {
        ensurePreviewWithoutProjection(rt::g_session);
        if (!g_previewDueThisFrame) {
            // Nothing paints this frame, so there is nothing to wipe either.
        } else if (!rt::g_session.usesD3D12) {
            clearPreviewToBlack(rt::g_session);
        } else if (!hasOverlays) {
            // D3D12 normally clears as part of opening the render target, which whichever
            // layer pass runs first does - so the black and the layers over it reach the
            // window in one repaint. A frame carrying no layers at all has no such pass,
            // so open and close the target here to wipe the last 3D frame off the mirror.
            BeginPreviewSlot(rt::g_session);
        }
    }

    // Third pass: render overlay layers (quad, cylinder) on top of the projection
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (!base) continue;

        switch (base->type) {
            case XR_TYPE_COMPOSITION_LAYER_QUAD: {
                const auto* quad = std::get_if<simxr::QuadLayerDescription>(&prepared->layers[i]);
                if (quad)
                    RenderQuadLayer(rt::g_session, *quad, previewViews);
                break;
            }
            default:
                break;
        }
    }
    LARGE_INTEGER afterOverlays{};
    QueryPerformanceCounter(&afterOverlays);

    // Publish the complete GPU frame, then retire actual completed submissions.
    // Only explicitly requested captures/diagnostics map a fenced readback.
    if (rt::g_session.usesD3D12) {
        ClosePreviewSlot(rt::g_session, (uint32_t)frameCount, projectionCount > 0);
        vkrt::FrameSyncEnd(rt::g_session);
        // Shows a finished composite if the GPU has produced one, and serves the
        // screenshot and burst requests off the very pixels it showed.
        ConsumeCompletedPreviewFrame(rt::g_session);
    }
    LARGE_INTEGER afterDetectionAndPaint{};
    QueryPerformanceCounter(&afterDetectionAndPaint);

    // MCP Integration - write frame status BEFORE Present (Present may block on D3D12)
    compatibilityCapture->PublishStatus({static_cast<std::uint32_t>(frameCount), rt::g_session.previewWidth, rt::g_session.previewHeight,
                          "RGBA8", mcp::GetSessionStateName((int)rt::g_session.state),
                          rt::g_headYaw, rt::g_headPitch, rt::g_headRoll,
                          rt::g_headPos.x, rt::g_headPos.y, rt::g_headPos.z});

    // Drain MCP projection-log dump request if pending.
    if (requests.projectionLog) compatibilityCapture->DumpProjection();

    // D3D11 and OpenGL capture from the preview swapchain inside RenderProjection, which a
    // frame carrying no projection layer never reaches - so on a 2D-only frame the request
    // was drained but never served, and g_previewDueThisFrame stayed latched on it, quietly
    // disabling Mirror Rate. Take the shot off the back buffer here instead, before the
    // deferred Present below makes its contents undefined.
    if (!rt::g_session.usesD3D12 && compatibilityCapture->ScreenshotRequested() && g_previewDueThisFrame &&
        rt::g_session.previewSwapchain && rt::g_session.d3d11Device) {
		simxr::windows::D3D11PreviewContext captureContext(rt::g_session.d3d11Context.Get());
        mcp::CaptureScreenshot(*compatibilityCapture, rt::g_session.d3d11Device.Get(), rt::g_session.d3d11Context.Get(),
                               rt::g_session.previewSwapchain.Get());

    }


    // Now Present after all layers are rendered (D3D11/GL only; D3D12 uses GDI in blit function)
    if (g_presentPending) {
        auto& s = rt::g_session;

        if (s.usesD3D12) {
            // The private D3D12 presentation owner already queued the complete frame.
            static uint64_t deferredD3D12Count = 0;
            if (++deferredD3D12Count <= 5 || deferredD3D12Count % 120 == 1) {
                Log("[SimXR] Deferred D3D12: composed GDI preview already staged");
            }
        } else if (s.previewSwapchain) {
            s.previewSwapchain->Present(0, 0);
        }
        g_presentPending = false;
    }

    // Keep the measurement visible without drawing into the eye images (and
    // therefore without contaminating screenshots or flicker/color analysis).
    // F3 / Tools > Show Statistics adds p50/p95 and the rolling sample count.
    if (rt::g_session.hwnd &&
        rt::g_projectionTiming.ShouldRefreshTitle(projectionTimingNow)) {
        ui::StatsInfo stats = rt::BuildStatsInfo(rt::g_session);
        ui::UpdateWindowTitle(rt::g_session.hwnd, &stats);
    }

    if (shouldLog && (quadCount > 0 || cylinderCount > 0)) {
        Logf("[SimXR] xrEndFrame: proj=%d quad=%d cyl=%d other=%d",
             projectionCount, quadCount, cylinderCount, otherCount);
    }

    if (projectionCount == 0 && shouldLog) {
        Logf("[SimXR] xrEndFrame: no projection layer - 2D-only frame (%d overlay layers)",
             quadCount + cylinderCount);
    }

    LARGE_INTEGER endFrameFinish{};
    QueryPerformanceCounter(&endFrameFinish);
    {
        static LARGE_INTEGER frequency = []() {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value;
        }();
        static uint64_t timingFrames = 0;
        static double projectionMs = 0.0;
        static double overlayMs = 0.0;
        static double detectPaintMs = 0.0;
        static double remainderMs = 0.0;
        static double totalMs = 0.0;
        const auto elapsedMs = [&](const LARGE_INTEGER& begin, const LARGE_INTEGER& end) {
            return 1000.0 * (double)(end.QuadPart - begin.QuadPart) / (double)frequency.QuadPart;
        };
        projectionMs += elapsedMs(endFrameStart, afterProjection);
        overlayMs += elapsedMs(afterProjection, afterOverlays);
        detectPaintMs += elapsedMs(afterOverlays, afterDetectionAndPaint);
        remainderMs += elapsedMs(afterDetectionAndPaint, endFrameFinish);
        totalMs += elapsedMs(endFrameStart, endFrameFinish);
        if (++timingFrames == 300) {
            Logf("[SimXR] xrEndFrame timing avg300: total=%.3fms projection=%.3f overlay=%.3f detectPaint=%.3f status=%.3f",
                 totalMs / timingFrames, projectionMs / timingFrames,
                 overlayMs / timingFrames, detectPaintMs / timingFrames,
                 remainderMs / timingFrames);
            timingFrames = 0;
            projectionMs = overlayMs = detectPaintMs = remainderMs = totalMs = 0.0;
        }
    }

    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrLocateViews_runtime(XrSession handle, const XrViewLocateInfo* info, XrViewState* state,
    uint32_t capacity, uint32_t* count, XrView* views) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    return core->LocateViews(*info, state, capacity, count, views);
}

// Add missing space/action functions for compatibility
static XrResult XRAPI_PTR xrCreateReferenceSpace_runtime(XrSession handle, const XrReferenceSpaceCreateInfo* info, XrSpace* out) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return core->Spaces().CreateReferenceSpace(info->referenceSpaceType, info->poseInReferenceSpace, *out);
}

static XrResult XRAPI_PTR xrDestroySpace_runtime(XrSpace space) {
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Spaces().Destroy(space);
}

static XrResult XRAPI_PTR xrLocateSpace_runtime(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return simxr::LocateSpace(core->Spaces(), space, base, time, location, &core->Extensions());
}

static XrResult XRAPI_PTR xrEnumerateReferenceSpaces_runtime(XrSession handle, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces) {
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle) return XR_ERROR_HANDLE_INVALID;
    if (!count || (capacity && !spaces)) return XR_ERROR_VALIDATION_FAILURE;
    *count = 3;
    if (!capacity) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateActionSpace_runtime(XrSession handle, const XrActionSpaceCreateInfo* info, XrSpace* out) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return core->Spaces().CreateActionSpace(info->action, info->subactionPath, info->poseInActionSpace, *out);
}

static XrResult XRAPI_PTR xrCreateActionSet_runtime(XrInstance handle, const XrActionSetCreateInfo *info, XrActionSet *set)
{
    if (handle == XR_NULL_HANDLE || handle != rt::g_instance.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !set)
        return XR_ERROR_VALIDATION_FAILURE;
    return rt::g_instance.core->Actions().CreateSet(*info, *set);
}

static XrResult XRAPI_PTR xrCreateAction_runtime(XrActionSet set, const XrActionCreateInfo *info, XrAction *action)
{
    if (!rt::g_instance.core)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !action)
        return XR_ERROR_VALIDATION_FAILURE;
    return rt::g_instance.core->Actions().CreateAction(set, *info, *action);
}

static XrResult XRAPI_PTR xrSuggestInteractionProfileBindings_runtime(XrInstance handle, const XrInteractionProfileSuggestedBinding *info)
{
    if (handle == XR_NULL_HANDLE || handle != rt::g_instance.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return rt::g_instance.core->Actions().SuggestBindings(*info);
}

static XrResult XRAPI_PTR xrAttachSessionActionSets_runtime(XrSession handle, const XrSessionActionSetsAttachInfo *info)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info)
        return XR_ERROR_VALIDATION_FAILURE;
    return core->Attach(*info);
}

static XrResult XRAPI_PTR xrGetActionStateBoolean_runtime(XrSession handle, const XrActionStateGetInfo *info, XrActionStateBoolean *state)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return core->Actions().GetBoolean(*info, *state);
}

static XrResult XRAPI_PTR xrGetActionStateFloat_runtime(XrSession handle, const XrActionStateGetInfo *info, XrActionStateFloat *state)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return core->Actions().GetFloat(*info, *state);
}

static XrResult XRAPI_PTR xrGetActionStateVector2f_runtime(XrSession handle, const XrActionStateGetInfo *info, XrActionStateVector2f *state)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return core->Actions().GetVector(*info, *state);
}

static XrResult XRAPI_PTR xrGetActionStatePose_runtime(XrSession handle, const XrActionStateGetInfo *info, XrActionStatePose *state)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !state)
        return XR_ERROR_VALIDATION_FAILURE;
    return core->Actions().GetPose(*info, *state);
}

static XrResult XRAPI_PTR xrStringToPath_runtime(XrInstance handle, const char *name, XrPath *path)
{
    if (handle == XR_NULL_HANDLE || handle != rt::g_instance.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!name || !path)
        return XR_ERROR_VALIDATION_FAILURE;
    auto end = std::find(name, name + XR_MAX_PATH_LENGTH, '\0');
    if (end == name + XR_MAX_PATH_LENGTH)
        return XR_ERROR_PATH_FORMAT_INVALID;
    return rt::g_instance.core->Actions().StringToPath(std::string_view(name, end - name), *path);
}

static XrResult XRAPI_PTR xrPathToString_runtime(XrInstance handle, XrPath path, uint32_t capacity, uint32_t *count, char *buffer)
{
    if (handle == XR_NULL_HANDLE || handle != rt::g_instance.handle)
        return XR_ERROR_HANDLE_INVALID;
    return rt::g_instance.core->Actions().PathToString(path, capacity, count, buffer);
}

static XrResult XRAPI_PTR xrGetCurrentInteractionProfile_runtime(XrSession handle, XrPath user, XrInteractionProfileState *state)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!state || state->type != XR_TYPE_INTERACTION_PROFILE_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    return core->Actions().CurrentProfile(user, state->interactionProfile);
}

static XrResult XRAPI_PTR xrEnumerateBoundSourcesForAction_runtime(XrSession handle, const XrBoundSourcesForActionEnumerateInfo *info,
                                     uint32_t capacity, uint32_t *count, XrPath *out)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    std::vector<XrPath> paths;
    auto result = core->Actions().BoundSources(info->action, paths);
    if (XR_FAILED(result))
        return result;
    if (!count || (capacity && !out)) return XR_ERROR_VALIDATION_FAILURE;
    *count = static_cast<uint32_t>(paths.size());
    if (!capacity) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(paths.begin(), paths.end(), out);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInputSourceLocalizedName_runtime(XrSession handle, const XrInputSourceLocalizedNameGetInfo *info,
                                  uint32_t capacity, uint32_t *count, char *out)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (handle == XR_NULL_HANDLE || handle != rt::g_session.handle)
        return XR_ERROR_HANDLE_INVALID;
    if (!info || !count || (capacity && !out) || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
        return XR_ERROR_VALIDATION_FAILURE;
    auto& registry = rt::g_instance.core->Actions();
    auto source = registry.PathString(info->sourcePath);
    if (!source)
        return XR_ERROR_PATH_INVALID;
    XrPath user;
    auto result = registry.StringToPath(source->starts_with("/user/hand/left/") ? "/user/hand/left" : "/user/hand/right", user);
    if (XR_FAILED(result))
        return result;
    XrPath profile;
    result = core->Actions().CurrentProfile(user, profile);
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

static XrResult XRAPI_PTR xrDestroyAction_runtime(XrAction action) {
    if (!rt::g_instance.core) return XR_ERROR_HANDLE_INVALID;
    auto core = rt::RetainSession();
    if (core) {
        core->Haptics().RemoveAction(action, core->Snapshot().time);
    }
    return rt::g_instance.core->Actions().DestroyAction(action);
}

static XrResult XRAPI_PTR xrDestroyActionSet_runtime(XrActionSet set) {
    if (!rt::g_instance.core) return XR_ERROR_HANDLE_INVALID;
    for (const auto& action : rt::g_instance.core->Actions().Actions())
        if (action.set == set) xrDestroyAction_runtime(action.handle);
    return rt::g_instance.core->Actions().DestroySet(set);
}

static XrResult XRAPI_PTR xrSyncActions_runtime(XrSession handle, const XrActionsSyncInfo* info) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    const auto controllerMotion = rt::RetainControllerMotion(core);
    if (!controllerMotion) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    std::array<simxr::ControllerControls, 2> nativeControls;
    const auto raw = controllerMotion->Snapshot();
    for (std::size_t hand = 0; hand < raw.hands.size(); ++hand) {
        auto& controls = nativeControls[hand];
        controls.connected = true; // scheduled overrides merge after this native baseline
        const auto& controller = raw.hands[hand].controls;
        controls.trigger = controller.trigger;
        controls.grip = controls.gripForce = controller.grip;
        controls.stick = controls.trackpad = controller.stick;
        controls.primary = controls.primaryTouch = controller.primary;
        controls.secondary = controls.secondaryTouch = controller.secondary;
        controls.menu = controller.menu;
        controls.thumbClick = controls.trackpadClick = controller.thumbClick;
        controls.triggerTouch = controller.triggerTouch;
        controls.thumbTouch = controls.trackpadTouch = controller.thumbClick;
    }
    return core->SyncActions(*info, nativeControls);
}

static XrResult XRAPI_PTR xrDestroySwapchain_runtime(XrSwapchain sc)
{
	auto it = rt::g_swapchains.find(sc);
	if (it == rt::g_swapchains.end())
		return XR_ERROR_HANDLE_INVALID;

	// A D3D12 command list does not keep the resources it references alive, and the
	// preview no longer waits for its own submissions inside xrEndFrame - so a composite
	// reading these images can still be queued. Dropping the last reference here would
	// pull them out from under it. Not on any frame path, so the wait costs nothing.
	if (rt::g_session.usesD3D12 && !rt::WaitForPreviewIdle(rt::g_session))
		return XR_ERROR_RUNTIME_FAILURE;

	if (it->second.glImages && !it->second.glImages->Close())
		return XR_ERROR_RUNTIME_FAILURE;

	if (it->second.isVulkan)
		vkrt::DestroySwapchainImages(rt::g_session, it->second);

	rt::g_swapchains.erase(it);
	Logf("[SimXR] xrDestroySwapchain: sc=%p", sc);
	return XR_SUCCESS;
}

// Missing functions Unity needs
static XrResult XRAPI_PTR xrResultToString_runtime(XrInstance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    const char* str = "XR_SUCCESS";
    if (value != XR_SUCCESS) str = "XR_ERROR";
    strncpy(buffer, str, XR_MAX_RESULT_STRING_SIZE - 1);
    buffer[XR_MAX_RESULT_STRING_SIZE - 1] = '\0';
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrStructureTypeToString_runtime(XrInstance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType_%d", (int)value);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetReferenceSpaceBoundsRect_runtime(XrSession handle, XrReferenceSpaceType type, XrExtent2Df* bounds) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    return core->Spaces().Bounds(type, *bounds);
}

static XrResult XRAPI_PTR xrGetViewConfigurationProperties_runtime(XrInstance, XrSystemId, XrViewConfigurationType type, 
                                                                   XrViewConfigurationProperties* props) {
    if (!props) return XR_ERROR_VALIDATION_FAILURE;
    props->type = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
    props->viewConfigurationType = type;
    props->fovMutable = XR_FALSE;
    Log("[SimXR] xrGetViewConfigurationProperties");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrApplyHapticFeedback_runtime(XrSession handle, const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->ApplyHaptic(info, feedback);
}
static XrResult XRAPI_PTR xrStopHapticFeedback_runtime(XrSession handle, const XrHapticActionInfo* info)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->StopHaptic(info);
}

static XrResult XRAPI_PTR xrEnumerateViveTrackerPathsHTCX_runtime(XrInstance handle, uint32_t capacity, uint32_t* count, XrViveTrackerPathsHTCX* paths)
{
    auto instance = RuntimeState().FindInstance(handle);
    if (!instance) return XR_ERROR_HANDLE_INVALID;
    return instance->Trackers().Enumerate(capacity, count, paths);
}

static XrResult XRAPI_PTR xrCreateBodyTrackerFB_runtime(XrSession handle, const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* output)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().CreateFB(info, output);
}

static XrResult XRAPI_PTR xrCreateBodyTrackerBD_runtime(XrSession handle, const XrBodyTrackerCreateInfoBD* info, XrBodyTrackerBD* output)
{
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().CreateBD(info, output);
}

static XrResult XRAPI_PTR xrDestroyBodyTrackerFB_runtime(XrBodyTrackerFB tracker)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().DestroyFB(tracker);
}

static XrResult XRAPI_PTR xrDestroyBodyTrackerBD_runtime(XrBodyTrackerBD tracker)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().DestroyBD(tracker);
}

static XrResult XRAPI_PTR xrLocateBodyJointsFB_runtime(XrBodyTrackerFB tracker, const XrBodyJointsLocateInfoFB* info, XrBodyJointLocationsFB* output)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().LocateFB(tracker, info, output);
}

static XrResult XRAPI_PTR xrLocateBodyJointsBD_runtime(XrBodyTrackerBD tracker, const XrBodyJointsLocateInfoBD* info, XrBodyJointLocationsBD* output)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().LocateBD(tracker, info, output);
}

static XrResult XRAPI_PTR xrGetBodySkeletonFB_runtime(XrBodyTrackerFB tracker, XrBodySkeletonFB* output)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().SkeletonFB(tracker, output);
}

static XrResult XRAPI_PTR xrSuggestBodyTrackingCalibrationOverrideMETA_runtime(XrBodyTrackerFB tracker, const XrBodyTrackingCalibrationInfoMETA* info)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().SuggestCalibrationFB(tracker, info);
}

static XrResult XRAPI_PTR xrResetBodyTrackingCalibrationMETA_runtime(XrBodyTrackerFB tracker)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().ResetCalibrationFB(tracker);
}

static XrResult XRAPI_PTR xrRequestBodyTrackingFidelityMETA_runtime(XrBodyTrackerFB tracker, XrBodyTrackingFidelityMETA fidelity)
{
    auto core = rt::RetainSession();
    if (!core) return XR_ERROR_HANDLE_INVALID;
    return core->Body().RequestFidelityFB(tracker, fidelity);
}

// Time conversion functions for XR_KHR_win32_convert_performance_counter_time
static XrResult XRAPI_PTR xrConvertWin32PerformanceCounterToTimeKHR_runtime(XrInstance instance,
                                                                            const LARGE_INTEGER* performanceCounter,
                                                                            XrTime* time) {
    auto result = RequireExtension(instance, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;

    if (!performanceCounter || !time) return XR_ERROR_VALIDATION_FAILURE;
    
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    
    // Convert to nanoseconds
    const long long secs = performanceCounter->QuadPart / freq.QuadPart;
    const long long rem = performanceCounter->QuadPart % freq.QuadPart;
    *time = secs * 1000000000LL + (rem * 1000000000LL) / freq.QuadPart;
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrConvertTimeToWin32PerformanceCounterKHR_runtime(XrInstance instance,
                                                                             XrTime time,
                                                                             LARGE_INTEGER* performanceCounter) {
    auto result = RequireExtension(instance, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    if (XR_FAILED(result)) return result;

    if (!performanceCounter) return XR_ERROR_VALIDATION_FAILURE;
    
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    
    // Convert from nanoseconds  
    const long long secs = time / 1000000000LL;
    const long long rem = time % 1000000000LL;
    performanceCounter->QuadPart = secs * freq.QuadPart + (rem * freq.QuadPart) / 1000000000LL;
    return XR_SUCCESS;
}

// ----------------------------------------------

static const simxr::DispatchEntry kFnTable[] = {
    {"xrGetInstanceProcAddr", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetInstanceProcAddr_runtime>::Invoke, true},
    {"xrEnumerateApiLayerProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateApiLayerProperties_runtime>::Invoke, true},
    {"xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateInstanceExtensionProperties_runtime>::Invoke, true},
    {"xrCreateInstance", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateInstance_runtime>::Invoke, true},
    {"xrDestroyInstance", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyInstance_runtime>::Invoke},
    {"xrGetInstanceProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetInstanceProperties_runtime>::Invoke},
    {"xrGetSystem", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetSystem_runtime>::Invoke},
    {"xrGetSystemProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetSystemProperties_runtime>::Invoke},
    {"xrEnumerateViewConfigurations", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateViewConfigurations_runtime>::Invoke},
    {"xrEnumerateViewConfigurationViews", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateViewConfigurationViews_runtime>::Invoke},
    {"xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateEnvironmentBlendModes_runtime>::Invoke},
    {"xrEnumerateViveTrackerPathsHTCX", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateViveTrackerPathsHTCX_runtime>::Invoke},
    {"xrCreateBodyTrackerFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateBodyTrackerFB_runtime>::Invoke},
    {"xrCreateBodyTrackerBD", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateBodyTrackerBD_runtime>::Invoke},
    {"xrDestroyBodyTrackerFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyBodyTrackerFB_runtime>::Invoke},
    {"xrDestroyBodyTrackerBD", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroyBodyTrackerBD_runtime>::Invoke},
    {"xrLocateBodyJointsFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrLocateBodyJointsFB_runtime>::Invoke},
    {"xrLocateBodyJointsBD", (PFN_xrVoidFunction)simxr::AbiBoundary<xrLocateBodyJointsBD_runtime>::Invoke},
    {"xrGetBodySkeletonFB", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetBodySkeletonFB_runtime>::Invoke},
    {"xrSuggestBodyTrackingCalibrationOverrideMETA", (PFN_xrVoidFunction)simxr::AbiBoundary<xrSuggestBodyTrackingCalibrationOverrideMETA_runtime>::Invoke},
    {"xrResetBodyTrackingCalibrationMETA", (PFN_xrVoidFunction)simxr::AbiBoundary<xrResetBodyTrackingCalibrationMETA_runtime>::Invoke},
    {"xrRequestBodyTrackingFidelityMETA", (PFN_xrVoidFunction)simxr::AbiBoundary<xrRequestBodyTrackingFidelityMETA_runtime>::Invoke},
    {"xrCreateSession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateSession_runtime>::Invoke},
    {"xrDestroySession", (PFN_xrVoidFunction)simxr::AbiBoundary<xrDestroySession_runtime>::Invoke},
    {"xrEnumerateSwapchainFormats", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateSwapchainFormats_runtime>::Invoke},
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
    {"xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetD3D11GraphicsRequirementsKHR_runtime>::Invoke},
    {"xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetD3D12GraphicsRequirementsKHR_runtime>::Invoke},
    {"xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetOpenGLGraphicsRequirementsKHR_runtime>::Invoke},
    // XR_KHR_vulkan_enable
    {"xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsRequirementsKHR_runtime>::Invoke},
    {"xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanInstanceExtensionsKHR_runtime>::Invoke},
    {"xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanDeviceExtensionsKHR_runtime>::Invoke},
    {"xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsDeviceKHR_runtime>::Invoke},
    // XR_KHR_vulkan_enable2
    {"xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsRequirements2KHR_runtime>::Invoke},
    {"xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateVulkanInstanceKHR_runtime>::Invoke},
    {"xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrCreateVulkanDeviceKHR_runtime>::Invoke},
    {"xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetVulkanGraphicsDevice2KHR_runtime>::Invoke},
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
    {"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)simxr::AbiBoundary<xrSuggestInteractionProfileBindings_runtime>::Invoke},
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
    {"xrGetCurrentInteractionProfile", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetCurrentInteractionProfile_runtime>::Invoke},
    {"xrEnumerateBoundSourcesForAction", (PFN_xrVoidFunction)simxr::AbiBoundary<xrEnumerateBoundSourcesForAction_runtime>::Invoke},
    {"xrGetInputSourceLocalizedName", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetInputSourceLocalizedName_runtime>::Invoke},
    // Utility functions
    {"xrResultToString", (PFN_xrVoidFunction)simxr::AbiBoundary<xrResultToString_runtime>::Invoke},
    {"xrStructureTypeToString", (PFN_xrVoidFunction)simxr::AbiBoundary<xrStructureTypeToString_runtime>::Invoke},
    {"xrGetReferenceSpaceBoundsRect", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetReferenceSpaceBoundsRect_runtime>::Invoke},
    {"xrGetViewConfigurationProperties", (PFN_xrVoidFunction)simxr::AbiBoundary<xrGetViewConfigurationProperties_runtime>::Invoke},
    // Haptic functions
    {"xrApplyHapticFeedback", (PFN_xrVoidFunction)simxr::AbiBoundary<xrApplyHapticFeedback_runtime>::Invoke},
    {"xrStopHapticFeedback", (PFN_xrVoidFunction)simxr::AbiBoundary<xrStopHapticFeedback_runtime>::Invoke},
    // Time conversion functions
    {"xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrConvertWin32PerformanceCounterToTimeKHR_runtime>::Invoke},
    {"xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction)simxr::AbiBoundary<xrConvertTimeToWin32PerformanceCounterKHR_runtime>::Invoke},
};

static XrResult XRAPI_PTR xrGetInstanceProcAddr_runtime(XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
	if (!function)
		return XR_ERROR_VALIDATION_FAILURE;
	*function = nullptr;
	if (!name)
		return XR_ERROR_VALIDATION_FAILURE;
	if (instance != XR_NULL_HANDLE && !RuntimeState().FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	static const simxr::DispatchTable table(kFnTable, Extensions());
	if (!table.CoversAdvertisedCommands())
		return XR_ERROR_RUNTIME_FAILURE;
	return table.Resolve(name, instance != XR_NULL_HANDLE ? &rt::g_instance.core->Extensions() : nullptr, *function);
}
