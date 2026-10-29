#include "headset/HostControl.h"
#include "Pch.h"

#include <windows.h>

namespace simxr::headset
{
void ValidateHostControlPipe()
{
	const auto input = GetStdHandle(STD_INPUT_HANDLE);
	if (!input || input == INVALID_HANDLE_VALUE || GetFileType(input) != FILE_TYPE_PIPE)
		throw std::invalid_argument("Parent control requires a standard-input pipe");
}

bool HostStopRequested()
{
	DWORD available = 0;
	if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr))
	{
		const auto error = GetLastError();
		if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
			return true;
		throw std::runtime_error("Cannot poll parent control pipe");
	}
	return available != 0;
}
} // namespace simxr::headset
