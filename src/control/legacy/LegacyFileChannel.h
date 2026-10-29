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

enum class LegacyOutputFile
{
	RuntimeStatus,
	FrameInfo
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

class LegacyFileChannel
{
  public:
	explicit LegacyFileChannel(std::filesystem::path directory, std::filesystem::path initialSequence = {});
	~LegacyFileChannel();
	std::optional<LegacyInput::Sequence> TakeSequence();
	bool Publish(LegacyOutputFile file, std::string document);
	bool SaveScreenshot(LegacyScreenshot& screenshot);
	bool DrainScreenshots(std::chrono::milliseconds timeout);
	bool Log(std::string message);
	LegacyFileStatus Status() const;
	void Close();

  private:
	void Run() noexcept;
	void RecordError(std::string_view detail) noexcept;
	void WriteFile(const char* filename, const std::string& document);
	void WriteScreenshot(const LegacyScreenshot& screenshot);
	void AppendLog(const std::vector<std::string>& messages);
	std::filesystem::path m_directory;
	mutable std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::condition_variable m_changed;
	std::optional<LegacyInput::Sequence> m_sequence;
	std::array<std::optional<std::string>, 2> m_outputs;
	std::vector<LegacyScreenshot> m_screenshots;
	std::vector<std::string> m_logs;
	std::size_t m_screenshotCount = 0;
	std::size_t m_screenshotBytes = 0;
	LegacyFileStatus m_status;
	bool m_stopping = false;
	std::thread m_worker;
};
} // namespace simxr
