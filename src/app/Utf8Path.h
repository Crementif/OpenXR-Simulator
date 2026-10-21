#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace simxr::app
{
inline std::filesystem::path Utf8Path(std::string_view value)
{
	return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
} // namespace simxr::app
