#pragma once

#include "control/WireProtocol.h"
#include "automation/MonitorReport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace simxr::automation
{
// Local source files are independent of the per-message IPC payload budget.
inline constexpr std::size_t MaxRigSourceBytes = 64 * 1024 * 1024;

enum class RigPhase
{
	Idle,
	Running,
	Paused,
	Complete,
	Cancelled,
	Failed
};
struct RigCheckpoint
{
	std::string name, detail;
	bool passed = false;
};
struct RigSnapshot
{
	RigPhase phase = RigPhase::Idle;
	std::uint64_t seed = 0, transactions = 0, captures = 0;
	std::string error;
	MonitorReport monitor;
	std::vector<RigCheckpoint> checkpoints;
	std::vector<std::string> output;
	std::optional<std::vector<std::string>> compatibilityDeviations;
};
struct RigOptions
{
	control::ControlSessionId session;
	std::uint64_t generation = 0, seed = 0;
	std::string source, name = "rig.luau";
	std::chrono::milliseconds deadline{30000};
	std::size_t memoryLimit = 64 * 1024 * 1024;
	std::filesystem::path recordingArchive;
	// Only application-owned, built-in observation policy runs without a total deadline.
	bool background = false;
};
// The transport belongs to this runner and is called only on its worker. It
// must bound each exchange and preserve retry identity after a timeout.
using RigExchange = std::function<control::Message(control::OpCode, std::vector<control::Field>)>;
} // namespace simxr::automation
