#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace simxr::windows
{
struct WindowsFileStatus
{
	bool closed = false;
	std::uint64_t errors = 0;
	std::uint64_t droppedLogs = 0;
	std::string detail;
};

class WindowsRuntimeFiles
{
public:
	explicit WindowsRuntimeFiles(std::filesystem::path directory);
	~WindowsRuntimeFiles();
	bool PublishStatus(std::string document);
	bool Log(std::string message);
	WindowsFileStatus Status() const;
	void Close();

private:
	void Run() noexcept;
	void RecordError(std::string_view error) noexcept;
	std::filesystem::path m_directory;
	mutable std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::condition_variable m_changed;
	std::optional<std::string> m_document;
	std::vector<std::string> m_logs;
	WindowsFileStatus m_status;
	bool m_stopping = false;
	std::thread m_worker;
};
} // namespace simxr::windows
