#include "app/platform/StartupFailure.h"
#include "Pch.h"

#include <SDL3/SDL_messagebox.h>

namespace simxr::app
{
std::string FormatStartupFailure(std::string_view message)
{
	constexpr std::size_t lineLimit = 48;
	std::string result;
	while (!message.empty())
	{
		const auto newline = message.find('\n');
		auto line = message.substr(0, newline);
		while (line.size() > lineLimit)
		{
			auto end = lineLimit;
			while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80)
				--end;
			if (!end)
				end = lineLimit;
			auto split = line.rfind(' ', end);
			if (split == std::string_view::npos || split == 0)
				split = end;
			result.append(line.substr(0, split));
			result += '\n';
			line.remove_prefix(split);
			while (!line.empty() && line.front() == ' ')
				line.remove_prefix(1);
		}
		result.append(line);
		if (newline == std::string_view::npos)
			break;
		result += '\n';
		message.remove_prefix(newline + 1);
	}
	return result;
}

void ShowStartupFailure(std::string_view message)
{
	const auto formatted = FormatStartupFailure(message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenXR Simulator", formatted.c_str(), nullptr);
}
} // namespace simxr::app
