#include "control/legacy/WindowsRuntimeSettings.h"
#include "Pch.h"
#include "json.h"

namespace simxr::legacy::settings
{
static std::recursive_mutex s_stateMutex;
static RuntimeSettings s_settings;
static constexpr float s_minMoveSpeed = 0.05f;
static constexpr float s_maxMoveSpeed = 50.0f;
static constexpr const char* s_inputPollingNames[]{"preview", "app", "background"};
static constexpr std::uintmax_t s_maxDocumentBytes = 1024 * 1024;

RuntimeSettings ReadState()
{
	std::lock_guard lock(s_stateMutex);
	return s_settings;
}

const HeadsetSpec& GetHeadsetSpec(HeadsetProfile profile)
{
	auto index = static_cast<int>(profile);
	if (index < 0 || index >= kHeadsetProfileCount)
		index = 0;
	return kHeadsetSpecs[index];
}

const HeadsetSpec& GetActiveHeadsetSpec()
{
	std::lock_guard stateLock(s_stateMutex);
	return GetHeadsetSpec(s_settings.headsetProfile);
}

bool UsesProfileFov()
{
	std::lock_guard stateLock(s_stateMutex);
	return s_settings.useAsymmetricFov && s_settings.headsetProfile != HeadsetProfile::GenericSymmetric;
}

float ActiveFovAspect()
{
	std::lock_guard stateLock(s_stateMutex);
	if (!UsesProfileFov())
		return 1.0f; // equal half-angles all round
	const auto& eye = GetActiveHeadsetSpec().eye[0];
	constexpr float degreesToRadians = 3.14159265f / 180.0f;
	const float width = std::tan(eye.angleRight * degreesToRadians) - std::tan(eye.angleLeft * degreesToRadians);
	const float height = std::tan(eye.angleUp * degreesToRadians) - std::tan(eye.angleDown * degreesToRadians);
	return (width > 0.0f && height > 0.0f) ? width / height : 1.0f;
}

int FindHeadsetSpec(const char* name)
{
	if (!name)
		return -1;
	for (int index = 0; index < kHeadsetProfileCount; ++index)
	{
		if (std::strcmp(name, kHeadsetSpecs[index].id) == 0)
			return index;
	}
	return -1;
}

void SetRenderResolution(int width, int height)
{
	std::lock_guard stateLock(s_stateMutex);
	if (width <= 0 || height <= 0)
	{
		s_settings.renderWidth = 0;
		s_settings.renderHeight = 0;
		return;
	}
	s_settings.renderWidth = std::clamp(width, kMinRenderWidth, kMaxRenderExtent);
	s_settings.renderHeight = std::clamp(height, kMinRenderHeight, kMaxRenderExtent);
}

void GetRenderResolution(uint32_t& width, uint32_t& height)
{
	std::lock_guard stateLock(s_stateMutex);
	if (s_settings.renderWidth > 0 && s_settings.renderHeight > 0)
	{
		width = static_cast<uint32_t>(s_settings.renderWidth);
		height = static_cast<uint32_t>(s_settings.renderHeight);
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

	auto snapped = std::lround(static_cast<float>(width) / aspect);
	if (snapped > kMaxRenderExtent)
	{
		// Losing some width beats recommending a target past our own maxImageRect.
		width = static_cast<uint32_t>(std::lround(static_cast<float>(kMaxRenderExtent) * aspect));
		snapped = kMaxRenderExtent;
	}
	height = static_cast<uint32_t>((std::max)(1L, snapped));
}

int GetIpdMillimeters()
{
	std::lock_guard stateLock(s_stateMutex);
	return static_cast<int>(s_settings.ipdMeters * 1000.0f + 0.5f);
}

void SetIpdMillimeters(int ipdMm)
{
	std::lock_guard stateLock(s_stateMutex);
	ipdMm = std::clamp(ipdMm, 0, 200);
	s_settings.ipdMeters = static_cast<float>(ipdMm) * 0.001f;
}

void SetHeadsetProfile(HeadsetProfile profile)
{
	std::lock_guard stateLock(s_stateMutex);
	s_settings.headsetProfile = profile;
	s_settings.useAsymmetricFov = (profile != HeadsetProfile::GenericSymmetric);
	SetIpdMillimeters(GetHeadsetSpec(profile).ipdMm);
}

const wchar_t* GetHeadsetProfileShortName()
{
	return GetActiveHeadsetSpec().shortName;
}

void SetMoveSpeed(float metersPerSecond)
{
	std::lock_guard stateLock(s_stateMutex);
	s_settings.moveSpeed = (std::max)(s_minMoveSpeed, (std::min)(s_maxMoveSpeed, metersPerSecond));
}

float GetMoveSpeed(bool boosted)
{
	std::lock_guard stateLock(s_stateMutex);
	return boosted ? s_settings.moveSpeed * s_settings.moveBoost : s_settings.moveSpeed;
}

HeadsetProfile HeadsetProfileFromName(const char* name, HeadsetProfile fallback)
{
	const auto index = FindHeadsetSpec(name);
	return index < 0 ? fallback : static_cast<HeadsetProfile>(index);
}

const char* InputPollingName(InputPolling polling)
{
	auto index = static_cast<int>(polling);
	if (index < 0 || index >= kInputPollingPresetCount)
		index = static_cast<int>(InputPolling::AppForeground);
	return s_inputPollingNames[index];
}

InputPolling InputPollingFromName(const char* name, InputPolling fallback)
{
	if (!name)
		return fallback;
	for (int index = 0; index < kInputPollingPresetCount; ++index)
	{
		if (std::strcmp(name, s_inputPollingNames[index]) == 0)
			return static_cast<InputPolling>(index);
	}
	return fallback;
}

void ApplySettingsJson(const char* text)
{
	std::lock_guard stateLock(s_stateMutex);
	const json::Object object(text);

	// Profile first: it resets FOV symmetry and IPD, so the saved values for
	// those have to land after it.
	const auto profile = object.String("headset_profile");
	if (!profile.empty())
	{
		SetHeadsetProfile(HeadsetProfileFromName(profile.c_str(), s_settings.headsetProfile));
	}

	// A missing pair is an older settings file: keep the new performance default. 0x0 is
	// the explicit "Headset Native" mode; positive custom values are accepted even when
	// they are not one of the menu presets.
	const bool hasRenderResolution = object.Has("render_width") || object.Has("render_height");
	if (hasRenderResolution)
	{
		const int width = object.Number("render_width", s_settings.renderWidth);
		const int height = object.Number("render_height", s_settings.renderHeight);
		SetRenderResolution(width, height);
	}

	s_settings.useAsymmetricFov = object.Boolean("asymmetric_fov", s_settings.useAsymmetricFov);
	s_settings.fovDegrees = std::clamp(object.Number("fov_degrees", s_settings.fovDegrees), 30, 170);
	SetIpdMillimeters(object.Number("ipd_mm", GetIpdMillimeters()));

	SetMoveSpeed(object.Number("move_speed", s_settings.moveSpeed));
	s_settings.moveBoost = (std::max)(1.0f, (std::min)(50.0f, object.Number("move_boost", s_settings.moveBoost)));

	s_settings.inputPolling = InputPollingFromName(object.String("input_polling").c_str(), s_settings.inputPolling);
}

void LoadSettings(const std::filesystem::path& dataDir)
{
	std::filesystem::create_directories(dataDir);
	const auto path = dataDir / "settings.json";
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error)
		throw std::system_error(error, "Inspect compatibility settings");
	if (!exists)
		return;

	const auto size = std::filesystem::file_size(path);
	if (size > s_maxDocumentBytes)
		throw std::runtime_error("Compatibility settings exceed the size limit");

	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open compatibility settings");
	std::string text(static_cast<std::size_t>(size), '\0');
	input.read(text.data(), static_cast<std::streamsize>(text.size()));
	if (!input || input.peek() != std::char_traits<char>::eof() || input.bad())
		throw std::runtime_error("Cannot read complete compatibility settings");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close compatibility settings");
	ApplySettingsJson(text.c_str());
}

} // namespace simxr::legacy::settings
