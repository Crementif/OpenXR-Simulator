#pragma once

#include "automation/RigRunner.h"
#include "control/ControlSchema.h"
#include "simulation/CommandTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::automation
{
struct ScriptStep
{
	std::optional<std::uint64_t> capture;
	std::string progress;
	std::optional<FrameId> frame;
};
struct ScriptGameOperation
{
	std::uint64_t operation = 0;
	std::uint64_t game = 0;
};

struct ScriptCaptureTarget
{
	control::TargetKind kind = control::TargetKind::ComposedEyes;
	std::uint32_t layer = 0;
	std::uint32_t view = 0;
};
struct ScriptCapture
{
	std::uint64_t identity = 0;
	std::string phase;
	std::optional<std::uint64_t> frame;
	bool imageAvailable = false;
};
struct ScriptImageRecording
{
	std::string phase;
	std::uint64_t capacity = 0;
	std::uint64_t captured = 0;
	std::uint64_t requested = 0;
	std::optional<std::uint64_t> missedFrame;
	std::vector<std::uint64_t> captures;
	bool available = false;
};
struct ScriptInspection
{
	std::vector<std::byte> document;
	std::uint64_t identity = 0;
};

class RigProgress;

class RigRuntime
{
  public:
	RigRuntime(RigExchange exchange, RigProgress& progress);
	RigRuntime(const RigRuntime&) = delete;
	RigRuntime& operator=(const RigRuntime&) = delete;
	void Configure(control::ControlSessionId session, std::uint64_t generation);
	std::string Cleanup();
	std::uint64_t Schedule(CommandTarget target, CommandLatePolicy policy, std::vector<SimulationCommand> commands);
	std::vector<std::byte> AwaitTransaction(std::uint64_t transaction);
	void PauseApplication();
	void ResumeApplication();
	ScriptStep StepApplication(ScriptCaptureTarget target);
	ScriptStep StepDisplay();
	void StartImageRecording(ScriptCaptureTarget target, std::uint32_t capacity, std::uint64_t frames);
	void StopImageRecording();
	ScriptImageRecording ImageRecording();
	ScriptCapture Capture(std::uint64_t capture);
	std::vector<std::byte> StartInputRecording();
	std::vector<std::byte> StopInputRecording();
	std::vector<std::byte> ReadInputRecording(std::uint64_t offset, std::uint64_t count);
	ScriptInspection Inspect(std::uint64_t capture);
	std::vector<std::byte> ProjectionHistory(std::uint32_t count);
	std::vector<std::byte> Revision(std::uint64_t revision);
	void ReleaseInspection(std::uint64_t inspection);
	std::vector<std::byte> State();
	void ReleaseInputs();
	void Export(std::uint64_t capture, const std::string& path,
				std::optional<ScriptCaptureTarget> target, std::optional<std::uint32_t> arraySlice);
	void ReleaseCapture(std::uint64_t capture);
	ScriptGameOperation GameCommand(std::uint32_t kind, std::string payload);
	std::vector<std::byte> AwaitGame(std::uint64_t game, std::uint64_t operation);
	std::vector<std::byte> GameState();

  private:
	using Fields = std::vector<control::Field>;
	RigExchange m_exchange;
	RigProgress& m_progress;
	control::ControlSessionId m_session;
	std::uint64_t m_generation = 0;
	std::uint64_t m_nextTransaction = 1;
	std::uint64_t m_nextCommand = 1;
	std::uint64_t m_lease = 0;
	bool m_inputRecording = false;
	std::chrono::steady_clock::time_point m_nextRenew;
	std::vector<std::uint64_t> m_captures;
	std::vector<std::uint64_t> m_inspections;
	Fields Session() const;
	Fields Source() const;
	control::Message Call(control::OpCode opcode, Fields fields);
	control::Message Exchange(control::OpCode opcode, Fields fields);
	void CleanupCall(control::OpCode opcode, Fields fields);
	control::Message Debug(control::CommandKind kind, Fields fields = {});
	ScriptStep CaptureStep(control::CommandKind kind, Fields fields = {});
	void ReleaseUnchecked(control::OpCode opcode, std::uint16_t tag, std::uint64_t id) noexcept;
	class RemoteResource;
};
} // namespace simxr::automation
