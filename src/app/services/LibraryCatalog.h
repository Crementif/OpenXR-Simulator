#pragma once

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
	bool take = false;
	std::uint64_t seed = 0;
	std::filesystem::file_time_type modified{};
	std::uintmax_t bytes = 0;
};
enum class LibraryAction
{
	Scan,
	Rename,
	Delete
};
struct LibraryResult
{
	LibraryAction action = LibraryAction::Scan;
	std::string path, renamed;
	bool changed = false, limited = false;
	std::optional<std::vector<LibraryEntry>> entries;
	std::exception_ptr error;
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
	bool Busy() const;
	std::optional<LibraryResult> TakeResult();
	static bool ValidName(const std::string& name);

  private:
	struct Job
	{
		LibraryAction action;
		std::string path, name;
	};
	bool Submit(Job job);
	void StartWorker();
	void Run(std::stop_token stop);
	void Scan(LibraryResult& result, std::stop_token stop) const;
	std::filesystem::path m_root;
	mutable std::mutex m_mutex;
	std::condition_variable_any m_changed;
	std::optional<Job> m_job;
	std::optional<LibraryResult> m_result;
	bool m_refresh = false, m_busy = false;
	std::jthread m_worker;
};
} // namespace simxr::app
