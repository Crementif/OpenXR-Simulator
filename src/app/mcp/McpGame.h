#pragma once

#include "control/ControlPacket.h"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
struct GameRequest
{
	control::OpCode opcode;
	std::vector<control::Field> fields;
};
bool GameHandles(const std::string& name);
nlohmann::json GameDefinitions();
GameRequest PrepareGame(const std::string& name, const nlohmann::json& arguments,
						std::uint64_t session, std::uint64_t generation);
nlohmann::json GameReply(const control::Message& message, control::OpCode opcode);
} // namespace simxr::app::mcp
