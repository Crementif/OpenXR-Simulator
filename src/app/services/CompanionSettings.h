#pragma once
#include "app/services/LaunchTypes.h"
#include "app/services/PilotBindings.h"
#include "graphics/SnapshotBudget.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simxr::app
{
// An app launched or connected from this companion. The profile's name is a
// display name; it need not be unique.
struct RecentApp
{
	LaunchProfile launch;
	// Seconds since the Unix epoch.
	std::int64_t lastUsed = 0;
	bool operator==(const RecentApp&) const = default;
};
inline constexpr std::size_t MaxRecentApps = 8;
struct SavedLayout
{
	std::string name;
	std::string ini;
	bool operator==(const SavedLayout&) const = default;
};
// A preview region pinned for one launch profile, in normalized image coordinates.
struct PreviewCrop
{
	std::string profile;
	float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
	bool operator==(const PreviewCrop&) const = default;
};
struct CompanionSettings
{
	float scale = 1;
	std::uint32_t snapshotBudgetMiB = SnapshotBudget::DefaultMiB;
	PilotBindings pilotBindings = DefaultPilotBindings();
	bool remoteMouseDrag = false;
	GamepadBindings gamepadBindings;
	float inspectorWidth = 340, timelineHeight = 132;
	// railCollapsed is no longer used; it stays so older builds can read the file.
	bool railCollapsed = true, timelineResized = false;
	bool inspectorFolded = false, timelineFolded = false, hudFolded = false;
	std::vector<PreviewCrop> crops;
	std::string endpoint;
	std::string activeProfile;
	std::string activeLayout;
	std::vector<LaunchProfile> profiles;
	std::vector<SavedLayout> layouts;
	std::vector<RecentApp> recent;
	bool operator==(const CompanionSettings&) const = default;
};
// Explicit override must be absolute. With no HOME/config base, report unavailable.
std::filesystem::path CompanionSettingsPath();
void ValidateSettings(const CompanionSettings& settings);
// Inspector width, timeline height and folds back to their defaults.
void ResetLayout(CompanionSettings& settings);
// Puts the app first, replacing an older entry with the same executable,
// arguments and working directory, and keeps at most MaxRecentApps.
void RememberRecentApp(CompanionSettings& settings, LaunchProfile launch, std::int64_t lastUsed);
CompanionSettings LoadSettings(const std::filesystem::path& path);
void SaveSettings(const std::filesystem::path& path, const CompanionSettings& settings);
// Window layout text is bounded to 8 MiB and cannot contain NUL. A missing
// file loads as empty; other I/O failures throw. Saves require an absolute path.
std::string LoadWindowLayout(const std::filesystem::path& path);
void SaveWindowLayout(const std::filesystem::path& path, std::string_view text);
enum class PathKind
{
	Executable,
	Folder,
	File
};
// Empty when the path can be used as that kind, otherwise a short reason.
std::string PathProblem(const std::string& path, PathKind kind);
} // namespace simxr::app
