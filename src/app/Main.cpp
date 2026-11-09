#include "app/UiPch.h"
#include "app/Application.h"
#include "app/platform/StartupFailure.h"
#include "app/services/LaunchTypes.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpTools.h"
#include "simxr/Version.h"
#if defined(__linux__)
#include "platform/linux/InstallUse.h"
#elif defined(_WIN32)
#include "platform/windows/InstallUse.h"
#include "app/platform/windows/UpdateTransport.h"
#endif

#include <SDL3/SDL_main.h>

static bool WriteStandardOutput(std::string_view text)
{
#ifdef _WIN32
	const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!output || output == INVALID_HANDLE_VALUE)
		return false;
	std::size_t offset = 0;
	while (offset < text.size())
	{
		DWORD written = 0;
		const auto count = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 65536));
		if (!WriteFile(output, text.data() + offset, count, &written, nullptr) || !written)
			return false;
		offset += written;
	}
	return true;
#else
	return std::fwrite(text.data(), 1, text.size(), stdout) == text.size() && std::fflush(stdout) == 0;
#endif
}

#ifdef _WIN32
static int RunUpdateWorker(std::string_view repository)
{
	try
	{
		return WriteStandardOutput(simxr::app::windows::FetchLatestReleaseDirect(repository)) ? 0 : 1;
	}
	catch (const std::exception&)
	{
		return 1;
	}
}
#endif

// The MCP connector lists tools without starting the simulator.
static int PrintMcpTools(int argc, char** argv)
{
	const bool windowless = argc == 3 && std::string_view(argv[2]) == "--windowless";
	if (argc > 3 || (argc == 3 && !windowless))
		return 2;
	try
	{
		return WriteStandardOutput(simxr::app::McpToolDefinitions(windowless).dump()) ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "OpenXR Simulator: %s\n", error.what());
		return 1;
	}
}

static std::optional<std::string_view> ReadValue(int argc, char** argv, int& index)
{
	if (index + 1 >= argc || !argv[index + 1][0] || std::string_view(argv[index + 1]).starts_with("--"))
	{
		std::fprintf(stderr, "%s requires a nonempty value before the next option\n", argv[index]);
		return {};
	}
	return argv[++index];
}

int main(int argc, char** argv)
{
#if defined(__linux__)
	if (!simxr::InstallationReady())
	{
		std::fputs("OpenXR Simulator installation is being updated or is no longer available. Try again.\n", stderr);
		return 1;
	}
#elif defined(_WIN32)
	simxr::windows::InstallUse installUse;
	if (!installUse.Ready())
	{
		std::fputs("OpenXR Simulator installation is being updated or removed. Try again.\n", stderr);
		return 1;
	}
#endif
	if (argc == 2 && std::string_view(argv[1]).starts_with("--update-worker="))
	{
#ifdef _WIN32
		return RunUpdateWorker(std::string_view(argv[1]).substr(std::string_view("--update-worker=").size()));
#else
		return 2;
#endif
	}
	if (argc >= 2 && std::string_view(argv[1]) == "--mcp-tools")
		return PrintMcpTools(argc, argv);
	simxr::app::ApplicationOptions options;
	for (int index = 1; index < argc; ++index)
	{
		const std::string_view argument = argv[index];
		if (argument == "--version")
		{
			std::printf("simxr_companion %s\n", simxr::Version);
			return 0;
		}
		if (argument == "--help")
		{
			std::puts(
				"Usage: simxr_companion [--connect SOCKET] [--hidden] [--windowless] [--validation] [--smoke-frames N] "
				"[--screenshot PATH [--screenshot-delay SECONDS]] [--window-size WIDTHxHEIGHT] "
				"[--runtime-smoke [--export PATH]] [--proof-consumer [FD]] "
				"[--proof-verify-pixels] [--mcp-http PORT] [--launch-profile=NAME]"
#ifdef SIMXR_MCP_DISCOVERY
				" [--mcp]"
#endif
			);
			return 0;
		}
		if (argument.starts_with("--launch-profile-hex="))
		{
			try
			{
				if (options.launchProfile)
					throw std::invalid_argument("Duplicate launch profile");
				options.launchProfile = simxr::app::DecodeProfileLaunchName(
					argument.substr(std::string_view("--launch-profile-hex=").size()));
			}
			catch (const std::exception& error)
			{
				std::fprintf(stderr, "%s\n", error.what());
				return 2;
			}
		}
		else if (argument.starts_with("--launch-profile="))
		{
			const auto value = argument.substr(std::string_view("--launch-profile=").size());
			if (value.empty() || options.launchProfile)
			{
				std::fputs("--launch-profile requires one nonempty saved profile name\n", stderr);
				return 2;
			}
			options.launchProfile = std::string(value);
		}
		else if (argument == "--hidden")
			options.hidden = true;
		else if (argument == "--windowless")
			options.windowless = true;
#ifdef SIMXR_MCP_DISCOVERY
		else if (argument == "--mcp")
		{
			if (options.mcpBootstrapDescriptor != -2)
				return 2;
			options.mcpBootstrapDescriptor = -1;
		}
#ifdef _WIN32
		else if (argument == "--mcp-bootstrap-handle")
#else
		else if (argument == "--mcp-bootstrap-fd")
#endif
		{
			const auto value = ReadValue(argc, argv, index);
			if (!value || options.mcpBootstrapDescriptor != -2)
				return 2;
			std::intptr_t descriptor = -1;
			const auto parsed = std::from_chars(value->data(), value->data() + value->size(), descriptor);
			if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || descriptor < 3)
				return 2;
			options.mcpBootstrapDescriptor = descriptor;
		}
#endif
		else if (argument == "--validation")
			options.validation = true;
		else if (argument == "--runtime-smoke")
			options.runtimeSmoke = true;
		else if (argument == "--connect" || argument == "--export" || argument == "--screenshot" ||
				 argument == "--mcp-http" || argument == "--smoke-frames")
		{
			const auto value = ReadValue(argc, argv, index);
			if (!value)
				return 2;
			if (argument == "--connect")
				options.controlSocket = *value;
			else if (argument == "--export")
				options.imageExport = *value;
			else if (argument == "--screenshot")
				options.screenshot = *value;
			else if (argument == "--mcp-http")
			{
				unsigned port = 0;
				const auto result = std::from_chars(value->data(), value->data() + value->size(), port);
				if (result.ec != std::errc{} || result.ptr != value->data() + value->size() || port > 65535 ||
					options.mcpPort)
				{
					std::fputs("--mcp-http requires one port from 0 to 65535\n", stderr);
					return 2;
				}
				options.mcpPort = static_cast<std::uint16_t>(port);
			}
			else
			{
				const auto result = std::from_chars(value->data(), value->data() + value->size(), options.smokeFrames);
				if (result.ec != std::errc{} || result.ptr != value->data() + value->size() || options.smokeFrames < 8)
				{
					std::fputs("--smoke-frames requires an integer >= 8\n", stderr);
					return 2;
				}
			}
		}
		else if (argument == "--window-size")
		{
			const auto value = ReadValue(argc, argv, index);
			if (!value)
				return 2;
			int width = 0, height = 0;
			const auto separator = value->find('x');
			const auto end = value->data() + value->size();
			const bool parsed =
				separator != std::string_view::npos &&
				std::from_chars(value->data(), value->data() + separator, width).ptr == value->data() + separator &&
				std::from_chars(value->data() + separator + 1, end, height).ptr == end;
			if (!parsed || width < 320 || height < 240 || width > 16384 || height > 16384)
			{
				std::fputs("--window-size requires WIDTHxHEIGHT in pixels, at least 320x240\n", stderr);
				return 2;
			}
			options.windowSize = std::pair{width, height};
		}
		else if (argument == "--screenshot-delay")
		{
			const auto value = ReadValue(argc, argv, index);
			if (!value)
				return 2;
			const auto end = value->data() + value->size();
			const auto result = std::from_chars(value->data(), end, options.screenshotDelay);
			if (result.ec != std::errc{} || result.ptr != end || !(options.screenshotDelay >= 0) ||
				options.screenshotDelay > 3600)
			{
				std::fputs("--screenshot-delay requires seconds from 0 to 3600\n", stderr);
				return 2;
			}
		}
		else if (argument == "--proof-consumer")
		{
			options.proofDescriptor = 3;
			if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--"))
			{
				const std::string_view number = argv[++index];
				const auto result =
					std::from_chars(number.data(), number.data() + number.size(), options.proofDescriptor);
				if (result.ec != std::errc{} || result.ptr != number.data() + number.size() ||
					options.proofDescriptor < 0)
				{
					std::fputs("--proof-consumer requires a nonnegative file descriptor\n", stderr);
					return 2;
				}
			}
		}
		else if (argument == "--proof-verify-pixels")
			options.proofVerifyPixels = true;
		else
		{
			std::fprintf(stderr, "Usage: simxr_companion [--hidden] [--validation] [--smoke-frames N] [--screenshot "
								 "PATH] [--connect SOCKET] [--runtime-smoke [--export PATH]] "
								 "[--proof-consumer [FD]] [--proof-verify-pixels]\n");
			return 2;
		}
	}
	if ((options.windowless &&
		 (options.hidden || options.validation || options.smokeFrames || options.runtimeSmoke ||
		  options.proofDescriptor >= 0 || !options.screenshot.empty() || !options.controlSocket.empty())) ||
		(options.launchProfile && (options.windowless || options.smokeFrames || options.runtimeSmoke ||
								   options.proofDescriptor >= 0 || !options.controlSocket.empty())) ||
		(options.proofVerifyPixels && options.proofDescriptor < 0) ||
		(options.runtimeSmoke && (options.controlSocket.empty() || options.proofDescriptor >= 0)) ||
		(!options.imageExport.empty() && !options.runtimeSmoke) ||
		(options.proofDescriptor >= 0 && !options.controlSocket.empty()) ||
		((options.mcpPort || options.mcpBootstrapDescriptor != -2) &&
		 (options.proofDescriptor >= 0 || options.runtimeSmoke || options.smokeFrames)) ||
		(options.mcpPort && options.mcpBootstrapDescriptor != -2) ||
		(options.screenshotDelay && (options.screenshot.empty() || options.smokeFrames)) ||
		(options.windowSize && (options.windowless || options.smokeFrames)))
	{
		std::fputs("--runtime-smoke requires --connect and excludes proof mode; --export requires --runtime-smoke; "
				   "--proof-verify-pixels requires --proof-consumer; proof mode excludes --connect; "
				   "--mcp-http excludes finite smoke and proof modes; --launch-profile excludes windowless, "
				   "smoke, proof and --connect modes; --screenshot-delay requires --screenshot and excludes smoke; "
				   "--window-size excludes windowless and smoke modes\n",
				   stderr);
		return 2;
	}
	if (options.windowless && !options.mcpPort && options.mcpBootstrapDescriptor == -2)
		options.mcpBootstrapDescriptor = -1;
	const bool finite = options.smokeFrames || options.proofDescriptor >= 0 || options.runtimeSmoke;
	const bool showFailure =
		!options.hidden && !options.windowless && !finite && !options.mcpPort && options.mcpBootstrapDescriptor == -2;
	try
	{
		const auto bootstrapToken = simxr::app::TakeMcpToken();
		if (options.mcpBootstrapDescriptor != -2)
		{
			options.mcpPort = 0;
			options.mcpToken = options.mcpBootstrapDescriptor < 0 ? simxr::app::NewMcpIdentifier() : bootstrapToken;
		}
		else if (options.mcpPort)
			options.mcpToken = bootstrapToken;
		return simxr::app::Application(std::move(options)).Run();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "OpenXR Simulator: %s\n", error.what());
		if (showFailure)
			simxr::app::ShowStartupFailure(error.what());
		const std::string_view message = error.what();
		return finite && (message.starts_with("Unable to initialize the desktop window") ||
						  message.starts_with("Unable to open a Vulkan window"))
				   ? 77
				   : 1;
	}
}
