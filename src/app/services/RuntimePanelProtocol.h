#pragma once

#include "control/ControlSchema.h"
#include "control/WireProtocol.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace simxr::app
{
using PanelFields = std::vector<control::Field>;
using PanelClock = std::chrono::steady_clock;
enum class PanelWork
{
	Sessions,
	Status,
	Acquire,
	Renew,
	Command,
	Operation,
	Acknowledge,
	Capture,
	Retain,
	Preview,
	Share,
	Release,
	Relinquish,
	Inspect,
	Revision,
	Export,
	ExportInfo,
	ExportRead,
	Simulate,
	SimulationStatus,
	FrameHistory,
	Haptics
};
struct PanelSession
{
	std::uint64_t id = 0, generation = 0;
	bool running = false;
};
struct PanelScope
{
	std::uint64_t epoch = 0, session = 0, generation = 0;
	bool operator==(const PanelScope&) const = default;
};
struct PanelTask
{
	PanelWork kind;
	std::uint64_t id = 0, session = 0, generation = 0;
};
namespace panel
{
std::uint64_t U64(const control::Message& message, std::uint16_t name, std::uint64_t fallback = 0);
std::uint32_t U32(const control::Message& message, std::uint16_t name, std::uint32_t fallback = 0);
bool Boolean(const control::Message& message, std::uint16_t name);
std::string Text(const control::Message& message, std::uint16_t name, const char* fallback = "");
control::Message Nested(const control::Field* field);
std::uint64_t Frame(const control::Message& message);
bool Terminal(std::string_view phase);
} // namespace panel
} // namespace simxr::app
