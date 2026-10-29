#include "Pch.h"
#include "app/platform/windows/ManifestAlias.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/PrivateSecurity.h"
#include "app/services/Utf8Path.h"

#include <nlohmann/json.hpp>

namespace simxr::app::windows
{
static constexpr std::size_t MaxManifestBytes = 128 * 1024;
static constexpr unsigned MaxManifestFiles = 256;

static bool AsciiPath(const std::filesystem::path& path)
{
	return path.is_absolute() &&
		   std::ranges::all_of(path.native(), [](wchar_t character) { return character <= 0x7f; });
}

static std::filesystem::path CacheDirectory()
{
	std::wstring temp(32768, L'\0');
	const auto length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
	if (!length || length >= temp.size())
		return {};
	temp.resize(length);
	std::vector<std::filesystem::path> candidates{LoaderManifestPath(temp)};
	PrivateSecurity security(true);
	for (const auto& parent : candidates)
	{
		if (!AsciiPath(parent))
			continue;
		const auto directory = parent / (L"OpenXRSimulator-ManifestCache-" + security.SidText());
		if (!CreateDirectoryW(directory.c_str(), security.Attributes()) && GetLastError() != ERROR_ALREADY_EXISTS)
			continue;
		Handle handle(CreateFileW(directory.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
								  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
								  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		if (handle.Get() == INVALID_HANDLE_VALUE)
			continue;
		BY_HANDLE_FILE_INFORMATION info{};
		if (!GetFileInformationByHandle(handle.Get(), &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			continue;
		try
		{
			security.Check(handle.Get(), SE_FILE_OBJECT, true);
			return directory;
		}
		catch (const std::exception&)
		{
			continue;
		}
	}
	return {};
}

static bool ExistingMatches(const std::filesystem::path& path, std::string_view content)
{
	Handle handle(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
							  nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (handle.Get() == INVALID_HANDLE_VALUE)
		return false;
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(handle.Get(), &info) ||
		(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nFileSizeHigh ||
		info.nFileSizeLow != content.size())
		return false;
	try
	{
		PrivateSecurity(false).Check(handle.Get(), SE_FILE_OBJECT, false);
	}
	catch (const std::exception&)
	{
		return false;
	}
	std::string bytes(content.size(), '\0');
	DWORD read = 0;
	return ReadFile(handle.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
		   read == bytes.size() && bytes == content;
}

static std::filesystem::path StoreManifest(const std::filesystem::path& directory, std::string_view content)
{
	PrivateSecurity security(false);
	for (unsigned slot = 0; slot < MaxManifestFiles; ++slot)
	{
		const auto path = directory / (L"manifest-" + std::to_wstring(slot) + L".json");
		const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, security.Attributes(), CREATE_NEW,
									  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			if ((GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) &&
				ExistingMatches(path, content))
				return path;
			continue;
		}
		DWORD written = 0;
		const bool success = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
							 written == content.size() && FlushFileBuffers(file);
		const bool closed = CloseHandle(file) != FALSE;
		if (success && closed)
			return path;
		// Leave an incomplete slot untouched: a concurrent launch may already have opened it.
	}
	return {};
}

std::wstring PrepareManifestAlias(const std::wstring& manifest)
{
	const std::filesystem::path source(manifest);
	if (!source.is_absolute())
		return manifest;
	std::ifstream file(source, std::ios::binary);
	if (!file)
		return manifest;
	std::string content(MaxManifestBytes + 1, '\0');
	file.read(content.data(), static_cast<std::streamsize>(content.size()));
	const auto count = file.gcount();
	if (file.bad() || count < 0 || static_cast<std::size_t>(count) > MaxManifestBytes)
		return manifest;
	file.clear();
	file.close();
	if (file.fail())
		return manifest;
	content.resize(static_cast<std::size_t>(count));
	try
	{
		auto json = nlohmann::json::parse(content, nullptr, true, true);
		auto& library = json.at("runtime").at("library_path");
		const auto name = library.get<std::string>();
		if (name.find('\0') != std::string::npos)
			return manifest;
		if (name.find_first_of("/\\") != std::string::npos)
		{
			const auto path = Utf8Path(name);
			if (!path.is_absolute())
			{
				if (path.has_root_name() || path.has_root_directory())
					return manifest;
				library = PathText(source.parent_path() / path);
			}
		}
		const auto serialized = json.dump();
		if (serialized.size() > MaxManifestBytes)
			return manifest;
		const auto directory = CacheDirectory();
		if (directory.empty())
			return manifest;
		const auto stored = StoreManifest(directory, serialized);
		return stored.empty() ? manifest : stored.native();
	}
	catch (const std::exception&)
	{
		// Preserve the original for loaders that support this manifest or path directly.
		return manifest;
	}
}
} // namespace simxr::app::windows
