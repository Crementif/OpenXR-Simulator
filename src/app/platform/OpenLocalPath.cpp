#include "app/UiPch.h"
#include "app/platform/OpenLocalPath.h"
#include "app/services/Utf8Path.h"

namespace simxr::app
{
std::string LocalFileUri(const std::filesystem::path& path)
{
	const auto text = PathText(path);
	std::string uri = "file://";
	constexpr char hex[] = "0123456789ABCDEF";
	for (const auto value : text)
	{
		const auto byte = static_cast<unsigned char>(value);
		if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
			(byte >= '0' && byte <= '9') || byte == '/' || byte == '-' || byte == '.' || byte == '_' || byte == '~')
			uri.push_back(static_cast<char>(byte));
		else
		{
			uri.push_back('%');
			uri.push_back(hex[byte >> 4]);
			uri.push_back(hex[byte & 15]);
		}
	}
	return uri;
}
std::string OpenLocalPath(const std::filesystem::path& path)
{
	const auto uri = LocalFileUri(path);
	return SDL_OpenURL(uri.c_str()) ? std::string{} : std::string(SDL_GetError());
}
} // namespace simxr::app
