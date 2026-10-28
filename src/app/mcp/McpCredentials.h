#pragma once

#include <string>

namespace simxr::app
{
// Reads and removes the bootstrap credential before any child is launched.
std::string TakeMcpToken();
std::string NewMcpIdentifier();
}
