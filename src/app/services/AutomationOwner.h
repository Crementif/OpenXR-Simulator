#pragma once

#include <atomic>

namespace simxr::app
{
// The MCP session revokes this token without waiting for the application thread.
struct AutomationOwner
{
	std::atomic<bool> active{true};
};
} // namespace simxr::app
