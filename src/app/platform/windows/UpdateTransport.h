#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace simxr::app::windows
{
// The caller owns the worker and supplies only a trusted destination.
std::string FetchUpdateResponse(std::wstring_view host, std::uint16_t port, std::wstring_view path, bool secure);
std::string FetchLatestReleaseDirect(std::string_view repository);
std::string RunUpdateWorker(std::wstring_view argument, std::wstring_view executable = {});
}
