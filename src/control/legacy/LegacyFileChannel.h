#pragma once

#include "control/legacy/LegacyInput.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace simxr
{
struct LegacyCaptureRequest
{
	std::string eye = "both";
	std::string layer = "projection";
};

struct LegacyFileCommands
{
	std::array<std::optional<std::string>, 5> input;
	std::optional<LegacyInput::Sequence> sequence;
};

struct LegacyFileRequests
{
	std::optional<LegacyCaptureRequest> capture;
	bool projectionLog = false;
};

enum class LegacyOutputFile
{
	RuntimeStatus,
	FrameInfo,
	ProjectionLog
};

struct LegacyScreenshot
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> bgra;
	std::string status;
	LegacyCaptureRequest selection;
};

struct LegacyFileStatus
{
	bool closed = false;
	std::uint64_t errors = 0;
	std::uint64_t droppedLogs = 0;
	std::string detail;
};

// The GPU capture observer only exchanges CPU requests and completed images.
// Platform file workers retain accepted bytes and perform all filesystem I/O.
class LegacyCaptureChannel
{
  public:
	virtual ~LegacyCaptureChannel() = default;
	virtual LegacyFileRequests TakeRequests() = 0;
	virtual bool Publish(LegacyOutputFile file, std::string document) = 0;
	virtual bool SaveScreenshot(LegacyScreenshot& screenshot) = 0;
	virtual bool DrainScreenshots(std::chrono::milliseconds timeout) = 0;
	virtual bool Log(std::string message) = 0;
	virtual LegacyFileStatus Status() const = 0;
};

class LegacyFileChannel : public LegacyCaptureChannel
{
  public:
	explicit LegacyFileChannel(std::filesystem::path directory, std::filesystem::path initialSequence = {});
	~LegacyFileChannel() override;
	LegacyFileCommands TakeCommands();
	LegacyFileRequests TakeRequests() override;
	bool Publish(LegacyOutputFile file, std::string document) override;
	bool SaveScreenshot(LegacyScreenshot& screenshot) override;
	bool DrainScreenshots(std::chrono::milliseconds timeout) override;
	bool Log(std::string message) override;
	LegacyFileStatus Status() const override;
	void Close();

  private:
	class Notifications;
	void Run() noexcept;
	void ReadCommand(std::size_t index);
	void RecordError(std::string_view detail) noexcept;
	void WriteFile(const char* filename, const std::string& document);
	void WriteScreenshot(const LegacyScreenshot& screenshot);
	void AppendLog(const std::vector<std::string>& messages);
	std::filesystem::path m_directory;
	std::unique_ptr<Notifications> m_notifications;
	mutable std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::condition_variable m_changed;
	LegacyFileCommands m_commands;
	LegacyFileRequests m_requests;
	std::array<std::optional<std::string>, 3> m_outputs;
	std::vector<LegacyScreenshot> m_screenshots;
	std::vector<std::string> m_logs;
	std::size_t m_screenshotCount = 0;
	std::size_t m_screenshotBytes = 0;
	LegacyFileStatus m_status;
	bool m_stopping = false;
	std::thread m_worker;
};
} // namespace simxr
