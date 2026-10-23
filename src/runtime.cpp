// Minimal OpenXR Simulator Runtime (D3D11/D3D12/OpenGL)
// - Implements enough of the runtime interface to let OpenXR apps start and render into runtime-owned swapchains
// - Opens a desktop window and presents the app's submitted images side-by-side
// - Supports D3D11, D3D12, and OpenGL graphics APIs

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN

#include "Pch.h"

#include <windows.h>
#include "simxr/Version.h"
#include <limits>
#include <windowsx.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi.h>
#include <dxgi1_6.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "Math.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "control/legacy/backends/vulkan/VulkanLegacyCapture.h"
#include "control/legacy/backends/vulkan/WindowsVulkanCaptureChannel.h"
#include "flicker_detector.h"
#include "graphics/LayerBuilder.h"
#include "graphics/SwapchainAccess.h"
#include "graphics/SwapchainWaits.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "graphics/backends/vulkan/VulkanSessionGraphics.h"
#include "graphics/backends/windows/WindowsSessionGraphics.h"
#include "graphics/backends/windows/WindowsGraphicsCapabilities.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
#include "platform/windows/OpenGLSwapchain.h"
#include "platform/windows/WindowsClock.h"
#include "presentation/FrameScheduler.h"
#include "runtime/DispatchTable.h"
#include "runtime/EntryPoints.h"
#include "runtime/EventQueue.h"
#include "runtime/Instance.h"
#include "runtime/Runtime.h"
#include "runtime/Session.h"
#include "runtime/SimulationExtensions.h"
#include "runtime/SpaceQueries.h"
#include "runtime/validation/LayerValidation.h"
#include "runtime/validation/StructureChain.h"
#include "simulation/LegacyControllerMotion.h"
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
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#include <bit>
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
#include "control/legacy/WindowsRuntimeSettings.h"
namespace settings = simxr::legacy::settings;

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

// Runtime state
namespace rt {

struct Swapchain;

static std::mutex g_coreMutex;
static std::mutex g_instanceLifecycleMutex;

static std::mutex g_vulkanMutex;

struct Instance {
	std::unique_ptr<simxr::VulkanInstanceGraphics> vulkan = std::make_unique<simxr::VulkanInstanceGraphics>();
	XrInstance handle{XR_NULL_HANDLE};
    std::shared_ptr<simxr::Instance> core;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> controllerMotion;
    std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
    std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
    std::shared_ptr<flicker::Diagnostics> diagnostics;
};

struct Session {
    XrSession handle{XR_NULL_HANDLE};
    std::shared_ptr<simxr::Session> core;
    std::shared_ptr<simxr::legacy::LegacyControllerMotion> controllerMotion;
    std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
    std::shared_ptr<mcp::WindowsLegacyCapture> compatibilityCapture;
    std::shared_ptr<flicker::Diagnostics> diagnostics;
    ComPtr<ID3D11Device> d3d11Device;
    // DX12 support
    ComPtr<ID3D12Device> d3d12Device;
    ComPtr<ID3D12CommandQueue> d3d12Queue;
    std::atomic<bool> usesD3D12{false};
	std::shared_ptr<simxr::VulkanSessionGraphics> vulkan;
	std::shared_ptr<simxr::WindowsSessionGraphics> nativeGraphics;
	std::shared_ptr<simxr::SwapchainWaits> nativeWaits;
	bool nativeClosing = false;
	// OpenGL support
    HDC glDC{nullptr};
    HGLRC glRC{nullptr};
    bool usesOpenGL{false};
};

struct Swapchain {
    XrSwapchain handle{(XrSwapchain)1};
    DXGI_FORMAT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    uint32_t width{0}, height{0}, arraySize{2};
    uint32_t mipCount{1};
    uint32_t sampleCount{1};
    // Backend type and images
    enum class Backend { D3D11, D3D12, OpenGL } backend{Backend::D3D11};
    std::vector<ComPtr<ID3D11Texture2D>> images;      // D3D11 path
    std::vector<ComPtr<ID3D12Resource>> images12;     // D3D12 path
    // State the app is required to release images in, per XR_KHR_D3D12_enable:
    // RENDER_TARGET for colour, DEPTH_WRITE for depth.
    D3D12_RESOURCE_STATES releaseState12{D3D12_RESOURCE_STATE_COMMON};
	std::unique_ptr<simxr::windows::OpenGLSwapchain> glImages;
	std::shared_ptr<simxr::SwapchainAccess> access;
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
static std::atomic<bool> g_useCustomFov{false};
static float g_eyeFovL[2] = { 0, 0 };
static float g_eyeFovR[2] = { 0, 0 };
static float g_eyeFovU[2] = { 0, 0 };
static float g_eyeFovD[2] = { 0, 0 };

static std::atomic<bool> g_useCustomIpd{false};
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
    const auto state = settings::ReadState();
    // Shared with settings::ActiveFovAspect, which snaps the recommended render target to the
    // shape of whatever this returns.
    if (!state.useAsymmetricFov || state.headsetProfile == settings::HeadsetProfile::GenericSymmetric) {
        int fovDeg = state.fovDegrees;
        if (fovDeg <= 0 || fovDeg > 180) fovDeg = 90;
        float fovRadians = fovDeg * 0.5f * 3.14159265f / 180.0f;
        // XrFovf fields are angles in radians, not tangent-space extents.
        return XrFovf{ -fovRadians, fovRadians, fovRadians, -fovRadians };
    }

    const settings::EyeFov& e = settings::GetHeadsetSpec(state.headsetProfile).eye[eyeIndex == 0 ? 0 : 1];
    return MakeFovDeg(e.angleLeft, e.angleRight, e.angleUp, e.angleDown);
}

static float GetUiIpdMeters() {
    return (std::max)(0.0f, (std::min)(0.2f, settings::ReadState().ipdMeters));
}

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

} // namespace rt


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
			auto nextService = std::make_unique<simxr::control::ControlService>(
				RuntimeState(), 16,
				simxr::control::CreateVulkanImageInspectionProvider(
					[](XrSession handle)
					{
						std::lock_guard lock(rt::g_vulkanMutex);
						if (rt::g_session.handle == handle && rt::g_session.nativeGraphics)
							return rt::g_session.nativeGraphics->Capabilities();
						return rt::g_session.handle == handle && rt::g_session.vulkan
								   ? rt::g_session.vulkan->Capabilities()
								   : simxr::control::ImageCapabilities{};
					}));
			auto nextServer = std::make_unique<simxr::control::LocalControlServer>(*nextService, path, 16);
            service = std::move(nextService);
            server = std::move(nextServer);
			Logf("[SimXR] Local control endpoint: %s (control/inspection; session-dependent native GPU transport)",
				 path.c_str());
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
        // process.
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
		const auto enumerated = f->EnumAdapters1(i, a.GetAddressOf());
		if (enumerated == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(enumerated)) return XR_ERROR_RUNTIME_FAILURE;
        DXGI_ADAPTER_DESC1 d{};
		if (FAILED(a->GetDesc1(&d))) return XR_ERROR_RUNTIME_FAILURE;
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
		const auto enumerated = f->EnumAdapters1(i, adapt.GetAddressOf());
		if (enumerated == DXGI_ERROR_NOT_FOUND) break;
		if (FAILED(enumerated)) return XR_ERROR_RUNTIME_FAILURE;
        DXGI_ADAPTER_DESC1 d{};
		if (FAILED(adapt->GetDesc1(&d))) return XR_ERROR_RUNTIME_FAILURE;
        
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

	// Canonical GLSL and direct-state-access image storage require OpenGL 4.5.
    req->minApiVersionSupported = XR_MAKE_VERSION(4, 5, 0);
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
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->Requirements(system, requirements);
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

static XrResult XRAPI_PTR xrGetVulkanInstanceExtensionsKHR_runtime(
    XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* countOutput, char* buffer) {
	auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->InstanceExtensions(system, capacity, countOutput, buffer);
}

static XrResult XRAPI_PTR xrGetVulkanDeviceExtensionsKHR_runtime(
    XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* countOutput, char* buffer) {
	auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->DeviceExtensions(system, capacity, countOutput, buffer);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsDeviceKHR_runtime(
    XrInstance instance, XrSystemId system, VkInstance vkInstance, VkPhysicalDevice* out) {
	auto result = RequireGraphicsExtension(instance, system, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->PhysicalDevice(system, vkInstance, out);
}

static XrResult XRAPI_PTR xrGetVulkanGraphicsDevice2KHR_runtime(
    XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* getInfo, VkPhysicalDevice* out) {
	auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	if (!getInfo || getInfo->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->PhysicalDevice(getInfo->systemId, getInfo->vulkanInstance, out);
}

// Thin passthrough: the app's own vkCreateInstance runs, with the interop extensions the
// runtime needs appended to whatever it asked for.
static XrResult XRAPI_PTR xrCreateVulkanInstanceKHR_runtime(
    XrInstance instance, const XrVulkanInstanceCreateInfoKHR* createInfo, VkInstance* vulkanInstance, VkResult* vulkanResult) {
	auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->CreateInstance(createInfo, vulkanInstance, vulkanResult);
}

static XrResult XRAPI_PTR xrCreateVulkanDeviceKHR_runtime(
    XrInstance instance, const XrVulkanDeviceCreateInfoKHR* createInfo, VkDevice* vulkanDevice, VkResult* vulkanResult) {
	auto result = RequireExtension(instance, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(rt::g_vulkanMutex);
	return rt::g_instance.vulkan->CreateDevice(createInfo, vulkanDevice, vulkanResult);
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

    // Restore persisted headset defaults before application view-configuration queries.
    static bool s_settingsLoaded = false;
    if (!s_settingsLoaded) {
        settings::LoadSettings(mcp::GetSimulatorDataDirectory());
        s_settingsLoaded = true;
        Logf("[SimXR] Settings restored: profile=%ls ipd=%dmm asymmetric=%d",
             settings::GetHeadsetProfileShortName(), settings::GetIpdMillimeters(),
             (int)settings::ReadState().useAsymmetricFov);
    }

    if (const auto* profile = std::getenv("SIMXR_HEADSET_PROFILE")) {
        const auto index = settings::FindHeadsetSpec(profile);
        if (index < 0) return XR_ERROR_INITIALIZATION_FAILED;
        settings::SetHeadsetProfile(static_cast<settings::HeadsetProfile>(index));
    }

    for (auto extension : requested)
        Logf("[SimXR] enabledExtension=%s", extension);
    const auto directory = mcp::GetSimulatorDataDirectory();
    newInstance.diagnostics = std::make_shared<flicker::Diagnostics>(directory);
    newInstance.compatibilityFiles = std::make_shared<mcp::WindowsLegacyChannel>(directory);
    newInstance.compatibilityCapture = std::make_shared<mcp::WindowsLegacyCapture>(newInstance.compatibilityFiles);
    rt::Instance previousInstance;
    {
        std::lock_guard lock(rt::g_coreMutex);
        previousInstance = std::move(rt::g_instance);
        rt::g_instance = std::move(newInstance);
    }
    { std::lock_guard lock(g_LogMutex); g_logChannel = rt::g_instance.compatibilityFiles; }
    ControlEndpoint().Start();
    *instance = core;
    creation.committed = true;
    Log("[SimXR] xrCreateInstance: SUCCESS");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroyInstance_runtime(XrInstance instance) {
    std::lock_guard lifecycleLock(rt::g_instanceLifecycleMutex);
    Logf("[SimXR] xrDestroyInstance called: instance=%p", instance);

    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
	// Keep the registered handles available if native retirement needs a retry.
	if (instance == rt::g_instance.handle && rt::g_session.handle != XR_NULL_HANDLE)
	{
		const auto result = DestroyNativeSession(rt::g_session.handle);
		if (XR_FAILED(result)) return result;
	}
	if (!RuntimeState().Game().Snapshot().game) ControlEndpoint().Close();
	const auto result = RuntimeState().DestroyInstance(instance);
	if (XR_FAILED(result)) return result;
    if (instance == rt::g_instance.handle) {

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
        simxr::StopVulkanImageDelivery();
    }

    Log("[SimXR] xrDestroyInstance: SUCCESS - Returning XR_SUCCESS");
    Log("[SimXR] ========== Instance Destroyed - Waiting for new instance ==========");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetInstanceProperties_runtime(XrInstance instance, XrInstanceProperties* props) {
    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!props || props->type != XR_TYPE_INSTANCE_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    props->runtimeVersion = simxr::RuntimeVersion;
    strncpy(props->runtimeName, "OpenXR Simulator Runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
    props->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
    Log("[SimXR] xrGetInstanceProperties: returning OpenXR Simulator Runtime");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetSystem_runtime(XrInstance instance, const XrSystemGetInfo* info, XrSystemId* systemId) {
    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !systemId || info->type != XR_TYPE_SYSTEM_GET_INFO) return XR_ERROR_VALIDATION_FAILURE;
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

static XrResult ValidateViewConfiguration(const simxr::EnabledExtensions& extensions, XrSystemId system, XrViewConfigurationType type)
{
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ||
		(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
		 extensions.Contains(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)))
		return XR_SUCCESS;
	return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
}

static XrResult XRAPI_PTR xrEnumerateViewConfigurations_runtime(
	XrInstance instance, XrSystemId system, uint32_t capacity, uint32_t* count, XrViewConfigurationType* types)
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

static XrResult XRAPI_PTR xrEnumerateViewConfigurationViews_runtime(
	XrInstance instance, XrSystemId system, XrViewConfigurationType viewType,
	uint32_t capacity, uint32_t* count, XrViewConfigurationView* views)
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

static XrResult XRAPI_PTR xrEnumerateEnvironmentBlendModes_runtime(
	XrInstance instance, XrSystemId system, XrViewConfigurationType viewType,
	uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* modes)
{
	const auto core = RuntimeState().FindInstance(instance);
	if (!core)
		return XR_ERROR_HANDLE_INVALID;
	const auto validation = ValidateViewConfiguration(core->Extensions(), system, viewType);
	if (XR_FAILED(validation))
		return validation;
	if (!count || (capacity && !modes)) return XR_ERROR_VALIDATION_FAILURE;
	*count = 1;
	if (capacity) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
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
	std::shared_ptr<mcp::WindowsLegacyChannel> compatibilityFiles;
	std::shared_ptr<flicker::Diagnostics> diagnostics;
	{
        std::lock_guard lock(rt::g_coreMutex);
        if (rt::g_instance.handle != instance) return XR_ERROR_HANDLE_INVALID;
        controllerMotion = rt::g_instance.controllerMotion;
		compatibilityFiles = rt::g_instance.compatibilityFiles;
		diagnostics = rt::g_instance.diagnostics;
	}
    if (!controllerMotion) return XR_ERROR_HANDLE_INVALID;
    if (rt::g_session.handle != XR_NULL_HANDLE) return XR_ERROR_LIMIT_REACHED;
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
	std::optional<simxr::VulkanBinding> vulkanBinding;
	auto publish = [&]() -> XrResult
    {
        auto result = RuntimeState().CreateSession(instance, creation.core, std::make_shared<simxr::WindowsClock>());
        if (XR_FAILED(result)) return result;
		if (vulkanBinding)
		{
			std::lock_guard lock(rt::g_vulkanMutex);
			std::unique_ptr<simxr::VulkanSessionGraphics> graphics;
			const auto* validation = std::getenv("SIMXR_VULKAN_VALIDATION");
			result = simxr::VulkanSessionGraphics::Create(*vulkanBinding, creation.core.Shared(),
														  validation && std::strcmp(validation, "1") == 0, graphics);
			if (XR_FAILED(result))
				return result;
			graphics->SetObserver(std::make_unique<simxr::VulkanLegacyCapture>(
				graphics->Device(), creation.core.Shared(),
				std::make_shared<simxr::WindowsVulkanCaptureChannel>(
					compatibilityFiles, rt::g_instance.compatibilityCapture, creation.core.Shared(),
					[]
					{
						mcp::HeadPoseCommand head;
						head.x = rt::g_headPos.x;
						head.y = rt::g_headPos.y;
						head.z = rt::g_headPos.z;
						head.yaw = rt::g_headYaw;
						head.pitch = rt::g_headPitch;
						head.roll = rt::g_headRoll;
						return head;
					},
					[](const mcp::HeadPoseCommand& pose)
					{
						rt::g_headPos = {pose.x, pose.y, pose.z};
						rt::g_headYaw = pose.yaw;
						rt::g_headPitch = pose.pitch;
						if (pose.hasRoll)
							rt::g_headRoll = pose.roll;
					}),
				diagnostics));
			rt::g_session.vulkan = std::move(graphics);
		}
		else if (rt::g_session.d3d11Device || rt::g_session.d3d12Device || rt::g_session.usesOpenGL)
		{
			std::lock_guard lock(rt::g_vulkanMutex);
			try
			{
				rt::g_session.nativeWaits = std::make_shared<simxr::SwapchainWaits>();
				rt::g_session.nativeClosing = false;
				if (rt::g_session.usesOpenGL)
					rt::g_session.nativeGraphics = std::make_shared<simxr::WindowsSessionGraphics>(
						rt::g_session.glDC, rt::g_session.glRC, creation.core.Shared());
				else if (rt::g_session.usesD3D12)
					rt::g_session.nativeGraphics = std::make_shared<simxr::WindowsSessionGraphics>(
						rt::g_session.d3d12Device.Get(), rt::g_session.d3d12Queue.Get(), creation.core.Shared());
				else
					rt::g_session.nativeGraphics = std::make_shared<simxr::WindowsSessionGraphics>(
						rt::g_session.d3d11Device.Get(), creation.core.Shared());
			}
			catch (const std::exception& error)
			{
				Logf("[SimXR] Native companion graphics unavailable: %s", error.what());
				return XR_ERROR_GRAPHICS_DEVICE_INVALID;
			}
		}
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
            
            rt::g_session.d3d11Device = b->device;
            rt::g_session.usesD3D12 = false;
            rt::g_session.d3d12Device.Reset();
            rt::g_session.d3d12Queue.Reset();
            // The shared adapter publishes GPU images to the companion.
            return publish();
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            auto enabled = RequireExtension(instance, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
            if (XR_FAILED(enabled)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            const auto* b12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
            if (!b12->device || !b12->queue) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            rt::g_session.usesD3D12 = true;
            rt::g_session.d3d12Device = b12->device;
            rt::g_session.d3d12Queue = b12->queue;
            rt::g_session.d3d11Device.Reset();
            return publish();
        } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            if (!rt::g_instance.core ||
                (!rt::g_instance.core->Extensions().Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
                 !rt::g_instance.core->Extensions().Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)))
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            // XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR is an alias of this, so one branch serves
            // XR_KHR_vulkan_enable and XR_KHR_vulkan_enable2 alike.
			simxr::VulkanBinding binding{};
			{
				std::lock_guard lock(rt::g_vulkanMutex);
				const auto result = rt::g_instance.vulkan->SessionBinding(*info, binding);
				if (XR_FAILED(result))
					return result;
			}
			vulkanBinding = binding;
			rt::g_session.usesD3D12 = false;
			rt::g_session.usesOpenGL = false;
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
            rt::g_session.d3d11Device.Reset();
            rt::g_session.d3d12Device.Reset();
            rt::g_session.d3d12Queue.Reset();
            return publish();
        }
        entry = entry->next;
    }
    Log("[SimXR] xrCreateSession: ERROR - No supported graphics binding found (D3D11/D3D12/OpenGL)");
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}

static XrResult CloseVulkanSession()
{
	std::shared_ptr<simxr::VulkanSessionGraphics> graphics;
	std::shared_ptr<simxr::Session> core;
	{
		std::lock_guard lock(rt::g_vulkanMutex);
		graphics = rt::g_session.vulkan;
		if (!graphics)
			return XR_SUCCESS;
		graphics->BeginClose();
		core = rt::g_session.core;
	}
	if (core)
		core->Close();
	auto result = graphics->Drain();
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(rt::g_vulkanMutex);
	result = graphics->FinishClose();
	if (XR_SUCCEEDED(result))
		rt::g_session.vulkan.reset();
	return result;
}

static XrResult DestroyNativeSession(XrSession s) {
    Logf("[SimXR] xrDestroySession called (handle=%llu)", (unsigned long long)s);
	std::shared_ptr<simxr::Session> core;
	{
		std::lock_guard lock(rt::g_coreMutex);
		if (s != rt::g_session.handle)
			return XR_ERROR_HANDLE_INVALID;
		core = rt::g_session.core;
	}
	std::shared_ptr<simxr::WindowsSessionGraphics> nativeGraphics;
	std::shared_ptr<simxr::SwapchainWaits> waits;
	{
		std::lock_guard lock(rt::g_vulkanMutex);
		nativeGraphics = rt::g_session.nativeGraphics;
		waits = rt::g_session.nativeWaits;
		rt::g_session.nativeClosing = true;
		if (waits)
			waits->Close();
		for (const auto& [handle, chain] : rt::g_swapchains)
			chain.access->Close();
	}
	if (core)
		core->Close();
	if (waits && !waits->Drain(std::chrono::seconds(5)))
		return XR_ERROR_RUNTIME_FAILURE;
	if (nativeGraphics)
	{
		const auto closed = nativeGraphics->Close();
		if (XR_FAILED(closed))
			return closed;
	}
	const auto vulkanClosed = CloseVulkanSession();
	if (XR_FAILED(vulkanClosed))
		return vulkanClosed;
	for (;;)
	{
		XrSwapchain handle;
		{
			std::lock_guard lock(rt::g_vulkanMutex);
			if (rt::g_swapchains.empty())
				break;
			handle = rt::g_swapchains.begin()->first;
		}
		const auto result = xrDestroySwapchain_runtime(handle);
		if (XR_FAILED(result))
			return result;
	}
	{
		std::lock_guard lock(rt::g_vulkanMutex);
		rt::g_session.nativeGraphics.reset();
		rt::g_session.nativeWaits.reset();
		rt::g_session.d3d11Device.Reset();
		rt::g_session.usesD3D12 = false;
		rt::g_session.d3d12Device.Reset();
		rt::g_session.d3d12Queue.Reset();
		rt::g_session.usesOpenGL = false;
		rt::g_session.glDC = nullptr;
		rt::g_session.glRC = nullptr;
	}
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
    Log("[SimXR] xrDestroySession: SUCCESS");
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrDestroySession_runtime(XrSession handle)
{
	if (!RuntimeState().FindSession(handle)) return XR_ERROR_HANDLE_INVALID;
	const auto result = DestroyNativeSession(handle);
	if (XR_FAILED(result)) return result;
	return RuntimeState().DestroySession(handle);
}


static XrResult XRAPI_PTR xrEnumerateSwapchainFormats_runtime(XrSession session, uint32_t capacity, uint32_t* count, int64_t* formats) {
	std::lock_guard lock(rt::g_vulkanMutex);
	{
		std::lock_guard coreLock(rt::g_coreMutex);
		if (session == XR_NULL_HANDLE || session != rt::g_session.handle)
			return XR_ERROR_HANDLE_INVALID;
	}
	if (rt::g_session.vulkan)
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
	if (rt::g_session.usesOpenGL)
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

	if (!count || (capacity && !formats)) return XR_ERROR_VALIDATION_FAILURE;
	*count = formatCount;
	if (!capacity) return XR_SUCCESS;
	if (capacity < formatCount) return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(std::begin(supportedFormats), std::end(supportedFormats), formats);
	return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrCreateSwapchain_runtime(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* sc) {
	std::lock_guard lock(rt::g_vulkanMutex);
	{
		std::lock_guard coreLock(rt::g_coreMutex);
		if (session == XR_NULL_HANDLE || session != rt::g_session.handle)
			return XR_ERROR_HANDLE_INVALID;
	}
	if (rt::g_session.vulkan)
	{
		if (!ci || !sc)
			return XR_ERROR_VALIDATION_FAILURE;
		return rt::g_session.vulkan->CreateSwapchain(*ci, *sc);
	}
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
    if (!ci || !sc) return XR_ERROR_VALIDATION_FAILURE;
	if (rt::g_session.nativeClosing) return XR_ERROR_SESSION_LOST;
	if (ci->type != XR_TYPE_SWAPCHAIN_CREATE_INFO || !ci->width || !ci->height || !ci->arraySize ||
		!ci->sampleCount || !ci->mipCount || !ci->usageFlags || (ci->faceCount != 1 && ci->faceCount != 6))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!rt::g_session.usesOpenGL)
	{
		constexpr XrSwapchainUsageFlags supportedUsage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
			XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT |
			XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
			XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
		if ((ci->createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
			(ci->usageFlags & ~supportedUsage) || ci->faceCount != 1)
			return XR_ERROR_FEATURE_UNSUPPORTED;
		if (!std::has_single_bit(ci->sampleCount) || ci->sampleCount > 32 ||
			(ci->sampleCount != 1 && ci->mipCount != 1) ||
			ci->mipCount > static_cast<uint32_t>(std::bit_width(std::max(ci->width, ci->height))) || ci->arraySize > 2048)
			return XR_ERROR_VALIDATION_FAILURE;
	}
    rt::Swapchain chain{}; 
    chain.handle = simxr::AllocateHandle<XrSwapchain>();
    chain.format = (DXGI_FORMAT)ci->format;  // Store the original requested format
    chain.width = ci->width; 
    chain.height = ci->height; 
    chain.arraySize = ci->arraySize;
    // Create textures on appropriate backend
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
            rd.MipLevels = ci->mipCount;
            chain.mipCount = rd.MipLevels;
            rd.Format = chain.format;
            rd.SampleDesc.Count = ci->sampleCount;
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
            chain.releaseState12 = init;
        }
        chain.access = std::make_shared<simxr::SwapchainAccess>(chain.imageCount, (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0);
        rt::g_swapchains.emplace(chain.handle, std::move(chain));
        *sc = chain.handle;
        Logf("[SimXR] xrCreateSwapchain(D3D12): sc=%p fmt=%d %ux%u array=%u samples=%u", *sc, (int)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
        return XR_SUCCESS;
    }
	if (rt::g_session.usesOpenGL)
	{
		const bool staticImage = (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
		chain.backend = rt::Swapchain::Backend::OpenGL;
		chain.sampleCount = ci->sampleCount;
		chain.imageCount = staticImage ? 1 : 3;
		chain.mipCount = ci->mipCount;
		chain.access = std::make_shared<simxr::SwapchainAccess>(chain.imageCount, staticImage);
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
    td.ArraySize = chain.arraySize;
    td.MipLevels = ci->mipCount;
    chain.mipCount = td.MipLevels;
    td.SampleDesc.Count = ci->sampleCount;
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
    chain.access = std::make_shared<simxr::SwapchainAccess>(chain.imageCount, (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0);
        rt::g_swapchains.emplace(chain.handle, std::move(chain));
    *sc = chain.handle;
    Logf("[SimXR] xrCreateSwapchain: sc=%p fmt=%d %ux%u array=%u samples=%u", *sc, (int)ci->format, ci->width, ci->height, ci->arraySize, ci->sampleCount);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrEnumerateSwapchainImages_runtime(XrSwapchain sc, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
	std::lock_guard lock(rt::g_vulkanMutex);
	if (rt::g_session.vulkan)
	{
		const auto chain = rt::g_session.vulkan->FindSwapchain(sc);
		return chain ? chain->EnumerateImages(capacity, count, images) : XR_ERROR_HANDLE_INVALID;
	}
	auto it = rt::g_swapchains.find(sc); if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    if (it->second.backend == rt::Swapchain::Backend::D3D12) {
        const uint32_t n = (uint32_t)it->second.images12.size();
		if (!count || (capacity && !images)) return XR_ERROR_VALIDATION_FAILURE;
		*count = n;
		if (!capacity) return XR_SUCCESS;
		if (capacity < n) return XR_ERROR_SIZE_INSUFFICIENT;
		auto* output = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
		for (uint32_t index = 0; index < n; ++index)
			if (output[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;
		for (uint32_t index = 0; index < n; ++index)
			output[index].texture = it->second.images12[index].Get();
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
		if (!count || (capacity && !images)) return XR_ERROR_VALIDATION_FAILURE;
		*count = n;
		if (!capacity) return XR_SUCCESS;
		if (capacity < n) return XR_ERROR_SIZE_INSUFFICIENT;
		auto* output = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
		for (uint32_t index = 0; index < n; ++index)
			if (output[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
		for (uint32_t index = 0; index < n; ++index)
			output[index].texture = it->second.images[index].Get();
        Logf("[SimXR] xrEnumerateSwapchainImages(D3D11): sc=%p count=%u", sc, n);
        return XR_SUCCESS;
    }
}

static XrResult XRAPI_PTR xrAcquireSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageAcquireInfo* info,
														  uint32_t* index)
{
	std::lock_guard lock(rt::g_vulkanMutex);
	if (rt::g_session.vulkan)
	{
		const auto chain = rt::g_session.vulkan->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		if (!index || (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO))
			return XR_ERROR_VALIDATION_FAILURE;
		return rt::g_session.vulkan->Acquire(*chain.Shared(), *index);
	}
	auto it = rt::g_swapchains.find(sc); if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    auto& ch = it->second;
    if (!index || (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO)) return XR_ERROR_VALIDATION_FAILURE;
    uint32_t i;
    auto result = ch.access->Acquire(i);
    if (XR_FAILED(result)) return result;
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
static XrResult XRAPI_PTR xrWaitSwapchainImage_runtime(XrSwapchain sc, const XrSwapchainImageWaitInfo* info)
{
	std::unique_lock lock(rt::g_vulkanMutex);
	if (auto graphics = rt::g_session.vulkan)
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
	const auto found = rt::g_swapchains.find(sc);
	if (found == rt::g_swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO || info->timeout < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto access = found->second.access;
	const auto graphics = rt::g_session.nativeGraphics;
	const auto waits = rt::g_session.nativeWaits;
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
	std::lock_guard lock(rt::g_vulkanMutex);
	if (rt::g_session.vulkan)
	{
		const auto chain = rt::g_session.vulkan->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		return chain->Release();
	}
	auto it = rt::g_swapchains.find(sc);
    if (it == rt::g_swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    auto& ch = it->second;
    if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    auto result = ch.access->Release();
    if (XR_FAILED(result)) return result;

    static int releaseCount = 0;
    bool shouldLog = (++releaseCount <= 10);
    if (shouldLog || (g_logVerbose && releaseCount % 60 == 1)) {
        Logf("[SimXR] xrReleaseSwapchainImage: sc=%p released=%u", sc, *ch.access->ReleasedImage());

    }
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrPollEvent_runtime(XrInstance instance, XrEventDataBuffer* buffer) {
    if (!RuntimeState().FindInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    return rt::g_instance.core->Events().Poll(*buffer);
}

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
        settings::GetRenderResolution(baseline.views.outerWidth, baseline.views.outerHeight);
    }
    auto configured = core->ConfigureCommands(seed, baseline, false);
    if (XR_FAILED(configured)) return configured;
	simxr::PresentationPolicy policy = simxr::PresentationPolicy::Repeat;
	{
		std::lock_guard lock(rt::g_vulkanMutex);
		if (rt::g_session.vulkan || rt::g_session.nativeGraphics)
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
			if (policy == simxr::PresentationPolicy::Rotational && rt::g_session.vulkan &&
				!rt::g_session.vulkan->SupportsReprojection())
				return XR_ERROR_FEATURE_UNSUPPORTED;
		}
	}
	auto result = core->Begin(*info, config, policy);
    return result;
}
static XrResult XRAPI_PTR xrEndSession_runtime(XrSession handle) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    auto result = core->End({simxr::SteadyClock{}.Now().time_since_epoch()});
    return result;
}
static XrResult XRAPI_PTR xrRequestExitSession_runtime(XrSession handle) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    auto result = core->RequestExit({simxr::SteadyClock{}.Now().time_since_epoch()});
    return result;
}
static XrResult XRAPI_PTR xrWaitFrame_runtime(XrSession session, const XrFrameWaitInfo* info, XrFrameState* s) {
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



		const double periodSec = std::chrono::duration<double>(admitted.displayPeriod).count();

		// Handle WASD keyboard input for movement (relative to head orientation).
		//
		// The PID check is what makes AppForeground the default: the runtime is in-process with
		// the host, so the game's own window counts as ours. PreviewFocused was once the only
		// behaviour and left WASD dead in any game that takes the foreground on launch.
		// Note: a game binding W/S/A/D itself will also act on them while it is focused.
		const float maxPitch = 1.5f; // ~85 degrees, short of gimbal lock
		rt::g_headPitch = (std::max)(-maxPitch, (std::min)(maxPitch, rt::g_headPitch));

		const settings::InputPolling inputPolling = settings::ReadState().inputPolling;
		bool inputActive = (inputPolling == settings::InputPolling::Background);
		const HWND foregroundWindow = inputActive ? nullptr : GetForegroundWindow();
		if (foregroundWindow && inputPolling == settings::InputPolling::AppForeground)
		{
			DWORD foregroundProcess = 0;
			GetWindowThreadProcessId(foregroundWindow, &foregroundProcess);
			inputActive = foregroundProcess == GetCurrentProcessId();
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
			const float moveSpeed = settings::GetMoveSpeed(shiftHeld); // meters per second
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

		compatibilityFiles->TakeWrittenScreenshot();

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
				const int idx = settings::FindHeadsetSpec(prof.name);
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
					const settings::HeadsetSpec& spec = settings::kHeadsetSpecs[idx];
					rt::g_useCustomFov = true;
					for (int i = 0; i < 2; ++i)
					{
						const settings::EyeFov& e = spec.eye[i];
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

			// Display layout belongs to the companion; the removed runtime mirror cannot apply this command.
			if (commands.anaglyph.valid)
				compatibilityFiles->Acknowledge("anaglyph", false);

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
		settings::GetRenderResolution(settings.outerWidth, settings.outerHeight);
		settings.ipd = rt::g_useCustomIpd ? rt::g_customIpd : rt::GetUiIpdMeters();
		settings.outerFov = {rt::GetViewFov(0), rt::GetViewFov(1)};
		core->PublishTracking(admitted.displayTime, tracking, settings, admitted.id);

		s->predictedDisplayPeriod = admitted.displayPeriod.count();
		s->predictedDisplayTime = admitted.displayTime.value.count();
		const auto state = core->Snapshot().state;
		s->shouldRender = state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED;
		return XR_SUCCESS;
	};
	return core->WaitFrame(prediction, publishNative);
}
static XrResult XRAPI_PTR xrBeginFrame_runtime(XrSession session, const XrFrameBeginInfo* info) {
    if (session == XR_NULL_HANDLE) return XR_ERROR_HANDLE_INVALID;
    auto core = rt::RetainSession(session);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    return core->BeginFrame();
}

static XrResult XRAPI_PTR xrEndFrame_runtime(XrSession session, const XrFrameEndInfo* info) {
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
	{
		return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
	}
	{
		std::lock_guard lock(rt::g_vulkanMutex);
		{
			std::lock_guard coreLock(rt::g_coreMutex);
			if (core != rt::g_session.core)
				return XR_ERROR_HANDLE_INVALID;
		}
		if (rt::g_session.nativeGraphics)
		{
			if (rt::g_session.nativeClosing)
				return XR_ERROR_SESSION_LOST;
			// Retain the exact image selected before submission; callbacks never look it up again.
			struct SourceImage
			{
				XrSwapchain handle;
				std::shared_ptr<simxr::SwapchainAccess> access;
				std::uint32_t index;
			};
			std::vector<SourceImage> sources;
			return rt::g_session.nativeGraphics->EndFrame(*info,
				[](XrSwapchain handle) -> std::optional<simxr::SwapchainDescription>
				{
					const auto found = rt::g_swapchains.find(handle);
					if (found == rt::g_swapchains.end()) return {};
					const auto& chain = found->second;
					return simxr::SwapchainDescription{chain.width, chain.height, chain.arraySize,
						chain.access->ReleasedImage().has_value()};
				},
				[&sources](XrSwapchain handle) -> simxr::WindowsSessionGraphics::Source
				{
					const auto found = rt::g_swapchains.find(handle);
					if (found == rt::g_swapchains.end()) return {};
					const auto& chain = found->second;
					const auto image = chain.access->ReleasedImage();
					if (!image) return {};
					if (std::none_of(sources.begin(), sources.end(),
						[handle](const SourceImage& source) { return source.handle == handle; }))
						sources.push_back({handle, chain.access, *image});
					if (chain.backend == rt::Swapchain::Backend::OpenGL)
					{
						if (!chain.glImages || *image >= chain.glImages->Images().size()) return {};
						return simxr::OpenGLCompositionSource{chain.glImages->Images()[*image],
							chain.glImages->Format(), chain.width, chain.height, chain.arraySize, chain.mipCount, chain.sampleCount};
					}
					if (chain.backend == rt::Swapchain::Backend::D3D12)
					{
						if (*image >= chain.images12.size()) return {};
						return simxr::D3D12CompositionSource{chain.images12[*image].Get(),
							static_cast<DXGI_FORMAT>(chain.format), chain.releaseState12};
					}
					if (*image >= chain.images.size()) return {};
					return simxr::D3D11CompositionSource{chain.images[*image].Get(), static_cast<DXGI_FORMAT>(chain.format)};
				},
				[&sources](XrSwapchain handle, const std::shared_ptr<const simxr::GpuCompletion>& completion)
				{
					const auto source = std::find_if(sources.begin(), sources.end(),
						[handle](const SourceImage& image) { return image.handle == handle; });
					if (source != sources.end())
						source->access->MarkUsed(source->index, completion);
				});
		}

		if (rt::g_session.vulkan)
		{
			const auto result = rt::g_session.vulkan->EndFrame(*info);
			if (XR_SUCCEEDED(result))
			{
				const auto snapshot = core->Snapshot();
				const auto views = core->ViewsAt(simxr::RuntimeTime{simxr::Duration{info->displayTime}});
				const auto width = views.views.empty() ? 0u : views.views.front().description.width * 2;
				const auto height = views.views.empty() ? 0u : views.views.front().description.height;
				compatibilityCapture->SetFrame(snapshot.lastSubmitted.value);
				compatibilityCapture->PublishStatus(
					{static_cast<std::uint32_t>(snapshot.lastSubmitted.value), width, height, "RGBA8",
					 mcp::GetSessionStateName(static_cast<int>(snapshot.state)), rt::g_headYaw, rt::g_headPitch,
					 rt::g_headRoll, rt::g_headPos.x, rt::g_headPos.y, rt::g_headPos.z});
			}
			return result;
		}
	}
	return XR_ERROR_SESSION_LOST;
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

static XrResult XRAPI_PTR xrEnumerateReferenceSpaces_runtime(
	XrSession handle, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces)
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
	std::lock_guard lock(rt::g_vulkanMutex);
	if (rt::g_session.vulkan)
	{
		const auto chain = rt::g_session.vulkan->FindSwapchain(sc);
		if (!chain)
			return XR_ERROR_HANDLE_INVALID;
		rt::g_session.vulkan->DestroySwapchain(chain);
		return XR_SUCCESS;
	}
	const auto found = rt::g_swapchains.find(sc);
	if (found == rt::g_swapchains.end())
		return XR_ERROR_HANDLE_INVALID;
	found->second.access->Close();
	if (found->second.glImages && !found->second.glImages->Close())
		return XR_ERROR_RUNTIME_FAILURE;
	rt::g_swapchains.erase(found);
	Logf("[SimXR] xrDestroySwapchain: sc=%p", sc);
	return XR_SUCCESS;
}

// Missing functions Unity needs
static XrResult XRAPI_PTR xrResultToString_runtime(XrInstance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    const char* str = "XR_SUCCESS";
    if (value != XR_SUCCESS) str = "XR_ERROR";
    strncpy(buffer, str, XR_MAX_RESULT_STRING_SIZE - 1);
    buffer[XR_MAX_RESULT_STRING_SIZE - 1] = '\0';
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrStructureTypeToString_runtime(XrInstance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType_%d", (int)value);
    return XR_SUCCESS;
}

static XrResult XRAPI_PTR xrGetReferenceSpaceBoundsRect_runtime(XrSession handle, XrReferenceSpaceType type, XrExtent2Df* bounds) {
    auto core = rt::RetainSession(handle);
    if (!core) return XR_ERROR_HANDLE_INVALID;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    return core->Spaces().Bounds(type, *bounds);
}

static XrResult XRAPI_PTR xrGetViewConfigurationProperties_runtime(
	XrInstance instance, XrSystemId system, XrViewConfigurationType type, XrViewConfigurationProperties* props)
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

static bool ScalePositiveTime(std::int64_t value, std::int64_t numerator, std::int64_t denominator,
							  std::int64_t& output)
{
	if (value <= 0 || numerator <= 0 || denominator <= 0)
		return false;
	const auto whole = static_cast<std::uint64_t>(value / denominator);
	const auto remainder = static_cast<std::uint64_t>(value % denominator);
	const auto scale = static_cast<std::uint64_t>(numerator);
	const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	if (whole > maximum / scale || remainder > std::numeric_limits<std::uint64_t>::max() / scale)
		return false;
	const auto fractional = remainder * scale / static_cast<std::uint64_t>(denominator);
	const auto integral = whole * scale;
	if (fractional > maximum - integral)
		return false;
	output = static_cast<std::int64_t>(integral + fractional);
	return true;
}

static XrResult XRAPI_PTR xrConvertWin32PerformanceCounterToTimeKHR_runtime(
	XrInstance instance, const LARGE_INTEGER* performanceCounter, XrTime* time)
{
	const auto result = RequireExtension(instance, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	if (XR_FAILED(result)) return result;
	if (!performanceCounter || !time) return XR_ERROR_VALIDATION_FAILURE;
	LARGE_INTEGER frequency{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return XR_ERROR_RUNTIME_FAILURE;
	return ScalePositiveTime(performanceCounter->QuadPart, 1000000000LL, frequency.QuadPart, *time)
		? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}

static XrResult XRAPI_PTR xrConvertTimeToWin32PerformanceCounterKHR_runtime(
	XrInstance instance, XrTime time, LARGE_INTEGER* performanceCounter)
{
	const auto result = RequireExtension(instance, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	if (XR_FAILED(result)) return result;
	if (!performanceCounter) return XR_ERROR_VALIDATION_FAILURE;
	LARGE_INTEGER frequency{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return XR_ERROR_RUNTIME_FAILURE;
	std::int64_t counter;
	if (!ScalePositiveTime(time, frequency.QuadPart, 1000000000LL, counter)) return XR_ERROR_TIME_INVALID;
	performanceCounter->QuadPart = counter;
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
