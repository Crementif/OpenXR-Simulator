#include "control/RuntimeEndpoint.h"
#include "Pch.h"

namespace simxr::control
{
std::string RuntimeEndpoint(std::uint64_t process)
{
	return R"(\\.\pipe\SimXR.Control.)" + std::to_string(process);
}
} // namespace simxr::control
