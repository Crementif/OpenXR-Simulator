#include "app/mcp/McpPch.h"
#include "app/mcp/McpInstance.h"
#include "app/mcp/McpCredentials.h"
#include "app/platform/FileStorage.h"
#include "app/services/Utf8Path.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace simxr::app
{
using Json = nlohmann::json;
static constexpr int s_childLockDescriptor = 197;

class McpDescriptor
{
  public:
	explicit McpDescriptor(int descriptor = -1) : m_descriptor(descriptor) {}
	~McpDescriptor() { if (m_descriptor >= 0) ::close(m_descriptor); }
	McpDescriptor(const McpDescriptor&) = delete;
	McpDescriptor& operator=(const McpDescriptor&) = delete;
	McpDescriptor(McpDescriptor&& other) noexcept : m_descriptor(other.Release()) {}
	McpDescriptor& operator=(McpDescriptor&& other) noexcept
	{
		if (this != &other)
		{
			if (m_descriptor >= 0)
				::close(m_descriptor);
			m_descriptor = other.Release();
		}
		return *this;
	}
	int Get() const { return m_descriptor; }
	int Release() { return std::exchange(m_descriptor, -1); }

  private:
	int m_descriptor;
};

static std::runtime_error SystemError(const char* operation, int error = errno)
{
	return std::runtime_error(std::string(operation) + ": " + std::strerror(error));
}

static void CheckPrivateDirectory(const std::filesystem::path& path)
{
	struct stat info{};
	if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
		(info.st_mode & 0077))
		throw std::runtime_error("MCP instance directory must be owned by this user and inaccessible to other users");
}

static std::filesystem::path PrivateDirectory()
{
	std::filesystem::path directory;
	if (const auto* override = std::getenv("SIMXR_MCP_DISCOVERY_DIR"); override && *override)
	{
		directory = std::filesystem::path(override);
		if (!directory.is_absolute())
			throw std::runtime_error("SIMXR_MCP_DISCOVERY_DIR must be absolute");
	}
	else if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
	{
		const std::filesystem::path parent(runtime);
		if (!parent.is_absolute())
			throw std::runtime_error("XDG_RUNTIME_DIR must be absolute for MCP discovery");
		CheckPrivateDirectory(parent);
		directory = parent / "openxr-simulator-mcp";
	}
	else
		directory = std::filesystem::path("/tmp") / ("openxr-simulator-mcp-" + std::to_string(::geteuid()));
	if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
		throw SystemError("Cannot create MCP instance directory");
	CheckPrivateDirectory(directory);
	return directory;
}

static std::string InstanceKey(const std::filesystem::path& executable)
{
	const auto text = PathText(executable);
	std::uint64_t first = 14695981039346656037ull, second = 7809847782465536322ull;
	for (const auto byte : text)
	{
		first = (first ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
		second = (second ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string key;
	for (const auto value : {first, second})
		for (int shift = 60; shift >= 0; shift -= 4)
			key.push_back(hex[(value >> shift) & 15]);
	return key;
}

static McpDescriptor OpenLock(const std::filesystem::path& directory, const std::string& key)
{
	McpDescriptor parent(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	if (parent.Get() < 0)
		throw SystemError("Cannot open MCP instance directory");
	struct stat parentInfo{};
	if (::fstat(parent.Get(), &parentInfo) != 0 || parentInfo.st_uid != ::geteuid() ||
		!S_ISDIR(parentInfo.st_mode) || (parentInfo.st_mode & 0077))
		throw std::runtime_error("MCP instance directory is not private");
	const auto name = key + ".lock";
	const int descriptor = ::openat(parent.Get(), name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (descriptor < 0)
		throw SystemError("Cannot open MCP instance lock");
	McpDescriptor lock(descriptor);
	struct stat info{};
	if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
		(info.st_mode & 0077) || info.st_nlink != 1)
		throw std::runtime_error("MCP instance lock is not private");
	return McpDescriptor(lock.Release());
}

std::filesystem::path McpOwnExecutable()
{
	std::array<char, 4096> path{};
	const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
	if (count <= 0 || static_cast<std::size_t>(count) >= path.size())
		throw SystemError("Cannot resolve the running executable");
	return std::filesystem::weakly_canonical(std::filesystem::path(std::string(path.data(), count)));
}

McpInstanceDiscovery::McpInstanceDiscovery(std::filesystem::path executable, bool windowless)
	: m_executable(std::filesystem::weakly_canonical(std::move(executable))),
	  m_directory(PrivateDirectory()), m_key(InstanceKey(m_executable)), m_windowless(windowless)
{
	if (!m_executable.is_absolute())
		throw std::invalid_argument("MCP app path must be absolute");
}

McpInstanceDiscovery::~McpInstanceDiscovery()
{
	if (m_child > 0)
		::waitpid(m_child, nullptr, WNOHANG);
}

void McpInstanceDiscovery::StartIfNeeded()
{
	auto lock = OpenLock(m_directory, m_key);
	if (::flock(lock.Get(), LOCK_EX | LOCK_NB) != 0)
	{
		if (errno == EWOULDBLOCK || errno == EAGAIN)
			return;
		throw SystemError("Cannot claim MCP app startup");
	}
	if (!std::filesystem::is_regular_file(m_executable) || ::access(m_executable.c_str(), X_OK) != 0)
		throw std::runtime_error("Selected MCP app executable is missing or not executable");
	std::error_code removal;
	std::filesystem::remove(m_directory / (m_key + ".json"), removal);
	if (removal)
		throw std::runtime_error("Cannot remove a stale MCP discovery record: " + removal.message());
	const auto token = NewMcpIdentifier();
	std::map<std::string, std::string> values;
	for (auto entry = environ; entry && *entry; ++entry)
	{
		const std::string value(*entry);
		if (const auto equals = value.find('='); equals != std::string::npos)
			values[value.substr(0, equals)] = value.substr(equals + 1);
	}
	values["SIMXR_MCP_TOKEN"] = token;
	std::vector<std::string> environment;
	for (const auto& [name, value] : values)
		environment.push_back(name + "=" + value);
	std::vector<char*> envp;
	for (auto& value : environment)
		envp.push_back(value.data());
	envp.push_back(nullptr);
	auto executable = m_executable.native();
	std::string option = "--mcp-bootstrap-fd", number = std::to_string(s_childLockDescriptor);
	std::string mode = "--windowless";
	std::array<char*, 5> arguments{executable.data(), option.data(), number.data(),
									   m_windowless ? mode.data() : nullptr, nullptr};
	posix_spawn_file_actions_t actions;
	int error = ::posix_spawn_file_actions_init(&actions);
	if (error)
		throw SystemError("Cannot prepare MCP app startup", error);
	posix_spawnattr_t attributes;
	error = ::posix_spawnattr_init(&attributes);
	if (error)
	{
		::posix_spawn_file_actions_destroy(&actions);
		throw SystemError("Cannot prepare MCP app session", error);
	}
	try
	{
		if ((error = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID)))
			throw SystemError("Cannot separate the MCP app session", error);
		McpDescriptor duplicate;
		int source = lock.Get();
		if (source == s_childLockDescriptor)
		{
			const int copy = ::fcntl(source, F_DUPFD_CLOEXEC, s_childLockDescriptor + 1);
			if (copy < 0)
				throw SystemError("Cannot retain MCP startup lock");
			duplicate = McpDescriptor(copy);
			source = copy;
		}
		if ((error = ::posix_spawn_file_actions_adddup2(&actions, source, s_childLockDescriptor)) ||
			(error = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) ||
			(error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0)) ||
			(error = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0)))
			throw SystemError("Cannot configure MCP app startup", error);
		pid_t process = 0;
		error = ::posix_spawn(&process, executable.c_str(), &actions, &attributes, arguments.data(), envp.data());
		if (error)
			throw SystemError("Cannot start the selected MCP app", error);
		m_child = process;
	}
	catch (...)
	{
		::posix_spawnattr_destroy(&attributes);
		::posix_spawn_file_actions_destroy(&actions);
		throw;
	}
	::posix_spawnattr_destroy(&attributes);
	::posix_spawn_file_actions_destroy(&actions);
}

std::optional<McpEndpoint> McpInstanceDiscovery::Read()
{
	if (m_child > 0)
	{
		int status = 0;
		if (::waitpid(m_child, &status, WNOHANG) == m_child)
		{
			if (!m_observed)
			{
				m_startupError = "The selected simulator exited before MCP became ready";
				if (WIFEXITED(status))
					m_startupError += " (exit code " + std::to_string(WEXITSTATUS(status)) + ")";
				m_startupError += "; start it manually to inspect the error";
			}
			m_child = -1;
		}
	}
	auto lock = OpenLock(m_directory, m_key);
	if (::flock(lock.Get(), LOCK_EX | LOCK_NB) == 0)
		return {};
	if (errno != EWOULDBLOCK && errno != EAGAIN)
		throw SystemError("Cannot check MCP app ownership");
	const auto path = m_directory / (m_key + ".json");
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > 16384)
		return {};
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return {};
	const auto value = Json::parse(stream, nullptr, false);
	if (value.is_discarded() || !value.is_object())
		return {};
	const auto protocol = value.find("connector_protocol");
	if (protocol != value.end() && (!protocol->is_number_unsigned() ||
		protocol->get<std::uint64_t>() != McpConnectorProtocolVersion))
	{
		m_startupError = "The selected app uses an unsupported MCP connector protocol; "
			"install a compatible connector and reconnect this MCP entry";
		return {};
	}
	if (value.value("executable", std::string{}) != PathText(m_executable) ||
		!value.contains("port") || !value["port"].is_number_unsigned() || !value.contains("token") ||
		!value["token"].is_string() || !value.contains("device") || !value["device"].is_string() ||
		!value.contains("inode") || !value["inode"].is_string())
		return {};
	struct stat selected{};
	if (::stat(m_executable.c_str(), &selected) != 0 ||
		value["device"] != std::to_string(static_cast<std::uintmax_t>(selected.st_dev)) ||
		value["inode"] != std::to_string(static_cast<std::uintmax_t>(selected.st_ino)))
	{
		m_startupError = "The selected app executable changed while an older simulator instance is running; "
			"close that instance and start the rebuilt app";
		return {};
	}
	const auto port = value["port"].get<std::uint64_t>();
	const auto token = value["token"].get<std::string>();
	if (!port || port > 65535 || token.size() != 64 || !std::ranges::all_of(token, [](unsigned char byte)
		{ return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'); }))
		return {};
	m_observed = true;
	m_startupError.clear();
	return McpEndpoint{static_cast<std::uint16_t>(port), token};
}

const std::string& McpInstanceDiscovery::StartupError() const noexcept
{
	return m_startupError;
}

McpInstanceRegistration::McpInstanceRegistration(std::intptr_t inheritedHandle)
	: m_executable(McpOwnExecutable()), m_directory(PrivateDirectory()), m_key(InstanceKey(m_executable))
{
	if (inheritedHandle > std::numeric_limits<int>::max())
		throw std::invalid_argument("Invalid MCP bootstrap descriptor");
	const auto inheritedDescriptor = static_cast<int>(inheritedHandle);
	auto expected = OpenLock(m_directory, m_key);
	McpDescriptor inherited(inheritedDescriptor);
	const int descriptor = inheritedDescriptor >= 0 ? inherited.Get() : expected.Get();
	if (inheritedDescriptor >= 0)
	{
		struct stat actual{}, target{};
		if (::fstat(inherited.Get(), &actual) || ::fstat(expected.Get(), &target) ||
			actual.st_dev != target.st_dev || actual.st_ino != target.st_ino)
			throw std::runtime_error("Invalid MCP bootstrap lock");
	}
	if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0)
		throw std::runtime_error("An MCP-enabled simulator for this executable is already running");
	if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
		throw SystemError("Cannot protect MCP bootstrap lock");
	m_lock = inheritedDescriptor >= 0 ? inherited.Release() : expected.Release();
}

McpInstanceRegistration::~McpInstanceRegistration()
{
	if (m_lock >= 0)
	{
		std::error_code ignored;
		std::filesystem::remove(m_directory / (m_key + ".json"), ignored);
		::close(m_lock);
	}
}

void McpInstanceRegistration::Publish(const McpEndpoint& endpoint)
{
	struct stat executable{};
	if (::stat("/proc/self/exe", &executable) != 0)
		throw SystemError("Cannot identify the running MCP app");
	const auto destination = m_directory / (m_key + ".json");
	const auto temporary = CreatePrivateTemporaryFile(destination);
	try
	{
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream)
			throw std::runtime_error("Cannot open MCP discovery record");
		stream << Json({{"connector_protocol", McpConnectorProtocolVersion},
						{"executable", PathText(m_executable)}, {"port", endpoint.port},
						{"token", endpoint.token}, {"process_id", ::getpid()},
						{"device", std::to_string(static_cast<std::uintmax_t>(executable.st_dev))},
						{"inode", std::to_string(static_cast<std::uintmax_t>(executable.st_ino))}}).dump();
		stream.close();
		if (!stream)
			throw std::runtime_error("Cannot write MCP discovery record");
		CommitTemporaryFile(temporary, destination);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}
} // namespace simxr::app
