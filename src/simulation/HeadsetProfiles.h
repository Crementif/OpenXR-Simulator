#pragma once

#include "simulation/ViewConfiguration.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace simxr
{
// Preserved headset optics from the existing simulator table. Geometry angles
// are in degrees for the legacy adapter; ProfileViews converts to XR radians.
struct HeadsetEyeFov
{
	float angleLeft, angleRight, angleUp, angleDown;
};

struct HeadsetSpec
{
	const char* id;			  // settings key and MCP set_headset_profile name
	const wchar_t* shortName; // title bar
	const wchar_t* menuLabel;
	std::uint32_t panelWidth;
	std::uint32_t panelHeight;
	int ipdMm;			  // nominal default, not the database's per-session value
	HeadsetEyeFov eye[2]; // [0] = left, [1] = right
};

// GenericSymmetric takes its FOV from g_uiState.fovDegrees, so its angles are unused.
inline constexpr HeadsetSpec HeadsetProfileTable[] = {
	{"generic",
	 L"Generic",
	 L"&Generic Symmetric",
	 1440,
	 1440,
	 64,
	 {{0.00f, 0.00f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.00f, 0.00f}}},
	{"quest2",
	 L"Quest 2",
	 L"Meta Quest &2",
	 1832,
	 1920,
	 64,
	 {{-52.00f, 45.00f, 48.00f, -50.00f}, {-45.00f, 52.00f, 48.00f, -50.00f}}},
	{"quest3",
	 L"Quest 3",
	 L"Meta Quest &3",
	 2064,
	 2208,
	 64,
	 {{-54.00f, 40.00f, 43.98f, -54.27f}, {-40.00f, 54.00f, 43.98f, -54.27f}}},
	{"questpro",
	 L"Quest Pro",
	 L"Meta Quest &Pro",
	 1800,
	 1920,
	 64,
	 {{-54.00f, 39.86f, 42.00f, -53.57f}, {-39.86f, 54.00f, 42.00f, -53.57f}}},
	{"index",
	 L"Index",
	 L"Valve &Index",
	 1440,
	 1600,
	 63,
	 {{-54.00f, 42.98f, 54.63f, -54.52f}, {-42.95f, 54.06f, 54.66f, -54.50f}}},
	{"vivepro2",
	 L"Vive Pro 2",
	 L"HTC &Vive Pro 2",
	 2448,
	 2448,
	 63,
	 {{-58.26f, 39.94f, 48.21f, -48.11f}, {-39.89f, 58.26f, 48.44f, -48.20f}}},
	{"reverbg2",
	 L"Reverb G2",
	 L"HP &Reverb G2",
	 2160,
	 2160,
	 64,
	 {{-49.37f, 42.14f, 45.53f, -45.35f}, {-42.17f, 49.48f, 45.78f, -45.05f}}},
	{"psvr2",
	 L"PS VR2",
	 L"&Sony PS VR2",
	 2000,
	 2040,
	 64,
	 {{-61.50f, 43.45f, 53.04f, -53.04f}, {-43.45f, 61.50f, 53.04f, -53.04f}}},
	{"pico4",
	 L"PICO 4",
	 L"PIC&O 4",
	 2160,
	 2160,
	 64,
	 {{-52.00f, 52.00f, 52.00f, -52.00f}, {-52.00f, 52.00f, 52.00f, -52.00f}}},
	{"beyond",
	 L"Beyond",
	 L"&Bigscreen Beyond",
	 2560,
	 2560,
	 64,
	 {{-48.97f, 39.58f, 38.01f, -50.52f}, {-40.02f, 48.56f, 38.13f, -50.43f}}},
};

inline constexpr std::span<const HeadsetSpec> HeadsetProfiles()
{
	return HeadsetProfileTable;
}

inline constexpr const HeadsetSpec* FindHeadsetProfile(std::string_view id)
{
	for (const auto& profile : HeadsetProfileTable)
		if (id == profile.id)
			return &profile;
	return nullptr;
}

ViewSettings ProfileViews(const HeadsetSpec& profile);
} // namespace simxr
