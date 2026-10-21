#pragma once

#include "control/ProtocolTypes.h"

namespace simxr::control
{
struct ControlRequestError
{
	ErrorCode error;
	const char* detail;
};
} // namespace simxr::control
