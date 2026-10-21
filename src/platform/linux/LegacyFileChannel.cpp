#include "control/legacy/LegacyFileChannel.h"
#include "json.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <poll.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace simxr
{
namespace
{
enum class CommandIndex : std::size_t
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
constexpr std::array CommandNames{
	"head_pose_command.json",		"ipd_command.json",		 "fov_command.json",		"controller_pose_command.json",
	"controller_hold_command.json", "sequence_command.json", "screenshot_request.json", "projection_log_dump_request"};
constexpr std::array OutputNames{"runtime_status.json", "frame_info.json", "projection_log.json"};
constexpr std::size_t MaxCommandBytes = 64 * 1024;
constexpr std::size_t MaxSequenceBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxDocumentBytes = 1024 * 1024;
constexpr std::size_t MaxScreenshotBytes = 512 * 1024 * 1024;
constexpr std::size_t MaxScreenshots = 4;
constexpr std::size_t MaxLogMessages = 64;

class FileDescriptor
{
  public:
	explicit FileDescriptor(int descriptor) : m_descriptor(descriptor)
	{
		if (descriptor < 0)
			throw std::system_error(errno, std::generic_category(), "Open legacy file descriptor");
	}
	~FileDescriptor()
	{
		if (m_descriptor >= 0)
			close(m_descriptor);
	}
	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;
	int Get() const
	{
		return m_descriptor;
	}
	void Close()
	{
		const auto descriptor = std::exchange(m_descriptor, -1);
		if (descriptor >= 0 && close(descriptor) != 0)
			throw std::system_error(errno, std::generic_category(), "Close legacy output");
	}

  private:
	int m_descriptor;
};

struct FileIdentity
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

std::optional<std::string> ReadRegularFile(const std::filesystem::path& path, std::size_t limit,
										   FileIdentity* identity = nullptr)
{
	const auto descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (descriptor < 0 && errno == ENOENT)
		return {};
	FileDescriptor file(descriptor);
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
	return text;
}

void WriteBytes(int descriptor, std::span<const std::byte> bytes)
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

class AtomicFile
{
  public:
	explicit AtomicFile(std::filesystem::path destination)
		: m_destination(std::move(destination)),
		  m_temporary(m_destination.string() + ".tmp." + std::to_string(getpid())),
		  m_file(open(m_temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0644))
	{
	}
	~AtomicFile()
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
	FileDescriptor m_file;
	bool m_committed = false;
};

void WriteAtomic(const std::filesystem::path& path, std::span<const std::byte> first,
				 std::span<const std::byte> second = {})
{
	AtomicFile file(path);
	file.Write(first);
	file.Write(second);
	file.Commit();
}
} // namespace

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
	std::array<bool, CommandNames.size()> Wait()
	{
		std::array<bool, CommandNames.size()> changed{};
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
					for (std::size_t index = 0; index < CommandNames.size(); ++index)
						if (name == CommandNames[index])
							changed[index] = true;
				}
				offset += sizeof(inotify_event) + event.len;
			}
		}
		return changed;
	}

  private:
	FileDescriptor m_events;
	FileDescriptor m_wake;
};

LegacyFileChannel::LegacyFileChannel(std::filesystem::path directory, std::filesystem::path initialSequence)
	: m_directory(std::move(directory))
{
	std::filesystem::create_directories(m_directory);
	m_notifications = std::make_unique<Notifications>(m_directory);
	m_screenshots.reserve(MaxScreenshots);
	m_logs.reserve(MaxLogMessages);
	if (!initialSequence.empty())
	{
		const auto text = ReadRegularFile(initialSequence, MaxSequenceBytes);
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
	if (index >= m_outputs.size() || document.size() > MaxDocumentBytes)
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
	if (!pixels || pixels > MaxScreenshotBytes / 4 || pixels * 4 != screenshot.bgra.size() ||
		screenshot.status.size() > MaxDocumentBytes)
		throw std::invalid_argument("Invalid legacy screenshot dimensions or byte count");
	const auto expected = pixels * 4;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping || m_screenshotCount == MaxScreenshots || expected > MaxScreenshotBytes - m_screenshotBytes)
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
		if (m_logs.size() == MaxLogMessages)
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
	const auto command = static_cast<CommandIndex>(index);
	const auto path = m_directory / CommandNames[index];
	const auto claimed = path.string() + ".pending." + std::to_string(getpid());
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
		FileIdentity identity;
		const auto text = ReadRegularFile(claimed, MaxCommandBytes, &identity);
		if (!text)
			return;
		if (index < m_commands.input.size())
		{
			std::optional<std::string> document(*text);
			std::lock_guard lock(m_mutex);
			m_commands.input[index].swap(document);
		}
		else if (command == CommandIndex::Sequence)
		{
			const json::Object document(text->c_str());
			const auto sequence = ReadRegularFile(document.string("path"), MaxSequenceBytes);
			std::istringstream stream(sequence.value_or(""));
			std::optional<LegacyInput::Sequence> prepared(
				LegacyInput::ReadSequence(stream, document.boolean("loop", false)));
			std::lock_guard lock(m_mutex);
			m_commands.sequence.swap(prepared);
		}
		else if (command == CommandIndex::Capture)
		{
			const json::Object document(text->c_str());
			std::optional<LegacyCaptureRequest> capture(
				LegacyCaptureRequest{document.string("eye", "both"), document.string("layer", "projection")});
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
		WriteFile((std::string(CommandNames[index]) + ".received").c_str(), identity.Receipt());
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
	FileDescriptor file(open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644));
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
		std::array<bool, CommandNames.size()> changed;
		changed.fill(true);
		std::vector<LegacyScreenshot> screenshots;
		std::vector<std::string> logs;
		screenshots.reserve(MaxScreenshots);
		logs.reserve(MaxLogMessages);
		for (;;)
		{
			for (std::size_t index = 0; index < changed.size(); ++index)
				if (changed[index])
					ReadCommand(index);
			std::array<std::optional<std::string>, OutputNames.size()> outputs;
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
						WriteFile(OutputNames[index], *outputs[index]);
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
