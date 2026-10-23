#include "diagnostics/platform/RenderDocCapture.h"
#include "Pch.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <bit>
#include <cstdlib>

namespace flicker
{
#ifdef _WIN32
#define SIMXR_CAPTURE_CALL __cdecl
#else
#define SIMXR_CAPTURE_CALL
#endif
using RenderDocSetCapturePath = void(SIMXR_CAPTURE_CALL*)(const char*);
using RenderDocTriggerCapture = void(SIMXR_CAPTURE_CALL*)(uint32_t);
// RenderDoc's append-only 1.1.0 layout through TriggerMultiFrameCapture.
struct RenderDocApi110
{
	void* getApiVersion;
	void* setCaptureOptionU32;
	void* setCaptureOptionF32;
	void* getCaptureOptionU32;
	void* getCaptureOptionF32;
	void* setFocusToggleKeys;
	void* setCaptureKeys;
	void* getOverlayBits;
	void* maskOverlayBits;
	void* removeHooks;
	void* unloadCrashHandler;
	RenderDocSetCapturePath setCaptureFilePathTemplate;
	void* getCaptureFilePathTemplate;
	void* getNumCaptures;
	void* getCapture;
	void* triggerCapture;
	void* isTargetControlConnected;
	void* launchReplayUi;
	void* setActiveWindow;
	void* startFrameCapture;
	void* isFrameCapturing;
	void* endFrameCapture;
	RenderDocTriggerCapture triggerMultiFrameCapture;
};
using RenderDocGetApi = int(SIMXR_CAPTURE_CALL*)(int, void**);
#undef SIMXR_CAPTURE_CALL

class CaptureModule
{
  public:
	CaptureModule()
	{
#ifdef _WIN32
		GetModuleHandleExW(0, L"renderdoc.dll", &m_module);
#else
		// Resolve an already injected API, then retain its actual module while calling it.
		Dl_info information{};
		if (const auto* symbol = dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI"); symbol && dladdr(symbol, &information))
			m_module = dlopen(information.dli_fname, RTLD_NOW | RTLD_NOLOAD);
#endif
	}
	CaptureModule(const CaptureModule&) = delete;
	CaptureModule& operator=(const CaptureModule&) = delete;
	~CaptureModule()
	{
		if (m_module)
		{
#ifdef _WIN32
			FreeLibrary(m_module);
#else
			dlclose(m_module);
#endif
		}
	}
	RenderDocGetApi Entry() const
	{
		if (!m_module)
			return nullptr;
#ifdef _WIN32
		return std::bit_cast<RenderDocGetApi>(GetProcAddress(m_module, "RENDERDOC_GetAPI"));
#else
		return reinterpret_cast<RenderDocGetApi>(dlsym(m_module, "RENDERDOC_GetAPI"));
#endif
	}

  private:
#ifdef _WIN32
	HMODULE m_module = nullptr;
#else
	void* m_module = nullptr;
#endif
};

RenderDocCapture::RenderDocCapture()
{
	if (const auto* enabled = std::getenv("BVR_RENDERDOC_AUTO_CAPTURE"))
		m_enabled = enabled[0] != '\0' && enabled[0] != '0';
	if (const auto* path = std::getenv("BVR_RENDERDOC_CAPTURE_TEMPLATE"))
		m_captureTemplate = path;
	if (const auto* frames = std::getenv("BVR_RENDERDOC_CAPTURE_FRAMES"))
		m_frameCount = static_cast<uint32_t>(std::clamp(std::strtoul(frames, nullptr, 10), 1ul, 10ul));
}

RenderDocResult RenderDocCapture::Trigger(uint64_t frame)
{
	if (!m_enabled || (m_lastTriggerFrame && frame >= *m_lastTriggerFrame && frame - *m_lastTriggerFrame < 60))
		return {};
	CaptureModule module;
	const auto getApi = module.Entry();
	if (!getApi)
		return {true, false};
	RenderDocApi110* api = nullptr;
	if (getApi(10100, reinterpret_cast<void**>(&api)) != 1 || !api || !api->triggerMultiFrameCapture)
		return {true, false};
	if (!m_captureTemplate.empty() && api->setCaptureFilePathTemplate)
		api->setCaptureFilePathTemplate(m_captureTemplate.c_str());
	api->triggerMultiFrameCapture(m_frameCount);
	m_lastTriggerFrame = frame;
	return {true, true};
}
} // namespace flicker
