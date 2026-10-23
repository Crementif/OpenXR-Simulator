#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace simxr::app::windows
{
std::wstring Wide(std::string_view value);
std::string Utf8(std::wstring_view value);
std::string Environment(const char* key);
std::wstring Quote(std::wstring_view argument);
std::runtime_error Error(const char* operation, DWORD code = GetLastError());
class Handle
{
  public:
	explicit Handle(HANDLE handle = nullptr);
	~Handle();
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept;
	Handle& operator=(Handle&& other) noexcept;
	HANDLE Get() const noexcept
	{
		return m_value;
	}

  private:
	HANDLE m_value = nullptr;
};
} // namespace simxr::app::windows
