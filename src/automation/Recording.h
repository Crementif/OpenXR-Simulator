#pragma once

#include "control/WireProtocol.h"
#include "simulation/CommandTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::automation
{
struct SavedRecording
{
	std::string path;
	std::uint64_t seed = 0;
};
struct RecordedTransaction
{
	std::uint64_t id = 0;
	std::string phase;
	FrameId frame;
	std::vector<SimulationCommand> commands;
	bool afterFrame = false;
};
struct InputRecording
{
	std::uint64_t version = 0, seed = 0, total = 0;
	FrameId originFrame;
	std::int64_t originTime = 0;
	bool active = false, overflow = false;
	std::vector<SimulationCommand> initial;
	ScenarioState initialScenarios{};
	std::vector<RecordedTransaction> transactions;
};
struct RecordingPage
{
	InputRecording recording;
	std::uint64_t offset = 0;
	bool end = false;
};
RecordingPage DecodeRecordingPage(const control::Message& response);

// Collects a stopped snapshot transactionally, rejecting mixed pages or gaps.
class RecordingCollection
{
  public:
	void Append(RecordingPage page);
	std::uint64_t Offset() const noexcept;
	bool Complete() const noexcept;
	const InputRecording& Recording() const;

  private:
	std::optional<InputRecording> m_recording;
	std::vector<std::byte> m_initialIdentity;
	std::size_t m_bytes = 0;
	bool m_complete = false;
};
// Exact initialization shared by editable export and core replay. Persistent
// failures are restored; consumed recenter triggers are cleared, never repeated.
std::vector<SimulationCommand> ReplayInitialCommands(const InputRecording& recording);
std::string EditableRig(const InputRecording& recording);
std::string EditableCommand(const SimulationCommand& command);
} // namespace simxr::automation
