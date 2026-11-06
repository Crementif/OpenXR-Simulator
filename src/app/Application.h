#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <utility>

namespace simxr::app
{
struct ApplicationOptions
{
	unsigned smokeFrames = 0;
	bool hidden = false;
	bool windowless = false;
	bool validation = false;
	std::string screenshot;
	double screenshotDelay = 0;
	std::optional<std::pair<int, int>> windowSize;
	int proofDescriptor = -1;
	bool proofVerifyPixels = false;
	std::string controlSocket;
	bool runtimeSmoke = false;
	std::string imageExport;
	std::optional<std::uint16_t> mcpPort;
	std::string mcpToken;
	std::intptr_t mcpBootstrapDescriptor = -2;
	std::optional<std::string> launchProfile;
};

class Application
{
public:
	explicit Application(ApplicationOptions options);
	int Run();

private:
	ApplicationOptions m_options;
};
} // namespace simxr::app
