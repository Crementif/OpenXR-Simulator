#pragma once

#include "control/ControlSchema.h"
#include "control/WireProtocol.h"

#include <chrono>
#include <stdexcept>
#include <string>
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
	Inspect,
	Revision,
	Export,
	ExportInfo,
	ExportRead,
	Simulate,
	SimulationStatus,
	FrameHistory
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
inline std::uint64_t U64(const control::Message& message, std::uint16_t name, std::uint64_t fallback = 0)
{
	const auto* field = message.Find(name);
	return field && field->AsU64() ? *field->AsU64() : fallback;
}
inline std::uint32_t U32(const control::Message& message, std::uint16_t name, std::uint32_t fallback = 0)
{
	const auto* field = message.Find(name);
	return field && field->AsU32() ? *field->AsU32() : fallback;
}
inline bool Boolean(const control::Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	return field && field->AsBool() && *field->AsBool();
}
inline std::string Text(const control::Message& message, std::uint16_t name, const char* fallback = "")
{
	const auto* field = message.Find(name);
	return field && field->AsText() ? std::string(*field->AsText()) : fallback;
}
inline control::Message Nested(const control::Field* field)
{
	control::Message message;
	if (field)
	{
		auto decoded = control::DecodeFields(field->value);
		if (decoded.error != control::DecodeError::None)
			throw std::runtime_error("Runtime returned malformed nested metadata");
		message.fields = std::move(decoded.fields);
	}
	return message;
}
inline std::uint64_t Frame(const control::Message& message)
{
	return U64(Nested(message.Find(control::tag::Frame)), control::tag::FrameId);
}
inline bool Terminal(std::string_view phase)
{
	return phase == "complete" || phase == "failed" || phase == "cancelled" || phase == "interrupted";
}
} // namespace panel
} // namespace simxr::app
