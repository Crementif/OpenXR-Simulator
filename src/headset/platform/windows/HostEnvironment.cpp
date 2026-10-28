#include "Pch.h"
#include "headset/RuntimeSelection.h"

namespace simxr::headset
{
void PrepareHostEnvironment(const std::filesystem::path& manifest)
{
	if (_wputenv_s(L"XR_RUNTIME_JSON", manifest.c_str()) != 0)
		throw std::runtime_error("Cannot select the headset host runtime");
}
} // namespace simxr::headset
