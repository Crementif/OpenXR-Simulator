#include "../TestSupport.h"
#include "platform/linux/RuntimeFiles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace simxr::tests
{
void TestRuntimeFiles()
{
	const auto directory = std::filesystem::temp_directory_path() / ("simxr-files-" + std::to_string(getpid()));
	std::filesystem::remove_all(directory);
	std::filesystem::create_directories(directory);

	struct Cleanup
	{
		std::filesystem::path path;

		~Cleanup()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} cleanup{directory};

	const auto output = directory / "données-画面";
	RuntimeLog log(output);
	Require(log.Log("joined writer"), "Log rejected its first entry");
	log.Close();
	Require(log.Status().closed && !log.Status().errors && !log.Log("late"), "Log did not close cleanly");
	const auto name = "openxr_simulator." + std::to_string(getpid()) + ".log";
	std::ifstream input(output / name);
	const std::string text(std::istreambuf_iterator<char>(input), {});
	Require(text == "[SimXR Linux] joined writer\n", "Shutdown did not drain accepted logs");
	std::filesystem::create_directory(directory / name);
	RuntimeLog failing(directory);
	Require(failing.Log("cannot write"), "Failure test was not admitted");
	failing.Close();
	Require(failing.Status().closed && failing.Status().errors == 1 && !failing.Status().detail.empty(),
			"Log failure was lost or prevented shutdown");
}
}

int main()
{
	try
	{
		simxr::tests::TestRuntimeFiles();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
