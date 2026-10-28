#pragma once

#include <string>
#include <string_view>

namespace simxr::app
{
std::string FormatStartupFailure(std::string_view message);
void ShowStartupFailure(std::string_view message);
} // namespace simxr::app
