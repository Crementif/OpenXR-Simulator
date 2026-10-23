#include "Pch.h"
#include "app/services/LaunchTypes.h"
#include "simulation/HeadsetProfiles.h"

#include <set>
#include <stdexcept>

namespace simxr::app
{
void ValidateLaunchProfile(const LaunchProfile& profile)
{
	const auto validateText = [](const std::string& text)
	{
		if (text.size() > 1024 * 1024 || text.find('\0') != std::string::npos)
			throw std::invalid_argument("Launch text exceeds its bound or contains NUL");
	};
	validateText(profile.name);
	validateText(profile.executable);
	validateText(profile.workingDirectory);
	validateText(profile.runtimeManifest);
	validateText(profile.headsetProfile);
	validateText(profile.rig);
	if (!profile.headsetProfile.empty() && !FindHeadsetProfile(profile.headsetProfile))
		throw std::invalid_argument("Unknown headset profile");
	if (profile.arguments.size() > 256 || profile.environment.size() > MaxLaunchEnvironment)
		throw std::invalid_argument("Launch profile exceeds its bounds");
	for (const auto& argument : profile.arguments)
		validateText(argument);
	std::set<std::string> keys;
	for (const auto& [key, value] : profile.environment)
	{
		validateText(key);
		validateText(value);
		if (key.empty() || key.find('=') != std::string::npos || !keys.insert(key).second)
			throw std::invalid_argument("Invalid or duplicate environment key");
	}
}
}
