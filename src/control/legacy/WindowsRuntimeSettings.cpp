#include "control/legacy/WindowsRuntimeSettings.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace simxr::legacy::settings
{
std::recursive_mutex g_stateMutex;
RuntimeSettings g_settings;
namespace
{
constexpr float kMinMoveSpeed = 0.05f;
constexpr float kMaxMoveSpeed = 50.0f;
constexpr const char* InputPollingNames[]{"preview", "app", "background"};
} // namespace
RuntimeSettings ReadState()
{
	std::lock_guard lock(g_stateMutex);
	return g_settings;
}

const HeadsetSpec& GetHeadsetSpec(HeadsetProfile profile)
{
	int i = (int)profile;
	if (i < 0 || i >= kHeadsetProfileCount)
		i = 0;
	return kHeadsetSpecs[i];
}

const HeadsetSpec& GetActiveHeadsetSpec()
{
	std::lock_guard stateLock(g_stateMutex);
	return GetHeadsetSpec(g_settings.headsetProfile);
}

bool UsesProfileFov()
{
	std::lock_guard stateLock(g_stateMutex);
	return g_settings.useAsymmetricFov && g_settings.headsetProfile != HeadsetProfile::GenericSymmetric;
}

float ActiveFovAspect()
{
	std::lock_guard stateLock(g_stateMutex);
	if (!UsesProfileFov())
		return 1.0f; // equal half-angles all round
	const EyeFov& e = GetActiveHeadsetSpec().eye[0];
	const float DEG2RAD = 3.14159265f / 180.0f;
	const float w = tanf(e.angleRight * DEG2RAD) - tanf(e.angleLeft * DEG2RAD);
	const float h = tanf(e.angleUp * DEG2RAD) - tanf(e.angleDown * DEG2RAD);
	return (w > 0.0f && h > 0.0f) ? w / h : 1.0f;
}

int FindHeadsetSpec(const char* s)
{
	for (int i = 0; i < kHeadsetProfileCount; ++i)
	{
		if (strcmp(s, kHeadsetSpecs[i].id) == 0)
			return i;
	}
	return -1;
}

void SetRenderResolution(int width, int height)
{
	std::lock_guard stateLock(g_stateMutex);
	if (width <= 0 || height <= 0)
	{
		g_settings.renderWidth = 0;
		g_settings.renderHeight = 0;
		return;
	}
	g_settings.renderWidth = (std::max)(kMinRenderWidth, (std::min)(kMaxRenderExtent, width));
	g_settings.renderHeight = (std::max)(kMinRenderHeight, (std::min)(kMaxRenderExtent, height));
}

void GetRenderResolution(uint32_t& width, uint32_t& height)
{
	std::lock_guard stateLock(g_stateMutex);
	if (g_settings.renderWidth > 0 && g_settings.renderHeight > 0)
	{
		width = (uint32_t)g_settings.renderWidth;
		height = (uint32_t)g_settings.renderHeight;
	}
	else
	{
		const HeadsetSpec& spec = GetActiveHeadsetSpec();
		width = spec.panelWidth;
		height = spec.panelHeight;
	}

	const float aspect = ActiveFovAspect();
	if (width == 0 || !(aspect > 0.0f))
		return;

	long snapped = lroundf((float)width / aspect);
	if (snapped > kMaxRenderExtent)
	{
		// Losing some width beats recommending a target past our own maxImageRect.
		width = (uint32_t)lroundf((float)kMaxRenderExtent * aspect);
		snapped = kMaxRenderExtent;
	}
	height = (uint32_t)(std::max)(1L, snapped);
}

int GetIpdMillimeters()
{
	std::lock_guard stateLock(g_stateMutex);
	return (int)(g_settings.ipdMeters * 1000.0f + 0.5f);
}

void SetIpdMillimeters(int ipdMm)
{
	std::lock_guard stateLock(g_stateMutex);
	ipdMm = (std::max)(0, (std::min)(200, ipdMm));
	g_settings.ipdMeters = (float)ipdMm * 0.001f;
}

void SetHeadsetProfile(HeadsetProfile profile)
{
	std::lock_guard stateLock(g_stateMutex);
	g_settings.headsetProfile = profile;
	g_settings.useAsymmetricFov = (profile != HeadsetProfile::GenericSymmetric);
	SetIpdMillimeters(GetHeadsetSpec(profile).ipdMm);
}

const wchar_t* GetHeadsetProfileShortName()
{
	return GetActiveHeadsetSpec().shortName;
}

void SetMoveSpeed(float mps)
{
	std::lock_guard stateLock(g_stateMutex);
	g_settings.moveSpeed = (std::max)(kMinMoveSpeed, (std::min)(kMaxMoveSpeed, mps));
}

float GetMoveSpeed(bool boosted)
{
	std::lock_guard stateLock(g_stateMutex);
	return boosted ? g_settings.moveSpeed * g_settings.moveBoost : g_settings.moveSpeed;
}

HeadsetProfile HeadsetProfileFromName(const char* s, HeadsetProfile def)
{
	int i = FindHeadsetSpec(s);
	return i < 0 ? def : (HeadsetProfile)i;
}

const char* InputPollingName(InputPolling p)
{
	int i = (int)p;
	if (i < 0 || i >= kInputPollingPresetCount)
		i = (int)InputPolling::AppForeground;
	return InputPollingNames[i];
}

InputPolling InputPollingFromName(const char* s, InputPolling def)
{
	for (int i = 0; i < kInputPollingPresetCount; ++i)
	{
		if (strcmp(s, InputPollingNames[i]) == 0)
			return (InputPolling)i;
	}
	return def;
}

void ApplySettingsJson(const char* text)
{
	std::lock_guard stateLock(g_stateMutex);
	json::Object o(text);

	// Profile first: it resets FOV symmetry and IPD, so the saved values for
	// those have to land after it.
	std::string profile = o.string("headset_profile");
	if (!profile.empty())
	{
		SetHeadsetProfile(HeadsetProfileFromName(profile.c_str(), g_settings.headsetProfile));
	}

	// A missing pair is an older settings file: keep the new performance default. 0x0 is
	// the explicit "Headset Native" mode; positive custom values are accepted even when
	// they are not one of the menu presets.
	const bool hasRenderResolution = o.has("render_width") || o.has("render_height");
	if (hasRenderResolution)
	{
		const int width = o.number("render_width", g_settings.renderWidth);
		const int height = o.number("render_height", g_settings.renderHeight);
		SetRenderResolution(width, height);
	}

	g_settings.useAsymmetricFov = o.boolean("asymmetric_fov", g_settings.useAsymmetricFov);
	g_settings.fovDegrees = (std::max)(30, (std::min)(170, o.number("fov_degrees", g_settings.fovDegrees)));
	SetIpdMillimeters(o.number("ipd_mm", GetIpdMillimeters()));

	SetMoveSpeed(o.number("move_speed", g_settings.moveSpeed));
	g_settings.moveBoost = (std::max)(1.0f, (std::min)(50.0f, o.number("move_boost", g_settings.moveBoost)));

	g_settings.inputPolling = InputPollingFromName(o.string("input_polling").c_str(), g_settings.inputPolling);
}

void LoadSettings(const std::filesystem::path& dataDir)
{
	std::error_code error;
	std::filesystem::create_directories(dataDir, error);
	const auto path = dataDir / L"settings.json";

	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f)
	{
		char buf[2048];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = '\0';
		fclose(f);
		ApplySettingsJson(buf);
	}
}

} // namespace simxr::legacy::settings
