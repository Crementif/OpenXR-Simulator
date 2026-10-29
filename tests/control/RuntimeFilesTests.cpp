#include "../TestSupport.h"
#include "platform/linux/RuntimeFiles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
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
	const auto source = directory / "poses-画面.jsonl";
	{
		std::ofstream output(source);
		output << R"({"frame":0,"head":{"x":2},"capture":true})" << '\n';
	}
	const auto program = LoadStartupSequence(source);
	std::filesystem::remove(source);
	Require(program && program->Size() == 1 && program->Steps()[0].capture &&
		program->Steps()[0].head->position[0] == 2 && !program->Loops(), "Startup program depends on its source file");
	Require(!LoadStartupSequence({}), "Unset startup path created a program");
	const auto reject = [](const auto& path)
	{
		bool rejected = false;
		try
		{
			LoadStartupSequence(path);
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Require(rejected, "Invalid startup file was admitted");
	};
	reject(source);
	reject(directory);
	Require(mkfifo(source.c_str(), 0600) == 0, "Create startup FIFO");
	reject(source);
	std::filesystem::remove(source);
	std::filesystem::create_symlink(directory, source);
	reject(source);
	std::filesystem::remove(source);
	std::ofstream(source).close();
	std::filesystem::resize_file(source, 16 * 1024 * 1024 + 1);
	reject(source);

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
