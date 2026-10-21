#pragma once

#include "app/RuntimePanelRequests.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace simxr::app
{
// Bounded single-job file worker. It owns only explicit CPU export bytes, never
// native resources or control handles, and performs all file I/O off the UI.
struct ImageExportWriteResult
{
	enum class Status
	{
		Saved,
		OpenFailed,
		WriteFailed
	};
	Status status;
	std::string path;
};
class ImageExportWriter
{
  public:
	ImageExportWriter();
	~ImageExportWriter();
	bool Save(std::string path, std::vector<std::byte> bytes);
	bool Busy() const;
	std::optional<ImageExportWriteResult> TakeResult();

  private:
	struct Job
	{
		std::string path;
		std::filesystem::path destination;
		std::vector<std::byte> bytes;
	};
	void Run() noexcept;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::optional<Job> m_job;
	std::optional<ImageExportWriteResult> m_result;
	bool m_busy = false, m_stopping = false;
	std::thread m_worker;
};
// Owns one connection-bound export exchange, exact byte-range validation and
// transfer to the independent file writer. Normal preview never enters it.
class ImageExportTask
{
  public:
	explicit ImageExportTask(RuntimePanelRequests& requests);
	void ResetEpoch();
	bool Request(std::uint64_t reader, std::string path);
	bool Handle(const PanelTask& task, const control::Message& message);
	void Failed(const PanelTask& task, std::string detail);
	void Poll();
	bool Busy() const;
	const std::string& Detail() const noexcept;

  private:
	void Abort(std::string detail);
	void ReleaseRemote();
	RuntimePanelRequests& m_requests;
	ImageExportWriter m_writer;
	std::uint64_t m_id = 0, m_offset = 0, m_size = 0;
	bool m_requested = false;
	std::vector<std::byte> m_bytes;
	std::string m_path, m_detail;
};
} // namespace simxr::app
