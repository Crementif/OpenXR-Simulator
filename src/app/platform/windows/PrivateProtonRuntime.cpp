#include "app/platform/windows/PrivateProtonRuntime.h"
#include "Pch.h"
#include "app/platform/windows/LaunchSupport.h"

namespace simxr::app::windows
{
static constexpr const wchar_t* s_marker = L".simxr-private-proton";

static std::string ReadText(const std::filesystem::path& path, std::size_t maximum)
{
	if (!std::filesystem::is_regular_file(path))
		throw std::runtime_error("Private Proton metadata or manifest is not a regular file: " + Utf8(path.native()));
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open private Proton metadata or manifest");
	std::string text;
	std::array<char, 4096> bytes;
	while (input.read(bytes.data(), bytes.size()) || input.gcount())
	{
		const auto count = static_cast<std::size_t>(input.gcount());
		if (count > maximum - text.size())
			throw std::runtime_error("Private Proton metadata or manifest exceeds its size limit");
		text.append(bytes.data(), count);
	}
	if (input.bad() || !input.eof())
		throw std::runtime_error("Cannot read private Proton metadata or manifest");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close private Proton metadata or manifest");
	if (text.find('\0') != std::string::npos)
		throw std::runtime_error("Private Proton metadata or manifest contains NUL");
	(void)Wide(text);
	return text;
}

static std::filesystem::path LocalPath(const std::string& text)
{
	const std::filesystem::path path(Wide(text));
	const auto root = path.root_name().native();
	if (!path.is_absolute() || root.size() != 2 || root[1] != L':' ||
		!((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')))
		throw std::invalid_argument("Private Proton prefix and runtime manifest must use absolute local drive paths");
	return path;
}

static Handle OpenDirectory(const std::filesystem::path& path)
{
	Handle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
								 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
	if (directory.Get() == INVALID_HANDLE_VALUE)
		throw Error("Cannot inspect private Proton directory");
	return directory;
}

static BY_HANDLE_FILE_INFORMATION DirectoryIdentity(const Handle& directory)
{
	BY_HANDLE_FILE_INFORMATION information{};
	if (!GetFileInformationByHandle(directory.Get(), &information))
		throw Error("Cannot identify private Proton directory");
	if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		throw std::runtime_error("Private Proton path is not a directory");
	return information;
}

static void RequireSameDirectory(const Handle& first, const Handle& second)
{
	const auto left = DirectoryIdentity(first), right = DirectoryIdentity(second);
	if (left.dwVolumeSerialNumber != right.dwVolumeSerialNumber || left.nFileIndexHigh != right.nFileIndexHigh ||
		left.nFileIndexLow != right.nFileIndexLow)
		throw std::runtime_error("Selected private Proton prefix is not the companion's current prefix");
}

static std::filesystem::path RecordedPrefix(const std::string& metadata)
{
	std::array<std::string_view, 6> lines;
	std::string_view remaining(metadata);
	for (auto& line : lines)
	{
		const auto end = remaining.find('\n');
		if (end == std::string_view::npos)
			throw std::runtime_error("Incomplete private Proton ownership marker");
		line = remaining.substr(0, end);
		remaining.remove_prefix(end + 1);
	}
	std::uint32_t uid = 0;
	const auto parsed = std::from_chars(lines[2].data(), lines[2].data() + lines[2].size(), uid);
	if (!remaining.empty() || lines[0] != "SIMXR-PRIVATE-PROTON-1" || lines[1].size() != 32 ||
		!std::ranges::all_of(lines[1], [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
		parsed.ec != std::errc{} || parsed.ptr != lines[2].data() + lines[2].size() || !lines[3].starts_with('/') ||
		!lines[5].starts_with('/'))
		throw std::runtime_error("Invalid private Proton ownership marker");
	std::string expected = "Z:";
	for (const auto character : lines[3])
		expected += character == '/' ? '\\' : character;
	expected += "\\pfx";
	if (lines[4] != expected)
		throw std::runtime_error("Private Proton marker does not identify its provisioned prefix");
	return LocalPath(std::string(lines[4]));
}

static void RequireRuntimeLibrary(const std::filesystem::path& path)
{
	if (!std::filesystem::is_regular_file(path))
		throw std::runtime_error("Private Proton runtime DLL is missing");
	std::ifstream input(path, std::ios::binary);
	IMAGE_DOS_HEADER dos{};
	IMAGE_NT_HEADERS64 image{};
	if (!input.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
		dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024)
		throw std::runtime_error("Private Proton runtime DLL has an invalid PE header");
	input.seekg(dos.e_lfanew);
	if (!input.read(reinterpret_cast<char*>(&image), sizeof(image)) || image.Signature != IMAGE_NT_SIGNATURE ||
		image.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || !(image.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
		image.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		throw std::runtime_error("Private Proton registration requires an AMD64 simulator runtime DLL");
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close private Proton runtime DLL");
}

static std::filesystem::path RuntimeManifest(const std::string& requested)
{
	const auto manifest = std::filesystem::canonical(LocalPath(requested));
	const auto document = ReadText(manifest, 65536);
	std::string compact;
	bool quoted = false;
	for (const auto character : document)
	{
		if (character == '"')
			quoted = !quoted;
		if (quoted || (character != ' ' && character != '\t' && character != '\r' && character != '\n'))
			compact += character;
	}
	for (const auto* library : {"openxr_simulator.dll", "libopenxr_simulator.dll"})
	{
		const std::string expected =
			std::string(R"({"file_format_version":"1.0.0","runtime":{"library_path":"./)") + library + R"("}})";
		if (compact == expected)
		{
			RequireRuntimeLibrary(manifest.parent_path() / library);
			return manifest;
		}
	}
	throw std::runtime_error("Private Proton registration requires the generated simulator runtime manifest");
}

void RegisterPrivateProtonRuntime(const std::string& selected, const std::string& requestedManifest)
{
	const auto ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll || !GetProcAddress(ntdll, "wine_get_version"))
		throw std::runtime_error("Private Proton registration is available only inside Wine/Proton");
	auto prefixPath = LocalPath(selected).lexically_normal();
	if (prefixPath.filename().empty())
		prefixPath = prefixPath.parent_path();
	if (prefixPath.filename() != L"pfx")
		throw std::invalid_argument("Select the provisioned private Proton pfx directory");
	const auto prefix = OpenDirectory(prefixPath);
	const auto compatibility = OpenDirectory(prefixPath.parent_path());
	(void)DirectoryIdentity(compatibility);
	if (!std::filesystem::is_regular_file(prefixPath / s_marker) ||
		!std::filesystem::is_regular_file(prefixPath.parent_path() / s_marker))
		throw std::runtime_error(
			"Private Proton ownership marker is missing. Use prepare-proton-prefix.py to create a new private prefix");
	const auto metadata = ReadText(prefixPath / s_marker, 16384);
	if (metadata != ReadText(prefixPath.parent_path() / s_marker, 16384))
		throw std::runtime_error("Private Proton ownership markers do not match. Use prepare-proton-prefix.py");
	const auto recorded = OpenDirectory(RecordedPrefix(metadata));
	RequireSameDirectory(prefix, recorded);
	const auto drive = OpenDirectory(prefixPath / L"drive_c");
	const auto current = OpenDirectory(L"C:\\");
	RequireSameDirectory(drive, current);
	const auto manifest = RuntimeManifest(requestedManifest);
	const auto& value = manifest.native();
	if (value.size() >= std::numeric_limits<DWORD>::max() / sizeof(wchar_t))
		throw std::invalid_argument("Private Proton manifest path exceeds the registry value limit");
	HKEY opened = nullptr;
	const auto created =
		RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, nullptr, REG_OPTION_NON_VOLATILE,
						KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &opened, nullptr);
	if (created != ERROR_SUCCESS)
		throw Error("Cannot open runtime registration in the selected private Proton prefix", created);
	const std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(opened, RegCloseKey);
	const auto written =
		RegSetValueExW(key.get(), L"ActiveRuntime", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
					   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
	if (written != ERROR_SUCCESS)
		throw Error("Cannot register runtime in the selected private Proton prefix", written);
}
} // namespace simxr::app::windows
