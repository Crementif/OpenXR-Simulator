#pragma once

#include "control/ControlPacket.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

namespace simxr::app::mcp
{
struct ImageRequest
{
	control::OpCode opcode;
	std::vector<control::Field> fields;
};

struct ImageRenderRequest
{
	std::uint64_t ticket = 0;
	bool png = false;
	int quality = 85;
	std::uint64_t session = 0, generation = 0;
};
struct CachedImage;

struct ImageRenderResult
{
	control::Message info;
	std::shared_ptr<const CachedImage> image;
};

bool ImageHandles(const std::string& name);
nlohmann::json ImageDefinitions();
ImageRequest PrepareImage(const std::string& name, const nlohmann::json& arguments, std::uint64_t session,
						  std::uint64_t generation);
ImageRenderRequest PrepareImageRender(const nlohmann::json& arguments);
std::uint64_t ImageId(const nlohmann::json& object, const char* key, bool zero = false);
nlohmann::json ImageDocument(const control::Message& message);
nlohmann::json ImageReply(const control::Message& message);
std::string EncodeBase64(std::span<const std::byte> bytes);
} // namespace simxr::app::mcp
