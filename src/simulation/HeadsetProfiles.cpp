#include "simulation/HeadsetProfiles.h"
#include "Pch.h"

namespace simxr
{
ViewSettings ProfileViews(const HeadsetSpec& profile)
{
	ViewSettings settings;
	settings.outerWidth = profile.panelWidth;
	settings.outerHeight = profile.panelHeight;
	settings.ipd = static_cast<float>(profile.ipdMm) / 1000;
	constexpr float radians = std::numbers::pi_v<float> / 180;
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		const auto& fov = profile.eye[eye];
		settings.outerFov[eye] = std::string_view(profile.id) == "generic"
									 ? XrFovf{-45 * radians, 45 * radians, 45 * radians, -45 * radians}
									 : XrFovf{fov.angleLeft * radians, fov.angleRight * radians, fov.angleUp * radians,
											  fov.angleDown * radians};
	}
	return settings;
}
} // namespace simxr
