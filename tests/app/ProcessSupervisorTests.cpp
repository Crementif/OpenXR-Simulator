#include "../TestSupport.h"
#include "app/ProcessLauncher.h"
#include "app/ProcessSupervisor.h"

#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
using namespace simxr::app;
using namespace std::chrono_literals;
struct Directory
{
	std::filesystem::path path;
	Directory()
	{
		char temporary[] = "/tmp/simxr-process-supervisor-XXXXXX";
		Require(::mkdtemp(temporary) != nullptr, "Create process supervisor fixture");
		path = temporary;
	}
	~Directory()
	{
		std::filesystem::remove_all(path);
	}
};
LaunchProfile Profile(const std::filesystem::path& directory, const std::string& name)
{
	LaunchProfile profile;
	profile.name = name;
	profile.executable = std::filesystem::read_symlink("/proc/self/exe").string();
	profile.workingDirectory = directory.string();
	profile.arguments = {"--controlled-child", name};
	return profile;
}
pid_t Running(ProcessLauncher& launcher, const std::filesystem::path& ready)
{
	pid_t process = 0;
	Eventually(
		[&]
		{
			const auto snapshot = launcher.Snapshot();
			process = static_cast<pid_t>(snapshot.process);
			return snapshot.state == LaunchState::Running && process > 0 && std::filesystem::exists(ready);
		},
		"Child did not reach controlled running boundary");
	return process;
}
void ObserverLifetime()
{
	Directory directory;
	ProcessSupervisor supervisor;
	pid_t process = 0;
	const auto closing = [&]
	{
		ProcessLauncher launcher(supervisor);
		Require(launcher.Launch(Profile(directory.path, "orphan")), "Admit observed child");
		process = Running(launcher, directory.path / "orphan.ready");
		Require(!launcher.Launch(Profile(directory.path, "duplicate")), "An active launcher admitted another child");
		return std::chrono::steady_clock::now();
	}();
	Require(std::chrono::steady_clock::now() - closing < 1s, "Closing observation waited for child exit");
	Require(supervisor.Outstanding() == 1 && ::kill(process, 0) == 0, "Closing observation lost or killed the child");
	std::ofstream(directory.path / "orphan.continue").put('1');
	Eventually(
		[&]
		{
			return supervisor.Outstanding() == 0;
		},
		"Unobserved child was not retired by its supervisor");
	int status = 0;
	errno = 0;
	Require(::waitpid(process, &status, WNOHANG) == -1 && errno == ECHILD,
			"Child became a zombie after its launcher observation disappeared");
}
void IndependentChildren()
{
	Directory directory;
	ProcessSupervisor supervisor;
	ProcessLauncher first(supervisor), second(supervisor);
	Require(first.Launch(Profile(directory.path, "first")) && second.Launch(Profile(directory.path, "second")),
			"Independent launch observations were not admitted");
	Running(first, directory.path / "first.ready");
	const auto signaled = Running(second, directory.path / "second.ready");
	const auto unrelated = ::fork();
	Require(unrelated >= 0, "Create unrelated fixture child");
	if (!unrelated)
		::_exit(19);
	std::ofstream(directory.path / "first.continue").put('1');
	Require(::kill(signaled, SIGTERM) == 0, "Signal supervised fixture child");
	Eventually(
		[&]
		{
			return supervisor.Outstanding() == 0;
		},
		"Independent child observations did not settle");
	Require(first.Snapshot().state == LaunchState::Exited && first.Snapshot().exitCode == 37 &&
				second.Snapshot().state == LaunchState::Exited && second.Snapshot().exitCode == 128 + SIGTERM,
			"Exit code or signal result changed during supervision");
	int status = 0;
	Require(::waitpid(unrelated, &status, 0) == unrelated && WIFEXITED(status) && WEXITSTATUS(status) == 19,
			"Supervisor reaped a child it did not own");
}
void FinalShutdownLeavesGameRunning()
{
	Directory directory;
	pid_t process = 0;
	auto closing = std::chrono::steady_clock::now();
	{
		ProcessSupervisor supervisor;
		ProcessLauncher launcher(supervisor);
		Require(launcher.Launch(Profile(directory.path, "shutdown")), "Admit shutdown fixture child");
		process = Running(launcher, directory.path / "shutdown.ready");
		closing = std::chrono::steady_clock::now();
	}
	Require(std::chrono::steady_clock::now() - closing < 1s && ::kill(process, 0) == 0,
			"Final supervisor shutdown waited for or killed the independent game");
	std::ofstream(directory.path / "shutdown.continue").put('1');
	// This fixture keeps its parent alive after simulated application shutdown,
	// so the harness explicitly assumes final reaping instead of exiting.
	int status = 0;
	Eventually(
		[&]
		{
			return ::waitpid(process, &status, WNOHANG) == process;
		},
		"Final child did not finish");
	Require(WIFEXITED(status) && WEXITSTATUS(status) == 37, "Final child could not continue after supervisor shutdown");
}
int Child(const std::string& name)
{
	std::ofstream(name + ".ready").put('1');
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!std::filesystem::exists(name + ".continue"))
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return 73;
		std::this_thread::sleep_for(5ms);
	}
	return 37;
}
} // namespace
int main(int argc, char** argv)
{
	if (argc == 3 && std::string_view(argv[1]) == "--controlled-child")
		return Child(argv[2]);
	try
	{
		struct sigaction before{}, after{};
		Require(::sigaction(SIGCHLD, nullptr, &before) == 0, "Read existing SIGCHLD action");
		ObserverLifetime();
		IndependentChildren();
		FinalShutdownLeavesGameRunning();
		Require(::sigaction(SIGCHLD, nullptr, &after) == 0 && before.sa_handler == after.sa_handler &&
					before.sa_flags == after.sa_flags,
				"Supervision changed the process-wide SIGCHLD policy");
		std::cout << "PASS child observation lifetime, owned reaping and final shutdown\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
