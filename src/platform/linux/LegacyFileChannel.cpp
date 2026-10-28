#include "control/legacy/LegacyFileChannel.h"
#include "Pch.h"
#include "json.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace simxr
{
enum class LegacyCommandIndex : std::size_t
{
	HeadPose,
	Ipd,
	Fov,
	ControllerPose,
	ControllerHold,
	Sequence,
	Capture,
	ProjectionDump
};
static constexpr std::array s_commandNames{
	"head_pose_command.json",		"ipd_command.json",		 "fov_command.json",		"controller_pose_command.json",
	"controller_hold_command.json", "sequence_command.json", "screenshot_request.json", "projection_log_dump_request"};
static constexpr std::array s_outputNames{"runtime_status.json", "frame_info.json", "projection_log.json"};
static constexpr std::size_t s_maxCommandBytes = 64 * 1024;
static constexpr std::size_t s_maxSequenceBytes = 16 * 1024 * 1024;
static constexpr std::size_t s_maxDocumentBytes = 1024 * 1024;
static constexpr std::size_t s_maxScreenshotBytes = 512 * 1024 * 1024;
static constexpr std::size_t s_maxScreenshots = 4;
static constexpr std::size_t s_maxLogMessages = 64;

class LegacyFileDescriptor
{
  public:
	explicit LegacyFileDescriptor(int descriptor) : m_descriptor(descriptor)
	{
		if (descriptor < 0)
			throw std::system_error(errno, std::generic_category(), "Open legacy file descriptor");
	}
	~LegacyFileDescriptor()
	{
		if (m_descriptor >= 0)
			close(m_descriptor);
	}
	LegacyFileDescriptor(const LegacyFileDescriptor&) = delete;
	LegacyFileDescriptor& operator=(const LegacyFileDescriptor&) = delete;
	int Get() const
	{
		return m_descriptor;
	}
	void Close()
	{
		const auto descriptor = std::exchange(m_descriptor, -1);
		if (descriptor >= 0 && close(descriptor) != 0)
			throw std::system_error(errno, std::generic_category(), "Close legacy file");
	}

  private:
	int m_descriptor;
};

struct LegacyFileIdentity
{
	std::uint64_t device = 0;
	std::uint64_t inode = 0;
	std::int64_t modifiedSeconds = 0;
	std::int64_t modifiedNanos = 0;
	std::string Receipt() const
	{
		return std::to_string(device) + ":" + std::to_string(inode) + ":" + std::to_string(modifiedSeconds) + ":" +
			   std::to_string(modifiedNanos);
	}
};

static std::optional<std::string> ReadRegularFile(const std::filesystem::path& path, std::size_t limit,
												  LegacyFileIdentity* identity = nullptr)
{
	const auto descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (descriptor < 0 && errno == ENOENT)
		return {};
	LegacyFileDescriptor file(descriptor);
	struct stat information{};
	if (fstat(file.Get(), &information) != 0)
		throw std::system_error(errno, std::generic_category(), "Inspect legacy command");
	if (!S_ISREG(information.st_mode) || information.st_size < 0 ||
		static_cast<std::uint64_t>(information.st_size) > limit)
		throw std::invalid_argument("Legacy command requires a bounded regular file");
	if (identity)
		*identity = {static_cast<std::uint64_t>(information.st_dev), static_cast<std::uint64_t>(information.st_ino),
					 information.st_mtim.tv_sec, information.st_mtim.tv_nsec};
	std::string text(static_cast<std::size_t>(information.st_size), '\0');
	std::size_t offset = 0;
	while (offset < text.size())
	{
		const auto count = read(file.Get(), text.data() + offset, text.size() - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			throw std::system_error(errno, std::generic_category(), "Read legacy command");
		if (count == 0)
			throw std::runtime_error("Legacy command changed while being read");
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
			throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Write legacy output");
		bytes = bytes.subspan(static_cast<std::size_t>(count));
	}
}

class LegacyAtomicFile
{
  public:
	explicit LegacyAtomicFile(std::filesystem::path destination)
		: m_destination(std::move(destination)),
		  m_temporary(std::filesystem::path(m_destination).concat(".tmp." + std::to_string(getpid()))),
		  m_file(open(m_temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0644))
	{
	}
	~LegacyAtomicFile()
	{
		if (!m_committed)
			unlink(m_temporary.c_str());
	}
	void Write(std::span<const std::byte> bytes)
	{
		struct stat information{};
		if (fstat(m_file.Get(), &information) != 0 || !S_ISREG(information.st_mode))
			throw std::runtime_error("Legacy output requires a regular file");
		WriteBytes(m_file.Get(), bytes);
	}
	void Commit()
	{
		m_file.Close();
		std::filesystem::rename(m_temporary, m_destination);
		m_committed = true;
	}

  private:
	std::filesystem::path m_destination;
	std::filesystem::path m_temporary;
	LegacyFileDescriptor m_file;
	bool m_committed = false;
};

static void WriteAtomic(const std::filesystem::path& path, std::span<const std::byte> first,
						std::span<const std::byte> second = {})
{
	LegacyAtomicFile file(path);
	file.Write(first);
	file.Write(second);
	file.Commit();
}

class LegacyFileChannel::Notifications
{
  public:
	explicit Notifications(const std::filesystem::path& directory)
		: m_events(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)), m_wake(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
	{
		if (inotify_add_watch(m_events.Get(), directory.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0)
			throw std::system_error(errno, std::generic_category(), "Watch legacy command directory");
	}
	void Wake() noexcept
	{
		const std::uint64_t signal = 1;
		while (write(m_wake.Get(), &signal, sizeof(signal)) < 0 && errno == EINTR)
		{
		}
	}
	std::array<bool, s_commandNames.size()> Wait()
	{
		std::array<bool, s_commandNames.size()> changed{};
		std::array<pollfd, 2> descriptors{{{m_events.Get(), POLLIN, 0}, {m_wake.Get(), POLLIN, 0}}};
		int result;
		do
		{
			result = poll(descriptors.data(), descriptors.size(), -1);
		} while (result < 0 && errno == EINTR);
		if (result < 0)
			throw std::system_error(errno, std::generic_category(), "Wait for legacy files");
		if ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
			throw std::runtime_error("Legacy file notification channel closed");
		std::uint64_t signal;
		while (read(m_wake.Get(), &signal, sizeof(signal)) > 0)
		{
		}
		alignas(inotify_event) std::array<std::byte, 64 * 1024> buffer;
		for (unsigned batch = 0; batch < 8; ++batch)
		{
			const auto count = read(m_events.Get(), buffer.data(), buffer.size());
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0 && errno == EAGAIN)
				break;
			if (count <= 0)
				throw std::runtime_error("Read legacy file notifications");
			std::size_t offset = 0;
			while (offset < static_cast<std::size_t>(count))
			{
				const auto& event = *reinterpret_cast<const inotify_event*>(buffer.data() + offset);
				if (event.mask & IN_Q_OVERFLOW)
					changed.fill(true);
				else if (event.len && !(event.mask & IN_ISDIR))
				{
					const std::string_view name(event.name, strnlen(event.name, event.len));
					for (std::size_t index = 0; index < s_commandNames.size(); ++index)
						if (name == s_commandNames[index])
							changed[index] = true;
				}
				offset += sizeof(inotify_event) + event.len;
			}
		}
		return changed;
	}

  private:
	LegacyFileDescriptor m_events;
	LegacyFileDescriptor m_wake;
};

LegacyFileChannel::LegacyFileChannel(std::filesystem::path directory, std::filesystem::path initialSequence)
	: m_directory(std::move(directory))
{
	std::filesystem::create_directories(m_directory);
	m_notifications = std::make_unique<Notifications>(m_directory);
	m_screenshots.reserve(s_maxScreenshots);
	m_logs.reserve(s_maxLogMessages);
	if (!initialSequence.empty())
	{
		const auto text = ReadRegularFile(initialSequence, s_maxSequenceBytes);
		std::istringstream stream(text.value_or(""));
		m_commands.sequence = LegacyInput::ReadSequence(stream, false);
	}
	m_worker = std::thread(
		[this]
		{
			Run();
		});
}

LegacyFileChannel::~LegacyFileChannel()
{
	Close();
}

LegacyFileCommands LegacyFileChannel::TakeCommands()
{
	static_assert(std::is_nothrow_swappable_v<LegacyFileCommands>);
	LegacyFileCommands commands;
	{
		std::lock_guard lock(m_mutex);
		std::swap(commands, m_commands);
	}
	return commands;
}

LegacyFileRequests LegacyFileChannel::TakeRequests()
{
	static_assert(std::is_nothrow_swappable_v<LegacyFileRequests>);
	LegacyFileRequests requests;
	{
		std::lock_guard lock(m_mutex);
		std::swap(requests, m_requests);
	}
	return requests;
}

bool LegacyFileChannel::Publish(LegacyOutputFile file, std::string document)
{
	const auto index = static_cast<std::size_t>(file);
	if (index >= m_outputs.size() || document.size() > s_maxDocumentBytes)
		return false;
	std::optional<std::string> output(std::move(document));
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		m_outputs[index].swap(output);
	}
	m_notifications->Wake();
	return true;
}

bool LegacyFileChannel::SaveScreenshot(LegacyScreenshot& screenshot)
{
	const auto pixels = std::uint64_t{screenshot.width} * screenshot.height;
	if (!pixels || pixels > s_maxScreenshotBytes / 4 || pixels * 4 != screenshot.bgra.size() ||
		screenshot.status.size() > s_maxDocumentBytes)
		throw std::invalid_argument("Invalid legacy screenshot dimensions or byte count");
	const auto expected = pixels * 4;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping || m_screenshotCount == s_maxScreenshots || expected > s_maxScreenshotBytes - m_screenshotBytes)
			return false;
		m_screenshots.push_back(std::move(screenshot));
		++m_screenshotCount;
		m_screenshotBytes += expected;
	}
	m_notifications->Wake();
	return true;
}

bool LegacyFileChannel::Log(std::string message)
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
	m_notifications->Wake();
	return true;
}

bool LegacyFileChannel::DrainScreenshots(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(m_mutex);
	return m_changed.wait_for(lock, timeout,
							  [this]
							  {
								  return m_screenshotCount == 0 || m_status.closed;
							  }) &&
		   m_screenshotCount == 0;
}

LegacyFileStatus LegacyFileChannel::Status() const
{
	std::lock_guard lock(m_mutex);
	return m_status;
}

void LegacyFileChannel::Close()
{
	std::lock_guard closeLock(m_closeMutex);
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_notifications->Wake();
	if (m_worker.joinable())
		m_worker.join();
}

void LegacyFileChannel::RecordError(std::string_view detail) noexcept
{
	try
	{
		std::lock_guard lock(m_mutex);
		++m_status.errors;
		m_status.detail.assign(detail);
	}
	catch (...)
	{
	}
}

void LegacyFileChannel::ReadCommand(std::size_t index)
{
	const auto command = static_cast<LegacyCommandIndex>(index);
	const auto path = m_directory / s_commandNames[index];
	const auto claimed = std::filesystem::path(path).concat(".pending." + std::to_string(getpid()));
	bool staged = false;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return;
	}
	if (syscall(SYS_renameat2, AT_FDCWD, path.c_str(), AT_FDCWD, claimed.c_str(), RENAME_NOREPLACE) != 0)
	{
		if (errno != ENOENT)
			RecordError("Claim legacy command");
		return;
	}
	try
	{
		LegacyFileIdentity identity;
		const auto text = ReadRegularFile(claimed, s_maxCommandBytes, &identity);
		if (!text)
			return;
		if (index < m_commands.input.size())
		{
			std::optional<std::string> document(*text);
			std::lock_guard lock(m_mutex);
			m_commands.input[index].swap(document);
		}
		else if (command == LegacyCommandIndex::Sequence)
		{
			const json::Object document(text->c_str());
			const auto sequence = ReadRegularFile(document.String("path"), s_maxSequenceBytes);
			std::istringstream stream(sequence.value_or(""));
			std::optional<LegacyInput::Sequence> prepared(
				LegacyInput::ReadSequence(stream, document.Boolean("loop", false)));
			std::lock_guard lock(m_mutex);
			m_commands.sequence.swap(prepared);
		}
		else if (command == LegacyCommandIndex::Capture)
		{
			const json::Object document(text->c_str());
			std::optional<LegacyCaptureRequest> capture(
				LegacyCaptureRequest{document.String("eye", "both"), document.String("layer", "projection")});
			if ((capture->eye != "left" && capture->eye != "right" && capture->eye != "both") ||
				(capture->layer != "projection" && capture->layer != "quad" && capture->layer != "all"))
				throw std::invalid_argument("Invalid legacy capture eye or layer selection");
			std::lock_guard lock(m_mutex);
			m_requests.capture.swap(capture);
		}
		else
		{
			std::lock_guard lock(m_mutex);
			m_requests.projectionLog = true;
		}
		staged = true;
		WriteFile((std::string(s_commandNames[index]) + ".received").c_str(), identity.Receipt());
		std::filesystem::remove(claimed);
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
		if (!staged && link(claimed.c_str(), path.c_str()) != 0 && errno != EEXIST)
		{
			RecordError("Cannot restore legacy command; claimed file is preserved");
			return;
		}
		if (unlink(claimed.c_str()) != 0)
			RecordError("Cannot retire claimed legacy command file");
	}
}

void LegacyFileChannel::WriteFile(const char* filename, const std::string& document)
{
	WriteAtomic(m_directory / filename, std::as_bytes(std::span(document.data(), document.size())));
}

void LegacyFileChannel::WriteScreenshot(const LegacyScreenshot& screenshot)
{
	std::array<std::byte, 54> header{};
	const auto store = [&](std::size_t offset, std::uint32_t value)
	{
		for (unsigned byte = 0; byte < 4; ++byte)
			header[offset + byte] = static_cast<std::byte>(value >> (8 * byte));
	};
	header[0] = std::byte{'B'};
	header[1] = std::byte{'M'};
	store(2, static_cast<std::uint32_t>(header.size() + screenshot.bgra.size()));
	store(10, header.size());
	store(14, 40);
	store(18, screenshot.width);
	store(22, 0u - screenshot.height);
	header[26] = std::byte{1};
	header[28] = std::byte{32};
	store(34, static_cast<std::uint32_t>(screenshot.bgra.size()));
	WriteAtomic(m_directory / "screenshot.bmp", header, std::as_bytes(std::span(screenshot.bgra)));
	WriteFile("screenshot_status.json", screenshot.status);
}

void LegacyFileChannel::AppendLog(const std::vector<std::string>& messages)
{
	if (messages.empty())
		return;
	const auto path = m_directory / ("openxr_simulator." + std::to_string(getpid()) + ".log");
	LegacyFileDescriptor file(
		open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644));
	struct stat information{};
	if (fstat(file.Get(), &information) != 0 || !S_ISREG(information.st_mode))
		throw std::runtime_error("Legacy log requires a regular file");
	for (const auto& message : messages)
	{
		const auto line = "[SimXR Linux] " + message + '\n';
		WriteBytes(file.Get(), std::as_bytes(std::span(line.data(), line.size())));
	}
	file.Close();
}

void LegacyFileChannel::Run() noexcept
{
	try
	{
		std::array<bool, s_commandNames.size()> changed;
		changed.fill(true);
		std::vector<LegacyScreenshot> screenshots;
		std::vector<std::string> logs;
		screenshots.reserve(s_maxScreenshots);
		logs.reserve(s_maxLogMessages);
		for (;;)
		{
			for (std::size_t index = 0; index < changed.size(); ++index)
				if (changed[index])
					ReadCommand(index);
			std::array<std::optional<std::string>, s_outputNames.size()> outputs;
			bool stopping;
			{
				std::lock_guard lock(m_mutex);
				outputs.swap(m_outputs);
				screenshots.swap(m_screenshots);
				logs.swap(m_logs);
				stopping = m_stopping;
			}
			for (std::size_t index = 0; index < outputs.size(); ++index)
				if (outputs[index])
				{
					try
					{
						WriteFile(s_outputNames[index], *outputs[index]);
					}
					catch (const std::exception& error)
					{
						RecordError(error.what());
					}
				}
			for (auto& screenshot : screenshots)
			{
				try
				{
					WriteScreenshot(screenshot);
				}
				catch (const std::exception& error)
				{
					RecordError(error.what());
				}
				const auto bytes = screenshot.bgra.size();
				std::vector<std::uint8_t>{}.swap(screenshot.bgra);
				std::lock_guard lock(m_mutex);
				--m_screenshotCount;
				m_screenshotBytes -= bytes;
				m_changed.notify_all();
			}
			try
			{
				AppendLog(logs);
			}
			catch (const std::exception& error)
			{
				RecordError(error.what());
			}
			screenshots.clear();
			logs.clear();
			if (stopping)
				break;
			changed = m_notifications->Wait();
		}
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
	}
	catch (...)
	{
	}
	std::lock_guard lock(m_mutex);
	m_stopping = true;
	m_status.closed = true;
	m_changed.notify_all();
}
} // namespace simxr
