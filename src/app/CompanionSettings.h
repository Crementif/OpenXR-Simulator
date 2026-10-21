#pragma once
#include "app/PilotBindings.h"
#include "graphics/SnapshotBudget.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace simxr::app
{
struct LaunchProfile
{
	std::string name;
	std::string executable;
	std::vector<std::string> arguments;
	std::string workingDirectory;
	std::string runtimeManifest;
	std::vector<std::pair<std::string, std::string>> environment;
	std::string headsetProfile;
	std::string rig;
	bool operator==(const LaunchProfile&) const = default;
};
struct SavedLayout
{
	std::string name;
	std::string ini;
	bool operator==(const SavedLayout&) const = default;
};
struct CompanionSettings
{
	float scale = 1;
	std::uint32_t snapshotBudgetMiB = SnapshotBudget::DefaultMiB;
	PilotBindings pilotBindings = DefaultPilotBindings();
	GamepadBindings gamepadBindings;
	float inspectorWidth = 340, timelineHeight = 132;
	bool railCollapsed = false, timelineResized = false;
	std::string endpoint;
	std::string activeProfile;
	std::string activeLayout;
	std::vector<LaunchProfile> profiles;
	std::vector<SavedLayout> layouts;
	bool operator==(const CompanionSettings&) const = default;
};
// Explicit override must be absolute. With no HOME/config base, report unavailable.
std::filesystem::path CompanionSettingsPath();
void ValidateSettings(const CompanionSettings& settings);
CompanionSettings LoadSettings(const std::filesystem::path& path);
void SaveSettings(const std::filesystem::path& path, const CompanionSettings& settings);
} // namespace simxr::app
