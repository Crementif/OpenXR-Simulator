#include "app/platform/windows/LaunchSupport.h"
#include "Pch.h"

namespace simxr::app::windows
{
// An ASCII short path lets narrow-stream OpenXR loaders open Unicode manifests
// without copying the runtime or changing the manifest's relative DLL path.
std::wstring LoaderManifestPath(const std::wstring& path)
{
	if (!std::filesystem::path(path).is_absolute() ||
		std::ranges::all_of(path, [](wchar_t character) { return character <= 0x7f; }))
		return path;
	const auto required = GetShortPathNameW(path.c_str(), nullptr, 0);
	if (!required || required > 32768)
		return path;
	std::wstring result(required, L'\0');
	const auto written = GetShortPathNameW(path.c_str(), result.data(), required);
	if (!written || written >= required)
		return path;
	result.resize(written);
	return std::ranges::all_of(result, [](wchar_t character) { return character <= 0x7f; }) ? result : path;
}

std::wstring Wide(std::string_view value)
{
	if (value.empty())
		return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		throw std::invalid_argument("UTF-8 text is too long");
	const auto size =
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (!size)
		throw std::invalid_argument("Launch text is not valid UTF-8");
	std::wstring result(size, L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
							 size))
		throw std::invalid_argument("Cannot convert launch text to Unicode");
	return result;
}
std::string Utf8(std::wstring_view value)
{
	if (value.empty())
		return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		throw std::invalid_argument("Unicode text is too long");
	const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
										  nullptr, 0, nullptr, nullptr);
	if (!size)
		throw std::invalid_argument("Launch text is not valid Unicode");
	std::string result(size, '\0');
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
							 size, nullptr, nullptr))
		throw std::invalid_argument("Cannot convert launch text to UTF-8");
	return result;
}
std::string Environment(const char* key)
{
	const auto name = Wide(key);
	SetLastError(ERROR_SUCCESS);
	const auto size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
	if (!size)
	{
		const auto error = GetLastError();
		if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND)
			throw Error("Cannot read launch environment", error);
		return {};
	}
	std::wstring value(size, L'\0');
	SetLastError(ERROR_SUCCESS);
	const auto read = GetEnvironmentVariableW(name.c_str(), value.data(), size);
	if (!read)
	{
		const auto error = GetLastError();
		if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND)
			throw Error("Cannot read launch environment", error);
		return {};
	}
	if (read >= size)
		throw std::runtime_error("Environment changed while preparing launch");
	value.resize(read);
	return Utf8(value);
}
std::wstring Quote(std::wstring_view argument)
{
	// Microsoft CRT argv parsing: double trailing backslashes and escape embedded quotes.
	std::wstring result(1, L'"');
	std::size_t slashes = 0;
	for (const auto character : argument)
	{
		if (character == L'\\')
		{
			++slashes;
			continue;
		}
		result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		result.push_back(character);
		slashes = 0;
	}
	result.append(slashes * 2, L'\\');
	result.push_back(L'"');
	return result;
}
std::runtime_error Error(const char* operation, DWORD code)
{
	wchar_t* buffer = nullptr;
	const auto size =
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
	const std::unique_ptr<wchar_t, decltype(&LocalFree)> message(buffer, LocalFree);
	const auto detail = size ? Utf8(std::wstring_view(message.get(), size)) : std::string{};
	return std::runtime_error(std::string(operation) + " (Windows " + std::to_string(code) + "): " + detail);
}
Handle::Handle(HANDLE handle) : m_value(handle) {}
Handle::~Handle()
{
	if (m_value && m_value != INVALID_HANDLE_VALUE)
		CloseHandle(m_value);
}
Handle::Handle(Handle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
Handle& Handle::operator=(Handle&& other) noexcept
{
	if (this != &other)
	{
		if (m_value && m_value != INVALID_HANDLE_VALUE)
			CloseHandle(m_value);
		m_value = std::exchange(other.m_value, nullptr);
	}
	return *this;
}
} // namespace simxr::app::windows
