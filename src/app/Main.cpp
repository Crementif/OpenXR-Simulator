#include "Pch.h"
#include "app/Application.h"
#include "simxr/Version.h"

#include <SDL3/SDL_main.h>
#include <charconv>
#include <cstdio>
#include <exception>

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
			std::puts("Usage: simxr_companion [--connect SOCKET] [--hidden] [--validation] [--smoke-frames N] "
					  "[--screenshot PATH] [--runtime-smoke [--export PATH]] [--proof-consumer [FD]] "
					  "[--proof-verify-pixels]");
			return 0;
		}
		if (argument == "--hidden")
			options.hidden = true;
		else if (argument == "--validation")
			options.validation = true;
		else if (argument == "--runtime-smoke")
			options.runtimeSmoke = true;
		else if (argument == "--connect" || argument == "--export" || argument == "--screenshot" ||
				 argument == "--smoke-frames")
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
	if ((options.proofVerifyPixels && options.proofDescriptor < 0) ||
		(options.runtimeSmoke && (options.controlSocket.empty() || options.proofDescriptor >= 0)) ||
		(!options.imageExport.empty() && !options.runtimeSmoke) ||
		(options.proofDescriptor >= 0 && !options.controlSocket.empty()))
	{
		std::fputs("--runtime-smoke requires --connect and excludes proof mode; --export requires --runtime-smoke; "
				   "--proof-verify-pixels requires --proof-consumer; proof mode excludes --connect\n",
				   stderr);
		return 2;
	}
	const bool finite = options.smokeFrames || options.proofDescriptor >= 0 || options.runtimeSmoke;
	try
	{
		return simxr::app::Application(std::move(options)).Run();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Companion: %s\n", error.what());
		const std::string_view message = error.what();
		return finite && (message.starts_with("SDL video/gamepad initialization failed") ||
						  message.starts_with("SDL Vulkan window unavailable"))
				   ? 77
				   : 1;
	}
}
