#pragma once

#include "automation/Recording.h"

#include <memory>
#include <string>

namespace simxr::app
{
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
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
