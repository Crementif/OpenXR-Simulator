#pragma once

#include <cstddef>
#include <string>

namespace simxr::app
{
struct McpUiState
{
	bool available = false, listening = false, stopping = false, startedForSession = false, discoverable = false;
	bool stopRequested = false, retryRequested = false;
	std::size_t sessions = 0, activeCalls = 0;
	std::string error, configuration, codexConfiguration, configurationError, httpConfiguration;
};
} // namespace simxr::app
