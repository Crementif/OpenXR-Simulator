#pragma once
#include "simulation/HeadsetProfiles.h"
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace simxr::legacy::settings
{
enum class HeadsetProfile
{
	GenericSymmetric,
	Quest2,
	Quest3,
	QuestPro,
	ValveIndex,
	VivePro2,
	ReverbG2,
	PSVR2,
	Pico4,
	BigscreenBeyond
};

// Keep the legacy persisted input-mode ordering.
enum class InputPolling
{
	PreviewFocused,
	AppForeground,
	Background
};

// Startup compatibility defaults from the former runtime settings.json.
struct RuntimeSettings
{
	HeadsetProfile headsetProfile = HeadsetProfile::Quest3;
	int renderWidth = 1280;
	int renderHeight = 1400;
	int fovDegrees = 90;
	bool useAsymmetricFov = true;
	float ipdMeters = 0.064f;
	float moveSpeed = 3.0f;
	float moveBoost = 4.0f;
	InputPolling inputPolling = InputPolling::AppForeground;
};
extern std::recursive_mutex g_stateMutex;
extern RuntimeSettings g_settings;
using EyeFov = simxr::HeadsetEyeFov;
using HeadsetSpec = simxr::HeadsetSpec;
inline constexpr auto& kHeadsetSpecs = simxr::HeadsetProfileTable;
inline constexpr int kHeadsetProfileCount = sizeof(kHeadsetSpecs) / sizeof(kHeadsetSpecs[0]);
inline constexpr int kMinRenderWidth = 320;
inline constexpr int kMinRenderHeight = 240;
inline constexpr int kMaxRenderExtent = 4096;
inline constexpr int kInputPollingPresetCount = 3;
RuntimeSettings ReadState();
const HeadsetSpec& GetHeadsetSpec(HeadsetProfile profile);
const HeadsetSpec& GetActiveHeadsetSpec();
bool UsesProfileFov();
float ActiveFovAspect();
int FindHeadsetSpec(const char* s);
void SetRenderResolution(int width, int height);
void GetRenderResolution(uint32_t& width, uint32_t& height);
int GetIpdMillimeters();
void SetIpdMillimeters(int ipdMm);
void SetHeadsetProfile(HeadsetProfile profile);
const wchar_t* GetHeadsetProfileShortName();
void SetMoveSpeed(float mps);
float GetMoveSpeed(bool boosted);
HeadsetProfile HeadsetProfileFromName(const char* s, HeadsetProfile def);
const char* InputPollingName(InputPolling p);
InputPolling InputPollingFromName(const char* s, InputPolling def);
void ApplySettingsJson(const char* text);
void LoadSettings(const std::filesystem::path& dataDir);
} // namespace simxr::legacy::settings
