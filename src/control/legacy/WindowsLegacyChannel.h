#pragma once

#include "control/legacy/WindowsLegacyCommands.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mcp
{
enum class WindowsCommandFile;
struct BitmapOutput
{
	std::string filename;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> bgra;
};
struct ImageOutput
{
	std::vector<BitmapOutput> images;
	std::string statusFilename;
	std::string status;
	std::string manifest;
	bool resetBurst = false;
};
enum class WindowsDocument
{
	RuntimeStatus,
	ProjectionLog,
	CommandAck
};
struct WindowsChannelStatus
{
	bool closed = false;
	std::uint64_t errors = 0;
	std::uint64_t droppedLogs = 0;
	std::size_t activeImages = 0;
	std::size_t activeBytes = 0;
	std::string detail;
};

// Owns filesystem activity and all accepted CPU output bytes through completion.
// Frame callers only exchange typed values with short, non-I/O locks.
class WindowsLegacyChannel
{
  public:
	explicit WindowsLegacyChannel(std::filesystem::path directory);
	~WindowsLegacyChannel();
	WindowsInputCommands TakeCommands();
	WindowsOutputRequests TakeRequests();
	std::optional<std::string> TakeWrittenScreenshot();
	bool Publish(WindowsDocument kind, std::string document);
	bool PublishImages(ImageOutput& output);
	bool DrainImages(std::chrono::milliseconds timeout);
	bool WaitForImageCapacity(std::size_t bytes);
	bool Log(std::string message);
	void Acknowledge(std::string_view command, bool success);
	WindowsChannelStatus Status() const;
	void Close();

  private:
	void Run() noexcept;
	void Scan();
	void ReadCommand(WindowsCommandFile command, std::string_view filename);
	void Stage(WindowsCommandFile command, const std::string& document);
	void WriteImages(const ImageOutput& output);
	void RecordError(std::string_view error) noexcept;
	std::filesystem::path m_directory;
	mutable std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::condition_variable m_changed;
	WindowsInputCommands m_commands;
	WindowsOutputRequests m_requests;
	std::array<std::string, 3> m_documents;
	std::array<bool, 3> m_documentReady{};
	std::vector<ImageOutput> m_images;
	std::vector<std::string> m_logs;
	WindowsChannelStatus m_status;
	std::optional<std::string> m_writtenScreenshot;
	bool m_stopping = false;
	std::thread m_worker;
};
} // namespace mcp
