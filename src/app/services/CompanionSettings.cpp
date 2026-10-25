#include "app/services/CompanionSettings.h"
#include "Pch.h"
#include "app/platform/FileStorage.h"
#include "app/services/Utf8Path.h"

#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace simxr::app
{
static constexpr std::string_view s_magic = "SIMXR-COMPANION\x01";
static constexpr std::size_t s_maxFile = 8 * 1024 * 1024;
static constexpr std::size_t s_maxText = 1024 * 1024;
static void TextValid(const std::string& text)
{
	if (text.size() > s_maxText || text.find('\0') != std::string::npos)
		throw std::invalid_argument("Settings text exceeds its bound or contains NUL");
}
static void Put(std::string& bytes, std::uint32_t number)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		bytes.push_back(static_cast<char>(number >> shift));
}
static void Put(std::string& bytes, const std::string& text)
{
	TextValid(text);
	if (bytes.size() + 4 + text.size() > s_maxFile)
		throw std::invalid_argument("Companion settings exceed the file budget");
	Put(bytes, static_cast<std::uint32_t>(text.size()));
	bytes += text;
}
class SettingsReader
{
  public:
	explicit SettingsReader(std::string_view bytes) : m_bytes(bytes) {}
	bool Empty() const noexcept
	{
		return m_bytes.empty();
	}
	std::uint32_t Number()
	{
		if (m_bytes.size() < 4)
			throw std::runtime_error("Truncated companion settings");
		std::uint32_t value = 0;
		for (unsigned index = 0; index < 4; ++index)
			value |= static_cast<std::uint32_t>(static_cast<unsigned char>(m_bytes[index])) << (index * 8);
		m_bytes.remove_prefix(4);
		return value;
	}
	std::string Text()
	{
		const auto size = Number();
		if (size > s_maxText || m_bytes.size() < size)
			throw std::runtime_error("Invalid companion settings text length");
		std::string result(m_bytes.substr(0, size));
		m_bytes.remove_prefix(size);
		TextValid(result);
		return result;
	}
	std::uint32_t Count(std::uint32_t limit)
	{
		const auto count = Number();
		if (count > limit)
			throw std::runtime_error("Companion settings table exceeds its bound");
		return count;
	}

  private:
	std::string_view m_bytes;
};
static void PutProfile(std::string& bytes, const LaunchProfile& profile)
{
	Put(bytes, profile.name);
	Put(bytes, profile.executable);
	Put(bytes, profile.workingDirectory);
	Put(bytes, profile.runtimeManifest);
	Put(bytes, profile.headsetProfile);
	Put(bytes, profile.rig);
	Put(bytes, static_cast<std::uint32_t>(profile.arguments.size()));
	for (const auto& argument : profile.arguments)
		Put(bytes, argument);
	Put(bytes, static_cast<std::uint32_t>(profile.environment.size()));
	for (const auto& [key, value] : profile.environment)
	{
		Put(bytes, key);
		Put(bytes, value);
	}
}
static LaunchProfile ReadProfile(SettingsReader& reader)
{
	LaunchProfile profile;
	profile.name = reader.Text();
	profile.executable = reader.Text();
	profile.workingDirectory = reader.Text();
	profile.runtimeManifest = reader.Text();
	profile.headsetProfile = reader.Text();
	profile.rig = reader.Text();
	for (auto arguments = reader.Count(256); arguments; --arguments)
		profile.arguments.push_back(reader.Text());
	for (auto values = reader.Count(MaxLaunchEnvironment); values; --values)
	{
		auto key = reader.Text();
		auto value = reader.Text();
		profile.environment.emplace_back(std::move(key), std::move(value));
	}
	return profile;
}
void ResetLayout(CompanionSettings& settings)
{
	const CompanionSettings defaults;
	settings.inspectorWidth = defaults.inspectorWidth;
	settings.timelineHeight = defaults.timelineHeight;
	settings.timelineResized = false;
	settings.inspectorFolded = false;
	settings.timelineFolded = false;
}
void ValidateSettings(const CompanionSettings& settings)
{
	if (settings.previewRamMiB < FramePreviewHistory::MinMiB || settings.previewRamMiB > FramePreviewHistory::MaxMiB ||
		settings.previewRamMiB % 4)
		throw std::invalid_argument("Invalid compressed history budget");
	if (settings.previewBudgetMiB < FramePreviewHistory::MinMiB ||
		settings.previewBudgetMiB > FramePreviewHistory::MaxMiB || settings.previewBudgetMiB % 4)
		throw std::invalid_argument("Invalid preview history budget");
	if (settings.snapshotBudgetMiB < SnapshotBudget::MinMiB || settings.snapshotBudgetMiB > SnapshotBudget::MaxMiB)
		throw std::invalid_argument("Invalid snapshot budget");
	if (!std::isfinite(settings.scale) || settings.scale < 0.75f || settings.scale > 2.5f ||
		!std::isfinite(settings.inspectorWidth) || settings.inspectorWidth < 280 || settings.inspectorWidth > 520 ||
		!std::isfinite(settings.timelineHeight) || settings.timelineHeight < 56 || settings.timelineHeight > 4096 ||
		settings.profiles.size() > 32 || settings.layouts.size() > 16)
		throw std::invalid_argument("Invalid scale or companion settings capacity");
	for (const auto& binding : settings.pilotBindings)
	{
		TextValid(binding.key);
		if (binding.key.size() > 64 || binding.modifiers > 15)
			throw std::invalid_argument("Invalid pilot binding");
	}
	if (!std::isfinite(settings.gamepadBindings.deadzone) || settings.gamepadBindings.deadzone < 0 ||
		settings.gamepadBindings.deadzone >= 1)
		throw std::invalid_argument("Invalid gamepad deadzone");
	TextValid(settings.gamepadBindings.gyroHold);
	if (settings.crops.size() > 32)
		throw std::invalid_argument("Too many pinned preview crops");
	for (const auto& crop : settings.crops)
	{
		TextValid(crop.profile);
		if (!(crop.x0 >= 0 && crop.x0 < crop.x1 && crop.x1 <= 1 && crop.y0 >= 0 && crop.y0 < crop.y1 && crop.y1 <= 1))
			throw std::invalid_argument("Invalid pinned preview crop");
	}
	for (const auto* bindings :
		 {&settings.gamepadBindings.stickX, &settings.gamepadBindings.stickY, &settings.gamepadBindings.trigger,
		  &settings.gamepadBindings.grip, &settings.gamepadBindings.primary, &settings.gamepadBindings.secondary,
		  &settings.gamepadBindings.menu, &settings.gamepadBindings.stickClick})
		for (const auto& binding : *bindings)
			TextValid(binding);
	TextValid(settings.endpoint);
	TextValid(settings.activeProfile);
	TextValid(settings.activeLayout);
	std::set<std::string> names;
	for (const auto& profile : settings.profiles)
	{
		ValidateLaunchProfile(profile);
		if (profile.name.empty() || !names.insert(profile.name).second)
			throw std::invalid_argument("Invalid or duplicate launch profile");
	}
	if (settings.recent.size() > MaxRecentApps)
		throw std::invalid_argument("Too many recent apps");
	for (const auto& recent : settings.recent)
		ValidateLaunchProfile(recent.launch);
	names.clear();
	for (const auto& layout : settings.layouts)
	{
		TextValid(layout.name);
		TextValid(layout.ini);
		if (layout.name.empty() || !names.insert(layout.name).second)
			throw std::invalid_argument("Invalid or duplicate layout name");
	}
}
std::string PathProblem(const std::string& path, PathKind kind)
{
	if (path.empty())
		return "Required";
	std::error_code error;
	const auto target = Utf8Path(path);
	const auto status = std::filesystem::status(target, error);
	if (error || !std::filesystem::exists(status))
		return kind == PathKind::Folder ? "Folder not found" : "File not found";
	if (kind == PathKind::Folder)
		return std::filesystem::is_directory(status) ? std::string{} : "Not a folder";
	if (std::filesystem::is_directory(status))
		return "This is a folder";
	if (kind == PathKind::Executable && !CanExecuteFile(target))
		return "Not executable";
	return {};
}
void RememberRecentApp(CompanionSettings& settings, LaunchProfile launch, std::int64_t lastUsed)
{
	ValidateLaunchProfile(launch);
	std::erase_if(settings.recent,
				  [&](const RecentApp& recent)
				  {
					  return recent.launch.executable == launch.executable &&
							 recent.launch.arguments == launch.arguments &&
							 recent.launch.workingDirectory == launch.workingDirectory;
				  });
	settings.recent.insert(settings.recent.begin(), RecentApp{std::move(launch), lastUsed});
	if (settings.recent.size() > MaxRecentApps)
		settings.recent.resize(MaxRecentApps);
}
CompanionSettings LoadSettings(const std::filesystem::path& path)
{
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error)
		throw std::runtime_error("Cannot inspect companion settings: " + error.message());
	if (!exists)
		return {};
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > s_maxFile)
		throw std::runtime_error("Cannot read companion settings size");
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open companion settings");
	std::string bytes(static_cast<std::size_t>(size), '\0');
	if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
		throw std::runtime_error("Cannot read companion settings");
	if (input.peek() != std::char_traits<char>::eof() || input.bad())
		throw std::runtime_error("Cannot read complete companion settings");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close companion settings");
	if (!std::string_view(bytes).starts_with(s_magic))
		throw std::runtime_error("Unsupported companion settings version");
	SettingsReader reader{std::string_view(bytes).substr(s_magic.size())};
	CompanionSettings result;
	result.scale = static_cast<float>(reader.Number()) / 1000;
	result.endpoint = reader.Text();
	result.activeProfile = reader.Text();
	result.activeLayout = reader.Text();
	for (auto count = reader.Count(32); count; --count)
		result.profiles.push_back(ReadProfile(reader));
	for (auto count = reader.Count(16); count; --count)
		result.layouts.push_back({reader.Text(), reader.Text()});
	if (!reader.Empty())
	{
		if (reader.Number() != 2)
			throw std::runtime_error("Unknown shell settings version");
		result.inspectorWidth = reader.Number() / 1000.0f;
		result.timelineHeight = reader.Number() / 1000.0f;
		result.railCollapsed = reader.Count(1) != 0;
		result.timelineResized = reader.Count(1) != 0;
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 3)
			throw std::runtime_error("Unknown input settings version");
		for (auto& binding : result.pilotBindings)
		{
			binding.key = reader.Text();
			binding.modifiers = reader.Count(15);
		}
		for (auto* bindings :
			 {&result.gamepadBindings.stickX, &result.gamepadBindings.stickY, &result.gamepadBindings.trigger,
			  &result.gamepadBindings.grip, &result.gamepadBindings.primary, &result.gamepadBindings.secondary,
			  &result.gamepadBindings.menu, &result.gamepadBindings.stickClick})
			for (auto& binding : *bindings)
				binding = reader.Text();
		result.gamepadBindings.gyroHold = reader.Text();
		result.gamepadBindings.deadzone = reader.Number() / 1000.0f;
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 4)
			throw std::runtime_error("Unknown retention settings version");
		result.snapshotBudgetMiB = reader.Number();
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 5)
			throw std::runtime_error("Unknown view settings version");
		result.inspectorFolded = reader.Count(1) != 0;
		result.timelineFolded = reader.Count(1) != 0;
		result.hudFolded = reader.Count(1) != 0;
		for (auto count = reader.Count(32); count; --count)
		{
			PreviewCrop crop;
			crop.profile = reader.Text();
			for (auto* value : {&crop.x0, &crop.y0, &crop.x1, &crop.y1})
				*value = reader.Number() / 100000.0f;
			result.crops.push_back(std::move(crop));
		}
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 6)
			throw std::runtime_error("Unknown recent apps settings version");
		for (auto count = reader.Count(MaxRecentApps); count; --count)
		{
			RecentApp recent;
			recent.launch = ReadProfile(reader);
			const std::uint64_t low = reader.Number(), high = reader.Number();
			recent.lastUsed = static_cast<std::int64_t>(high << 32 | low);
			result.recent.push_back(std::move(recent));
		}
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 7)
			throw std::runtime_error("Unknown private Proton settings version");
		if (reader.Count(32) != result.profiles.size())
			throw std::runtime_error("Private Proton profile count mismatch");
		for (auto& profile : result.profiles)
			profile.privateProtonPrefix = reader.Text();
		if (reader.Count(MaxRecentApps) != result.recent.size())
			throw std::runtime_error("Private Proton recent app count mismatch");
		for (auto& recent : result.recent)
			recent.launch.privateProtonPrefix = reader.Text();
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 8)
			throw std::runtime_error("Unknown pointer settings version");
		result.remoteMouseDrag = reader.Count(1) != 0;
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 9)
			throw std::runtime_error("Unknown history settings version");
		result.previewBudgetMiB = reader.Number();
	}
	if (!reader.Empty())
	{
		if (reader.Number() != 10)
			throw std::runtime_error("Unknown compressed history settings version");
		result.previewRamMiB = reader.Number();
	}
	if (!reader.Empty())
		throw std::runtime_error("Unexpected trailing companion settings data");
	ValidateSettings(result);
	return result;
}
void SaveSettings(const std::filesystem::path& path, const CompanionSettings& settings)
{
	ValidateSettings(settings);
	if (!path.is_absolute())
		throw std::invalid_argument("Companion settings destination must be absolute");
	std::string bytes(s_magic);
	Put(bytes, static_cast<std::uint32_t>(std::lround(settings.scale * 1000)));
	Put(bytes, settings.endpoint);
	Put(bytes, settings.activeProfile);
	Put(bytes, settings.activeLayout);
	Put(bytes, static_cast<std::uint32_t>(settings.profiles.size()));
	for (const auto& profile : settings.profiles)
		PutProfile(bytes, profile);
	Put(bytes, static_cast<std::uint32_t>(settings.layouts.size()));
	for (const auto& layout : settings.layouts)
	{
		Put(bytes, layout.name);
		Put(bytes, layout.ini);
	}
	Put(bytes, 2);
	Put(bytes, static_cast<std::uint32_t>(std::lround(settings.inspectorWidth * 1000)));
	Put(bytes, static_cast<std::uint32_t>(std::lround(settings.timelineHeight * 1000)));
	Put(bytes, static_cast<std::uint32_t>(settings.railCollapsed));
	Put(bytes, static_cast<std::uint32_t>(settings.timelineResized));
	Put(bytes, 3);
	for (const auto& binding : settings.pilotBindings)
	{
		Put(bytes, binding.key);
		Put(bytes, binding.modifiers);
	}
	for (const auto* bindings :
		 {&settings.gamepadBindings.stickX, &settings.gamepadBindings.stickY, &settings.gamepadBindings.trigger,
		  &settings.gamepadBindings.grip, &settings.gamepadBindings.primary, &settings.gamepadBindings.secondary,
		  &settings.gamepadBindings.menu, &settings.gamepadBindings.stickClick})
		for (const auto& binding : *bindings)
			Put(bytes, binding);
	Put(bytes, settings.gamepadBindings.gyroHold);
	Put(bytes, static_cast<std::uint32_t>(std::min(999L, std::lround(settings.gamepadBindings.deadzone * 1000))));
	Put(bytes, std::uint32_t{4});
	Put(bytes, settings.snapshotBudgetMiB);
	Put(bytes, std::uint32_t{5});
	Put(bytes, static_cast<std::uint32_t>(settings.inspectorFolded));
	Put(bytes, static_cast<std::uint32_t>(settings.timelineFolded));
	Put(bytes, static_cast<std::uint32_t>(settings.hudFolded));
	Put(bytes, static_cast<std::uint32_t>(settings.crops.size()));
	for (const auto& crop : settings.crops)
	{
		Put(bytes, crop.profile);
		// Keep valid intervals nonempty at the stored fixed-point precision.
		const auto x0 = std::min(99999L, std::lround(crop.x0 * 100000));
		const auto y0 = std::min(99999L, std::lround(crop.y0 * 100000));
		const auto x1 = std::max(x0 + 1, std::lround(crop.x1 * 100000));
		const auto y1 = std::max(y0 + 1, std::lround(crop.y1 * 100000));
		for (const auto value : {x0, y0, x1, y1})
			Put(bytes, static_cast<std::uint32_t>(value));
	}
	Put(bytes, std::uint32_t{6});
	Put(bytes, static_cast<std::uint32_t>(settings.recent.size()));
	for (const auto& recent : settings.recent)
	{
		PutProfile(bytes, recent.launch);
		const auto lastUsed = static_cast<std::uint64_t>(recent.lastUsed);
		Put(bytes, static_cast<std::uint32_t>(lastUsed));
		Put(bytes, static_cast<std::uint32_t>(lastUsed >> 32));
	}
	Put(bytes, std::uint32_t{7});
	Put(bytes, static_cast<std::uint32_t>(settings.profiles.size()));
	for (const auto& profile : settings.profiles)
		Put(bytes, profile.privateProtonPrefix);
	Put(bytes, static_cast<std::uint32_t>(settings.recent.size()));
	for (const auto& recent : settings.recent)
		Put(bytes, recent.launch.privateProtonPrefix);
	Put(bytes, std::uint32_t{8});
	Put(bytes, static_cast<std::uint32_t>(settings.remoteMouseDrag));
	Put(bytes, std::uint32_t{9});
	Put(bytes, settings.previewBudgetMiB);
	Put(bytes, std::uint32_t{10});
	Put(bytes, settings.previewRamMiB);
	if (bytes.size() > s_maxFile)
		throw std::invalid_argument("Companion settings exceed the file budget");
	std::filesystem::create_directories(path.parent_path());
	const auto temporary = CreatePrivateTemporaryFile(path);
	try
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("Cannot open companion settings temporary file");
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		if (!output)
			throw std::runtime_error("Cannot write companion settings");
		output.close();
		if (!output)
			throw std::runtime_error("Cannot close companion settings temporary file");
		CommitTemporaryFile(temporary, path);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}
std::string LoadWindowLayout(const std::filesystem::path& path)
{
	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	if (error)
		throw std::runtime_error("Cannot inspect window layout: " + error.message());
	if (!exists)
		return {};
	const auto size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("Cannot read window layout size: " + error.message());
	if (size > s_maxFile)
		throw std::runtime_error("Window layout exceeds 8 MiB");
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open window layout");
	std::string text(static_cast<std::size_t>(size), '\0');
	if (!input.read(text.data(), static_cast<std::streamsize>(text.size())))
		throw std::runtime_error("Cannot read window layout");
	if (input.peek() != std::char_traits<char>::eof() || input.bad())
		throw std::runtime_error("Cannot read complete window layout");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close window layout");
	if (text.find('\0') != std::string::npos)
		throw std::runtime_error("Window layout contains NUL");
	return text;
}
void SaveWindowLayout(const std::filesystem::path& path, std::string_view text)
{
	if (!path.is_absolute())
		throw std::invalid_argument("Window layout destination must be absolute");
	if (text.size() > s_maxFile || text.find('\0') != std::string_view::npos)
		throw std::invalid_argument("Window layout exceeds 8 MiB or contains NUL");
	std::filesystem::create_directories(path.parent_path());
	const auto temporary = CreatePrivateTemporaryFile(path);
	try
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("Cannot open window layout temporary file");
		if (!text.empty())
			output.write(text.data(), static_cast<std::streamsize>(text.size()));
		if (!output)
			throw std::runtime_error("Cannot write window layout");
		output.close();
		if (!output)
			throw std::runtime_error("Cannot close window layout temporary file");
		CommitTemporaryFile(temporary, path);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}
} // namespace simxr::app
