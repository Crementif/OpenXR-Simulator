#pragma once

#include "simulation/PoseSequence.h"

#include <cstdint>
#include <string_view>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace simxr
{
std::shared_ptr<const PoseSequence> LoadStartupSequence(const std::filesystem::path& path);

struct RuntimeLogStatus
{
	bool closed = false;
	std::uint64_t errors = 0, droppedLogs = 0;
	std::string detail;
};

class RuntimeLog
{
public:
	explicit RuntimeLog(std::filesystem::path directory);
	~RuntimeLog();

	bool Log(std::string message);
	RuntimeLogStatus Status() const;
	void Close();

private:
	void RecordError(std::string_view detail) noexcept;
	void AppendLog(const std::vector<std::string>& messages);
	void Run() noexcept;

	std::filesystem::path m_directory;
	mutable std::mutex m_mutex;
	std::mutex m_closeMutex;
	std::condition_variable m_changed;
	std::vector<std::string> m_logs;
	RuntimeLogStatus m_status;
	bool m_stopping = false;
	std::thread m_worker;
};
}
