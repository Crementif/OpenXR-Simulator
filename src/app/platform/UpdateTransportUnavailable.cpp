#include "app/UiPch.h"
#include "app/services/UpdateChecker.h"

namespace simxr::app
{
std::string FetchLatestRelease(std::string_view)
{
	throw std::runtime_error("Update checks are unavailable on this platform");
}
} // namespace simxr::app
