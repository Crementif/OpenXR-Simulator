#include "VulkanImageDeliveryProbe.h"
#include "Pch.h"

#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace simxr::probe
{
static std::uint64_t ParseId(std::string_view text)
{
	std::uint64_t value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
		throw std::invalid_argument("Session, generation and capture must be positive uint64 values");
	return value;
}
} // namespace simxr::probe

int main(int argc, char** argv)
{
	try
	{
		if (argc < 6 || argc > 7 || (argc == 7 && std::string_view(argv[6]) != "--verify-pixels"))
			throw std::invalid_argument("Expected socket session generation capture gate [--verify-pixels]");
		simxr::probe::VerifyControlImage(argv[1], simxr::probe::ParseId(argv[2]), simxr::probe::ParseId(argv[3]),
										 simxr::probe::ParseId(argv[4]), argv[5], argc == 7);
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: native control image consumer: %s\n", error.what());
		return 1;
	}
}
