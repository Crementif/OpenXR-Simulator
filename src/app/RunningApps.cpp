#include "app/RunningApps.h"
#include "app/RuntimeEndpoints.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace simxr::app
{
namespace
{
// control.<pid>.sock on Linux, SimXR.Control.<pid> on Windows; zero when the
// endpoint does not name a process.
std::uint64_t EndpointProcess(const std::string& endpoint)
{
	auto name = std::filesystem::path(endpoint).filename().string();
	if (name.ends_with(".sock"))
		name.resize(name.size() - 5);
	const auto dot = name.rfind('.');
	if (dot == std::string::npos)
		return 0;
	std::uint64_t process = 0;
	const auto* first = name.data() + dot + 1;
	const auto* last = name.data() + name.size();
	const auto [end, error] = std::from_chars(first, last, process);
	return error == std::errc{} && end == last ? process : 0;
}
std::string BaseName(std::string_view path)
{
	const auto slash = path.find_last_of("/\\");
	return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}
#ifdef __linux__
std::string ReadFile(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	std::ostringstream text;
	text << input.rdbuf();
	return text.str();
}
std::vector<std::string> NulSeparated(const std::string& text)
{
	std::vector<std::string> values;
	for (std::size_t begin = 0; begin < text.size();)
	{
		const auto end = text.find('\0', begin);
		values.push_back(text.substr(begin, end - begin));
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return values;
}
std::string Link(const std::string& path)
{
	std::error_code error;
	const auto target = std::filesystem::read_symlink(path, error);
	return error ? std::string{} : target.string();
}
bool Answers(const std::string& endpoint, std::chrono::milliseconds timeout)
{
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (endpoint.size() >= sizeof(address.sun_path))
		return false;
	std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return false;
	bool alive = ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
	// A full backlog still means a listening runtime.
	if (!alive && errno == EAGAIN)
		alive = true;
	else if (!alive && errno == EINPROGRESS)
	{
		pollfd wait{descriptor, POLLOUT, 0};
		int error = 0;
		socklen_t size = sizeof(error);
		alive = ::poll(&wait, 1, static_cast<int>(timeout.count())) == 1 &&
				::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
	}
	::close(descriptor);
	return alive;
}
std::optional<std::chrono::system_clock::time_point> ProcessStart(std::uint64_t process)
{
	const auto stat = ReadFile("/proc/" + std::to_string(process) + "/stat");
	const auto name = stat.rfind(')');
	if (name == std::string::npos)
		return {};
	// Field 22, counted from the state field (3) after the parenthesised name.
	std::istringstream fields(stat.substr(name + 1));
	std::string field;
	for (int index = 3; index < 22 && fields >> field; ++index)
	{
	}
	unsigned long long ticks = 0;
	if (!(fields >> ticks))
		return {};
	std::istringstream system(ReadFile("/proc/stat"));
	for (std::string key; system >> key;)
		if (key == "btime")
		{
			long long boot = 0;
			const long rate = ::sysconf(_SC_CLK_TCK);
			if (!(system >> boot) || rate <= 0)
				return {};
			return std::chrono::system_clock::time_point(std::chrono::seconds(boot)) +
				   std::chrono::milliseconds(ticks * 1000 / static_cast<unsigned long long>(rate));
		}
	return {};
}
std::string ProcessName(std::uint64_t process)
{
	const auto base = "/proc/" + std::to_string(process);
	const auto arguments = NulSeparated(ReadFile(base + "/cmdline"));
	if (!arguments.empty() && !arguments.front().empty())
		return BaseName(arguments.front());
	auto name = ReadFile(base + "/comm");
	while (!name.empty() && (name.back() == '\n' || name.back() == '\0'))
		name.pop_back();
	return name;
}
#endif
} // namespace
std::vector<RunningApp> DiscoverRunningApps(std::chrono::milliseconds timeout)
{
	std::vector<RunningApp> apps;
	for (auto& endpoint : DiscoverRuntimeEndpoints())
	{
		RunningApp app;
		app.process = EndpointProcess(endpoint);
#ifdef __linux__
		std::error_code error;
		if (app.process && !std::filesystem::exists("/proc/" + std::to_string(app.process), error))
			continue;
		if (!Answers(endpoint, timeout))
			continue;
		if (app.process)
		{
			app.name = ProcessName(app.process);
			app.started = ProcessStart(app.process);
		}
#endif
		if (app.name.empty())
			app.name = std::filesystem::path(endpoint).filename().string();
		app.endpoint = std::move(endpoint);
		apps.push_back(std::move(app));
	}
	return apps;
}
std::optional<LaunchProfile> DescribeProcess(std::uint64_t process)
{
#ifdef __linux__
	if (!process)
		return {};
	const auto base = "/proc/" + std::to_string(process);
	auto arguments = NulSeparated(ReadFile(base + "/cmdline"));
	if (arguments.empty())
		return {};
	LaunchProfile profile;
	std::error_code error;
	const std::filesystem::path first = arguments.front();
	profile.executable = first.is_absolute() && std::filesystem::exists(first, error) ? first.string()
																					  : Link(base + "/exe");
	profile.name = BaseName(arguments.front());
	profile.arguments.assign(arguments.begin() + 1, arguments.end());
	profile.workingDirectory = Link(base + "/cwd");
	// Only what the simulator and Wine need to start the app the same way again.
	for (const auto& entry : NulSeparated(ReadFile(base + "/environ")))
	{
		const auto equals = entry.find('=');
		if (equals == std::string::npos || equals == 0)
			continue;
		auto key = entry.substr(0, equals);
		auto value = entry.substr(equals + 1);
		if (key == "XR_RUNTIME_JSON")
			profile.runtimeManifest = std::move(value);
		else if (key == "SIMXR_DATA_DIR" || key == "WINEPREFIX")
			profile.environment.emplace_back(std::move(key), std::move(value));
	}
	if (profile.arguments.size() > 256)
		profile.arguments.resize(256);
	return profile;
#else
	(void)process;
	return {};
#endif
}
} // namespace simxr::app
