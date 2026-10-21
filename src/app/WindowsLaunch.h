#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <windows.h>

namespace simxr::app::windows
{
inline std::wstring Wide(std::string_view value)
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
inline std::string Utf8(std::wstring_view value)
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
inline std::string Environment(const char* key)
{
	const auto name = Wide(key);
	const auto size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
	if (!size)
		return {};
	std::wstring value(size, L'\0');
	const auto read = GetEnvironmentVariableW(name.c_str(), value.data(), size);
	if (!read)
		return {};
	if (read >= size)
		throw std::runtime_error("Environment changed while preparing launch");
	value.resize(read);
	return Utf8(value);
}
inline std::wstring Quote(std::wstring_view argument)
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
inline std::runtime_error Error(const char* operation, DWORD code = GetLastError())
{
	wchar_t* buffer = nullptr;
	const auto size =
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
	const std::unique_ptr<wchar_t, decltype(&LocalFree)> message(buffer, LocalFree);
	const auto detail = size ? Utf8(std::wstring_view(message.get(), size)) : std::string{};
	return std::runtime_error(std::string(operation) + " (Windows " + std::to_string(code) + "): " + detail);
}
class Handle
{
	HANDLE value = nullptr;

  public:
	explicit Handle(HANDLE handle = nullptr) : value(handle) {}
	~Handle()
	{
		if (value && value != INVALID_HANDLE_VALUE)
			CloseHandle(value);
	}
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
	Handle& operator=(Handle&& other) noexcept
	{
		if (this != &other)
		{
			if (value && value != INVALID_HANDLE_VALUE)
				CloseHandle(value);
			value = std::exchange(other.value, nullptr);
		}
		return *this;
	}
	HANDLE Get() const
	{
		return value;
	}
};
} // namespace simxr::app::windows
#endif
