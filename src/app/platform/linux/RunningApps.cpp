#include "app/platform/RunningApps.h"
#include "Pch.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace simxr::app
{
static std::optional<std::string> ReadFile(const std::filesystem::path& path)
{
	constexpr std::size_t maxBytes = 8 * 1024 * 1024;
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return {};
	std::string text;
	std::array<char, 4096> buffer;
	while (input)
	{
		input.read(buffer.data(), buffer.size());
		const auto count = static_cast<std::size_t>(input.gcount());
		if (count > maxBytes - text.size())
			return {};
		text.append(buffer.data(), count);
	}
	if (!input.eof() || input.bad())
		return {};
	input.clear();
	input.close();
	if (!input)
		return {};
	return text;
}
static std::optional<std::vector<std::string>> NulSeparated(const std::string& text, std::size_t limit)
{
	if (!text.empty() && text.back() != '\0')
		return {};
	std::vector<std::string> values;
	for (std::size_t begin = 0; begin < text.size();)
	{
		if (values.size() == limit)
			return {};
		const auto end = text.find('\0', begin);
		values.push_back(text.substr(begin, end - begin));
		begin = end + 1;
	}
	return values;
}
static std::string Link(const std::filesystem::path& path)
{
	std::error_code error;
	const auto target = std::filesystem::read_symlink(path, error);
	return error ? std::string{} : target.string();
}
class ProbeSocket
{
  public:
	ProbeSocket() : m_descriptor(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))
	{
		if (m_descriptor < 0)
			throw std::system_error(errno, std::generic_category(), "Cannot probe runtime endpoint");
	}
	~ProbeSocket()
	{
		::close(m_descriptor);
	}
	ProbeSocket(const ProbeSocket&) = delete;
	ProbeSocket& operator=(const ProbeSocket&) = delete;
	int Get() const noexcept
	{
		return m_descriptor;
	}

  private:
	int m_descriptor;
};
static bool Answers(const std::string& endpoint, std::chrono::milliseconds timeout, std::uint64_t& process)
{
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (endpoint.size() >= sizeof(address.sun_path) || endpoint.find('\0') != std::string::npos)
		return false;
	std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
	ProbeSocket socket;
	const auto descriptor = socket.Get();
	bool alive = ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
	// A full backlog still means a listening runtime.
	if (!alive && errno == EAGAIN)
		alive = true;
	else if (!alive && errno == EINPROGRESS)
	{
		const auto deadline = std::chrono::steady_clock::now() +
							  std::clamp(timeout, std::chrono::milliseconds{0}, std::chrono::milliseconds{2000});
		pollfd wait{descriptor, POLLOUT, 0};
		int ready;
		do
		{
			const auto remaining =
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			ready = ::poll(&wait, 1, static_cast<int>(std::max<std::int64_t>(0, remaining.count())));
		} while (ready < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);
		int error = 0;
		socklen_t size = sizeof(error);
		alive = ready == 1 && ::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
	}
	if (alive)
	{
		ucred peer{};
		socklen_t size = sizeof(peer);
		if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.pid > 0)
			process = static_cast<std::uint64_t>(peer.pid);
	}
	return alive;
}
static std::optional<std::chrono::system_clock::time_point> ProcessStart(std::uint64_t process)
{
	const auto stat = ReadFile(std::filesystem::path("/proc") / std::to_string(process) / "stat");
	if (!stat)
		return {};
	const auto name = stat->rfind(')');
	if (name == std::string::npos)
		return {};
	// Field 22, counted from the state field (3) after the parenthesised name.
	std::istringstream fields(stat->substr(name + 1));
	std::string field;
	for (int index = 3; index < 22 && fields >> field; ++index)
	{
	}
	std::uint64_t ticks = 0;
	if (!(fields >> ticks))
		return {};
	const auto systemStat = ReadFile("/proc/stat");
	if (!systemStat)
		return {};
	std::istringstream system(*systemStat);
	for (std::string key; system >> key;)
		if (key == "btime")
		{
			std::int64_t boot = 0;
			const long rate = ::sysconf(_SC_CLK_TCK);
			if (!(system >> boot) || boot < 0 || rate <= 0)
				return {};
			using Duration = std::chrono::system_clock::duration;
			const std::chrono::duration<long double, Duration::period> elapsed =
				std::chrono::duration<long double>(boot + static_cast<long double>(ticks) / rate);
			if (elapsed.count() >= static_cast<long double>(Duration::max().count()))
				return {};
			return std::chrono::system_clock::time_point(Duration(static_cast<Duration::rep>(elapsed.count())));
		}
	return {};
}
static std::string ProcessName(const std::filesystem::path& base)
{
	if (const auto arguments = ReadFile(base / "cmdline"); arguments && !arguments->empty())
	{
		const auto first = arguments->substr(0, arguments->find('\0'));
		if (!first.empty())
			return std::filesystem::path(first).filename().string();
	}
	auto name = ReadFile(base / "comm").value_or("");
	while (!name.empty() && (name.back() == '\n' || name.back() == '\0'))
		name.pop_back();
	return name;
}
std::optional<RunningApp> ProbeRunningApp(const std::string& endpoint, std::chrono::milliseconds timeout)
{
	RunningApp app;
	if (!Answers(endpoint, timeout, app.process))
		return {};
	if (app.process)
	{
		const auto base = std::filesystem::path("/proc") / std::to_string(app.process);
		app.name = ProcessName(base);
		app.executable = Link(base / "exe");
		app.started = ProcessStart(app.process);
	}
	return app;
}
std::optional<LaunchProfile> ReadProcessLaunch(std::uint64_t process)
{
	if (!process || process > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
		return {};
	const auto base = std::filesystem::path("/proc") / std::to_string(process);
	const auto commandLine = ReadFile(base / "cmdline");
	const auto environment = ReadFile(base / "environ");
	if (!commandLine || !environment)
		return {};
	const auto arguments = NulSeparated(*commandLine, 257);
	const auto variables = NulSeparated(*environment, MaxLaunchEnvironment + 1);
	if (!arguments || arguments->empty() || !variables)
		return {};
	LaunchProfile profile;
	std::error_code error;
	const std::filesystem::path first = arguments->front();
	profile.executable =
		first.is_absolute() && std::filesystem::is_regular_file(first, error) ? first.string() : Link(base / "exe");
	profile.name = first.filename().string();
	profile.arguments.assign(arguments->begin() + 1, arguments->end());
	profile.workingDirectory = Link(base / "cwd");
	bool runtimeSeen = false;
	for (const auto& entry : *variables)
	{
		const auto equals = entry.find('=');
		if (equals == std::string::npos || equals == 0)
			return {};
		auto key = entry.substr(0, equals);
		auto value = entry.substr(equals + 1);
		if (key == "XR_RUNTIME_JSON")
		{
			if (runtimeSeen)
				return {};
			runtimeSeen = true;
			profile.runtimeManifest = value;
			if (!value.empty())
				continue;
		}
		profile.environment.emplace_back(std::move(key), std::move(value));
	}
	return profile;
}
} // namespace simxr::app
