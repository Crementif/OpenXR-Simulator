#pragma once

#include "automation/Recording.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace simxr::app
{
class WorkerConnection;
struct RecordingTarget
{
	std::string endpoint;
	std::uint64_t session = 0, generation = 0;
	bool operator==(const RecordingTarget&) const = default;
};
enum class RecordingAction
{
	Start,
	Stop,
	Read,
	Save
};
struct RecordingSnapshot
{
	bool busy = false;
	std::shared_ptr<const automation::InputRecording> page;
	std::uint64_t offset = 0;
	std::string error, savedPath;
	std::uint64_t savedSeed = 0;
};
// Read the metadata of an editable recording without executing its script.
automation::SavedRecording OpenSavedRecording(const std::string& path);
// Writes an editable take through a private temporary file. Existing recording
// saves replace their destination; a new range take can require a fresh name.
void SaveRecording(const std::filesystem::path& destination, const automation::InputRecording& recording,
				   bool replace = true);

// One joined worker owns recording control, transport and explicit persistence.
class RecordingController
{
  public:
	RecordingController();
	~RecordingController();
	RecordingController(const RecordingController&) = delete;
	RecordingController& operator=(const RecordingController&) = delete;
	void SetTarget(RecordingTarget target);
	bool Request(RecordingAction action, std::uint64_t offset = 0, std::string path = {});
	RecordingSnapshot Snapshot() const;

  private:
	struct Job
	{
		RecordingAction action;
		RecordingTarget target;
		std::uint64_t revision, offset;
		std::string path;
	};
	static control::Message Read(WorkerConnection& connection, const Job& job, std::uint64_t offset);
	static RecordingSnapshot Execute(WorkerConnection& connection, const Job& job);
	void Run();
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	bool m_stopping = false;
	std::uint64_t m_revision = 0;
	RecordingTarget m_target;
	std::optional<Job> m_pending;
	RecordingSnapshot m_snapshot;
	std::shared_ptr<WorkerConnection> m_current;
};
} // namespace simxr::app
