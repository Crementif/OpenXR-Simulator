#include "app/services/LaunchTypes.h"
#include "Pch.h"
#include "app/services/Utf8Path.h"
#include "simulation/HeadsetProfiles.h"

namespace simxr::app
{
std::string ProfileLaunchCommand(const std::filesystem::path& executable, std::string_view profile,
								 LaunchCommandShell shell)
{
	const auto path = PathText(executable);
	if (!executable.is_absolute() || path.find('\0') != std::string::npos || profile.empty() ||
		profile.find('\0') != std::string_view::npos)
		throw std::invalid_argument("A saved profile and an absolute simulator path are required");
	const auto quote = [shell](std::string_view value) {
		std::string result = "'";
		for (const auto character : value)
		{
			if (character == '\'')
				result += shell == LaunchCommandShell::Posix ? "'\\''" : "''";
			else
				result += character;
		}
		return result + "'";
	};
	if (shell == LaunchCommandShell::PowerShell)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string encoded;
		encoded.reserve(profile.size() * 2);
		for (const unsigned char character : profile)
		{
			encoded += digits[character >> 4];
			encoded += digits[character & 15];
		}
		return "& " + quote(path) + " '--launch-profile-hex=" + encoded + "'";
	}
	return quote(path) + " " + quote("--launch-profile=" + std::string(profile));
}

std::string DecodeProfileLaunchName(std::string_view encoded)
{
	if (encoded.empty() || encoded.size() % 2)
		throw std::invalid_argument("Invalid encoded launch profile name");
	const auto digit = [](char character) -> unsigned {
		if (character >= '0' && character <= '9')
			return static_cast<unsigned>(character - '0');
		if (character >= 'a' && character <= 'f')
			return static_cast<unsigned>(character - 'a' + 10);
		throw std::invalid_argument("Invalid encoded launch profile name");
	};
	std::string result;
	result.reserve(encoded.size() / 2);
	for (std::size_t index = 0; index < encoded.size(); index += 2)
	{
		const auto character = static_cast<char>((digit(encoded[index]) << 4) | digit(encoded[index + 1]));
		if (!character)
			throw std::invalid_argument("Invalid encoded launch profile name");
		result += character;
	}
	return result;
}

std::string LinuxSteamRuntimeOption(const std::filesystem::path& manifest)
{
	const auto path = PathText(manifest);
	if (!manifest.is_absolute() || path.find('\0') != std::string::npos || path.find("%command%") != std::string::npos)
		throw std::invalid_argument("Steam requires an absolute runtime manifest path without %command%");
	std::string quoted = "'";
	for (const auto character : path)
	{
		if (character == '\'')
			quoted += "'\\''";
		else
			quoted += character;
	}
	quoted += '\'';
	return "XR_RUNTIME_JSON=" + quoted + " %command%";
}

static std::string Lower(std::string text)
{
	std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

// The quoted value after key in a Steam .acf file, or empty.
static std::string AcfValue(const std::string& text, std::string_view key)
{
	const auto quotedKey = "\"" + std::string(key) + "\"";
	const auto at = Lower(text).find(Lower(quotedKey));
	if (at == std::string::npos)
		return {};
	const auto open = text.find('"', at + quotedKey.size());
	const auto close = open == std::string::npos ? open : text.find('"', open + 1);
	return close == std::string::npos ? std::string{} : text.substr(open + 1, close - open - 1);
}

std::optional<std::string> SteamAppId(const std::filesystem::path& executable)
{
	// A game lives in <library>/steamapps/common/<installdir>/; its manifest names that folder.
	std::filesystem::path library;
	std::string folder;
	for (auto part = executable.begin(); part != executable.end(); ++part)
	{
		const auto next = std::next(part);
		if (Lower(PathText(*part)) == "steamapps" && next != executable.end() && Lower(PathText(*next)) == "common" &&
			std::next(next) != executable.end())
		{
			folder = Lower(PathText(*std::next(next)));
			break;
		}
		library /= *part;
	}
	if (folder.empty())
		return std::nullopt;
	std::error_code error;
	for (std::filesystem::directory_iterator entry(library / "steamapps", error), end; !error && entry != end;
		 entry.increment(error))
	{
		const auto name = PathText(entry->path().filename());
		if (!name.starts_with("appmanifest_") || !name.ends_with(".acf"))
			continue;
		std::ifstream input(entry->path(), std::ios::binary);
		const std::string text{std::istreambuf_iterator<char>(input), {}};
		const auto id = AcfValue(text, "appid");
		if (Lower(AcfValue(text, "installdir")) == folder && !id.empty() &&
			std::ranges::all_of(id, [](unsigned char c) { return std::isdigit(c); }))
			return id;
	}
	return std::nullopt;
}

LaunchProfile ResolveLaunchProfile(LaunchProfile profile, const std::filesystem::path& applicationDirectory)
{
	ValidateLaunchProfile(profile);
	// Started outside Steam, a Steam game restarts itself through Steam and loses this launch's runtime; naming
	// its app id lets it run in place.
	if (const auto id = SteamAppId(Utf8Path(profile.executable)))
		for (const auto* name : {"SteamAppId", "SteamGameId"})
			if (std::ranges::none_of(profile.environment, [&](const auto& entry) { return entry.first == name; }))
				profile.environment.emplace_back(name, *id);
	if (profile.useBundledRuntime)
	{
		if (!applicationDirectory.is_absolute())
			throw std::invalid_argument("Cannot locate the OpenXR Simulator installation");
		const auto manifest = applicationDirectory / "openxr_simulator.json";
		std::error_code error;
		if (!std::filesystem::is_regular_file(manifest, error))
			throw std::runtime_error("Cannot find OpenXR Simulator runtime manifest: " + PathText(manifest) +
									 (error ? " (" + error.message() + ")" : ""));
		std::ifstream input(manifest, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot read OpenXR Simulator runtime manifest: " + PathText(manifest));
		input.close();
		if (!input)
			throw std::runtime_error("Cannot close OpenXR Simulator runtime manifest: " + PathText(manifest));
		profile.runtimeManifest = PathText(manifest);
		profile.useBundledRuntime = false;
	}
	return profile;
}

void ValidateLaunchProfile(const LaunchProfile& profile)
{
	const auto validateText = [](const std::string& text) {
		if (text.size() > 1024 * 1024 || text.find('\0') != std::string::npos)
			throw std::invalid_argument("Launch text exceeds its bound or contains NUL");
	};
	validateText(profile.name);
	validateText(profile.executable);
	validateText(profile.workingDirectory);
	validateText(profile.runtimeManifest);
	validateText(profile.headsetProfile);
	validateText(profile.rig);
	validateText(profile.privateProtonPrefix);
	if (profile.useBundledRuntime && !profile.runtimeManifest.empty())
		throw std::invalid_argument("Bundled runtime cannot have a custom manifest");
	if (!profile.privateProtonPrefix.empty() && !profile.useBundledRuntime && profile.runtimeManifest.empty())
		throw std::invalid_argument("Private Proton prefix requires a runtime manifest");
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
} // namespace simxr::app
