#include "VulkanImageDeliveryProbe.h"

#include <cstdio>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
	try
	{
		if (argc < 6 || argc > 7 || (argc == 7 && std::string(argv[6]) != "--verify-pixels"))
			throw std::invalid_argument("Expected socket session generation capture gate [--verify-pixels]");
		simxr::probe::VerifyControlImage(argv[1], std::stoull(argv[2]), std::stoull(argv[3]), std::stoull(argv[4]),
										 argv[5], argc == 7);
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: native control image consumer: %s\n", error.what());
		return 1;
	}
}
