#include "Pch.h"
#include "platform/linux/RuntimeFiles.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace simxr
{
static constexpr std::size_t s_maxLogMessages = 64;

class RuntimeFileDescriptor
{
public:
	explicit RuntimeFileDescriptor(int descriptor) : m_descriptor(descriptor)
	{
		if (descriptor < 0)
			throw std::system_error(errno, std::generic_category(), "Open runtime file descriptor");
	}

	~RuntimeFileDescriptor()
	{
		if (m_descriptor >= 0)
			close(m_descriptor);
	}

	RuntimeFileDescriptor(const RuntimeFileDescriptor&) = delete;
	RuntimeFileDescriptor& operator=(const RuntimeFileDescriptor&) = delete;

	int Get() const
	{
		return m_descriptor;
	}

	void Close()
	{
		const auto descriptor = std::exchange(m_descriptor, -1);
		if (descriptor >= 0 && close(descriptor) != 0)
			throw std::system_error(errno, std::generic_category(), "Close runtime file");
	}

private:
	int m_descriptor;
};

static std::optional<std::string> ReadRegularFile(const std::filesystem::path& path, std::size_t limit)
{
	const auto descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (descriptor < 0 && errno == ENOENT)
		return {};
	RuntimeFileDescriptor file(descriptor);
	struct stat information{};
	if (fstat(file.Get(), &information) != 0)
		throw std::system_error(errno, std::generic_category(), "Inspect startup sequence");
	if (!S_ISREG(information.st_mode) || information.st_size < 0 ||
		static_cast<std::uint64_t>(information.st_size) > limit)
		throw std::invalid_argument("Startup sequence requires a bounded regular file");
	std::string text(static_cast<std::size_t>(information.st_size), '\0');
	std::size_t offset = 0;
	while (offset < text.size())
	{
		const auto count = read(file.Get(), text.data() + offset, text.size() - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			throw std::system_error(errno, std::generic_category(), "Read startup sequence");
		if (count == 0)
			throw std::runtime_error("Startup sequence changed while being read");
		offset += static_cast<std::size_t>(count);
	}
	file.Close();
	return text;
}

static void WriteBytes(int descriptor, std::span<const std::byte> bytes)
{
	while (!bytes.empty())
	{
		const auto count = write(descriptor, bytes.data(), bytes.size());
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Write runtime log");
		bytes = bytes.subspan(static_cast<std::size_t>(count));
	}
}

std::shared_ptr<const PoseSequence> LoadStartupSequence(const std::filesystem::path& path)
{
	if (path.empty())
		return {};
	const auto text = ReadRegularFile(path, 16 * 1024 * 1024);
	if (!text)
		throw std::runtime_error("Startup sequence does not exist: " + path.string());
	return std::make_shared<const PoseSequence>(PoseSequence::Parse(*text, false));
}

RuntimeLog::RuntimeLog(std::filesystem::path directory) : m_directory(std::move(directory))
{
	std::filesystem::create_directories(m_directory);
	m_logs.reserve(s_maxLogMessages);
	m_worker = std::thread([this] { Run(); });
}

RuntimeLog::~RuntimeLog()
{
	Close();
}

bool RuntimeLog::Log(std::string message)
{
	if (message.size() > 4096)
		message.resize(4096);
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		if (m_logs.size() == s_maxLogMessages)
		{
			++m_status.droppedLogs;
			return false;
		}
		m_logs.push_back(std::move(message));
	}
	m_changed.notify_all();
	return true;
}

RuntimeLogStatus RuntimeLog::Status() const
{
	std::lock_guard lock(m_mutex);
	return m_status;
}

void RuntimeLog::Close()
{
	std::lock_guard closeLock(m_closeMutex);
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

void RuntimeLog::RecordError(std::string_view detail) noexcept
{
	try
	{
		std::lock_guard lock(m_mutex);
		++m_status.errors;
		m_status.detail.assign(detail);
	}
	catch (...)
	{}
}

void RuntimeLog::AppendLog(const std::vector<std::string>& messages)
{
	if (messages.empty())
		return;
	const auto path = m_directory / ("openxr_simulator." + std::to_string(getpid()) + ".log");
	RuntimeFileDescriptor file(
		open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644));
	struct stat information{};
	if (fstat(file.Get(), &information) != 0 || !S_ISREG(information.st_mode))
		throw std::runtime_error("Runtime log requires a regular file");
	for (const auto& message : messages)
	{
		const auto line = "[SimXR Linux] " + message + '\n';
		WriteBytes(file.Get(), std::as_bytes(std::span(line.data(), line.size())));
	}
	file.Close();
}

void RuntimeLog::Run() noexcept
{
	try
	{
		std::vector<std::string> logs;
		logs.reserve(s_maxLogMessages);
		for (;;)
		{
			bool stopping;
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait(lock, [&] { return m_stopping || !m_logs.empty(); });
				logs.swap(m_logs);
				stopping = m_stopping;
			}
			try
			{
				AppendLog(logs);
			}
			catch (const std::exception& error)
			{
				RecordError(error.what());
			}
			logs.clear();
			if (stopping)
				break;
		}
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
	}
	catch (...)
	{
		RecordError("Unexpected runtime log failure");
	}
	std::lock_guard lock(m_mutex);
	m_stopping = true;
	m_status.closed = true;
	m_changed.notify_all();
}
}
