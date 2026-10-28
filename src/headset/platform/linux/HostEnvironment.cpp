#include "Pch.h"
#include "headset/RuntimeSelection.h"

namespace simxr::headset
{
void PrepareHostEnvironment(const std::filesystem::path& manifest)
{
	const auto result = manifest.empty() ? unsetenv("XR_RUNTIME_JSON") : setenv("XR_RUNTIME_JSON", manifest.c_str(), 1);
	if (result != 0)
		throw std::runtime_error("Cannot select the headset host runtime");
}
} // namespace simxr::headset
