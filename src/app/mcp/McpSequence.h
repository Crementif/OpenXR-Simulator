#pragma once

#include "control/ControlPacket.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace simxr::app::mcp
{
struct SequenceRequest
{
	control::OpCode opcode;
	std::vector<control::Field> fields;
};

bool SequenceHandles(const std::string& name);
nlohmann::json SequenceDefinitions();
SequenceRequest PrepareSequence(const std::string& name, const nlohmann::json& arguments, std::uint64_t session,
								std::uint64_t generation);
nlohmann::json SequenceReply(const control::Message& response, const SequenceRequest& request);
nlohmann::json SequenceDocument(const control::Message& response, const SequenceRequest& request);
std::uint64_t SequenceInteger(const nlohmann::json& object, const char* key, bool zero = false);
void DecimalSequenceIdentities(nlohmann::json& value);
}
