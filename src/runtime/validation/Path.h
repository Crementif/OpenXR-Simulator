#pragma once

#include <openxr/openxr.h>
#include <string_view>

namespace simxr
{
inline bool IsValidXrPath(std::string_view path)
{
	if (path.size() < 2 || path.size() >= XR_MAX_PATH_LENGTH || path.front() != '/' || path.back() == '/')
		return false;
	bool segmentHasName = false;
	for (char character : path.substr(1))
	{
		if (character == '/')
		{
			if (!segmentHasName)
				return false;
			segmentHasName = false;
			continue;
		}
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
			  character == '-' || character == '.'))
			return false;
		segmentHasName |= character != '.';
	}
	return segmentHasName;
}
} // namespace simxr
