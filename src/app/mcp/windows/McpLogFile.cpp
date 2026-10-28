#include "Pch.h"
#include "app/mcp/McpLogFile.h"
#include "app/platform/windows/LaunchSupport.h"

namespace simxr::app::mcp
{
std::optional<LogFileSnapshot> ReadLogFile(const std::filesystem::path& path)
{
	const auto raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (raw == INVALID_HANDLE_VALUE)
	{
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return {};
		throw windows::Error("Cannot open runtime log", error);
	}
	windows::Handle file(raw);
	FILE_ATTRIBUTE_TAG_INFO attributes{};
	LARGE_INTEGER size{};
	if (GetFileType(raw) != FILE_TYPE_DISK || !GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
		(attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
		throw std::runtime_error("Runtime log must be a regular non-reparse file");
	if (!GetFileSizeEx(raw, &size) || size.QuadPart < 0)
		throw windows::Error("Cannot inspect runtime log");
	LogFileSnapshot result;
	result.size = static_cast<std::uint64_t>(size.QuadPart);
	constexpr std::size_t maximum = 256 * 1024;
	const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(result.size, maximum));
	result.truncated = result.size > count;
	LARGE_INTEGER offset{};
	offset.QuadPart = static_cast<LONGLONG>(result.size - count);
	if (!SetFilePointerEx(raw, offset, nullptr, FILE_BEGIN))
		throw windows::Error("Cannot seek runtime log");
	result.bytes.resize(count);
	std::size_t read = 0;
	while (read < count)
	{
		DWORD amount = 0;
		if (!ReadFile(raw, result.bytes.data() + read, static_cast<DWORD>(count - read), &amount, nullptr))
			throw windows::Error("Cannot read runtime log");
		if (!amount)
			break;
		read += amount;
	}
	result.bytes.resize(read);
	if (!GetFileSizeEx(raw, &size))
		throw windows::Error("Cannot recheck runtime log");
	result.changed = read != count || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != result.size;
	return result;
}
bool ClearLogFile(const std::filesystem::path& path)
{
	const auto raw = CreateFileW(path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (raw == INVALID_HANDLE_VALUE)
	{
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return false;
		throw windows::Error("Cannot open runtime log for clearing", error);
	}
	struct File
	{
		HANDLE value;
		~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
	} file{raw};
	FILE_ATTRIBUTE_TAG_INFO attributes{};
	BY_HANDLE_FILE_INFORMATION info{};
	if (GetFileType(raw) != FILE_TYPE_DISK || !GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
		(attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
		!GetFileInformationByHandle(raw, &info) || info.nNumberOfLinks != 1)
		throw std::runtime_error("Runtime log must be a regular non-reparse file with one link to clear it");
	LARGE_INTEGER beginning{};
	if (!SetFilePointerEx(raw, beginning, nullptr, FILE_BEGIN))
		throw windows::Error("Cannot seek runtime log for clearing");
	if (!SetEndOfFile(raw))
		throw windows::Error("Log clear may have completed; cannot truncate runtime log");
	file.value = INVALID_HANDLE_VALUE;
	if (!CloseHandle(raw))
		throw windows::Error("Log clear completed, but closing the file failed");
	return true;
}

}
