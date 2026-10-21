#include "app/Application.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <string_view>

int main(int argc, char** argv)
{
	simxr::app::ApplicationOptions options;
	for (int index = 1; index < argc; ++index)
	{
		const std::string_view argument = argv[index];
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
		else if (argument == "--connect" && index + 1 < argc)
			options.controlSocket = argv[++index];
		else if (argument == "--runtime-smoke")
			options.runtimeSmoke = true;
		else if (argument == "--export" && index + 1 < argc)
			options.imageExport = argv[++index];
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
					return 2;
			}
		}
		else if (argument == "--proof-verify-pixels")
			options.proofVerifyPixels = true;
		else if (argument == "--screenshot" && index + 1 < argc)
			options.screenshot = argv[++index];
		else if (argument == "--smoke-frames" && index + 1 < argc)
		{
			const std::string_view count = argv[++index];
			auto result = std::from_chars(count.data(), count.data() + count.size(), options.smokeFrames);
			if (result.ec != std::errc{} || result.ptr != count.data() + count.size() || options.smokeFrames < 8)
			{
				std::fprintf(stderr, "--smoke-frames requires an integer >= 8\n");
				return 2;
			}
		}
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
	try
	{
		return simxr::app::Application(options).Run();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Companion: %s\n", error.what());
		const std::string_view message = error.what();
		return (options.smokeFrames || options.proofDescriptor >= 0) &&
					   (message.starts_with("SDL video unavailable") ||
						message.starts_with("SDL Vulkan window unavailable"))
				   ? 77
				   : 1;
	}
}
