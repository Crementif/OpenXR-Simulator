#include "FakeImageInspection.h"
#include "control/LocalControlServer.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

int main(int argc, char** argv)
{
	try
	{
		if (argc != 5)
			throw std::invalid_argument("Expected socket, stop, export-ready and destroy-session marker paths");
		simxr::control::test::Fixture fixture;
		simxr::control::LocalControlServer server{fixture.service, argv[1]};
		std::printf("READY CPU image fixture capture=%llu; memfd/pipe only, no GPU qualification\n",
					static_cast<unsigned long long>(fixture.capture.value));
		std::fflush(stdout);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
		bool destroyed = false;
		while (!std::filesystem::exists(argv[2]))
		{
			if (std::filesystem::exists(argv[3]))
				fixture.state->exportPhase = simxr::CompletionStatus::Complete;
			if (!destroyed && std::filesystem::exists(argv[4]))
			{
				Require(fixture.runtime.DestroySession(fixture.session) == XR_SUCCESS,
						"Destroy fixture public session");
				destroyed = true;
			}
			Require(std::chrono::steady_clock::now() < deadline, "CPU image fixture timed out");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		server.Close();
		Require(fixture.state->reads == 0 || fixture.state->exports != 0, "Implicit fixture readback");
		std::printf("PASS CPU image endpoint shares=%u exposures=%u explicit_reads=%u\n", fixture.state->shares.load(),
					fixture.state->exposures.load(), fixture.state->reads.load());
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
