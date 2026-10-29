#pragma once

#include "control/ControlPacket.h"

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
struct HistoryRequest
{
	control::OpCode opcode;
	std::vector<control::Field> fields;
	std::uint64_t after = 0;
	std::uint32_t count = 0;
};

bool HistoryHandles(const std::string& name);
nlohmann::json HistoryDefinitions();
HistoryRequest PrepareHistory(const std::string& name, const nlohmann::json& arguments, std::uint64_t session,
							  std::uint64_t generation);
nlohmann::json HistoryReply(const control::Message& message, const HistoryRequest& request, std::uint64_t generation);
nlohmann::json ProjectionHistoryDocument(const control::Message& message, std::uint32_t count,
										 std::uint64_t generation);
} // namespace simxr::app::mcp
