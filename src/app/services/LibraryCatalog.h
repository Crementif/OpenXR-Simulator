#pragma once

#include "automation/RecordingEvidence.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace simxr::app
{
struct LibraryEntry
{
	std::string path, name, error;
	bool take = false, archive = false;
	std::uint64_t seed = 0, firstFrame = 0, lastFrame = 0;
	std::filesystem::file_time_type modified{};
	std::uintmax_t bytes = 0;
};
enum class LibraryAction
{
	Scan,
	Rename,
	ExportRange,
	Evidence,
	Delete
};

struct LibraryResult
{
	LibraryAction action = LibraryAction::Scan;
	std::string path, renamed;
	bool changed = false, limited = false;
	std::optional<std::vector<LibraryEntry>> entries;
	std::exception_ptr error;
	std::optional<automation::RecordingEvidence> evidence;
};

// One worker serializes file changes and scans. Results remain owned until consumed.
class LibraryCatalog
{
public:
	explicit LibraryCatalog(std::filesystem::path root);
	~LibraryCatalog();
	void Refresh();
	bool Rename(std::string path, std::string name);
	bool Delete(std::string path);
	bool ExportRange(std::string path, std::string name, std::uint64_t first, std::uint64_t last);
	bool ReadEvidence(std::string path, std::uint64_t cursor = 0);
	bool Busy() const;
	std::optional<LibraryResult> TakeResult();
	static bool ValidName(const std::string& name);

private:
	struct Job
	{
		LibraryAction action;
		std::string path, name;
		std::uint64_t first = 0, last = 0;
	};

	bool Submit(Job job);
	void StartWorker();
	void Run(std::stop_token stop);
	void Scan(LibraryResult& result, std::stop_token stop);
	std::filesystem::path m_root;
	std::vector<LibraryEntry> m_archiveCache;
	mutable std::mutex m_mutex;
	std::condition_variable_any m_changed;
	std::optional<Job> m_job;
	std::optional<LibraryResult> m_result;
	bool m_refresh = false, m_busy = false;
	std::jthread m_worker;
};
} // namespace simxr::app
