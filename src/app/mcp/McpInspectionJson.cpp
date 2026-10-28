#include "app/mcp/McpPch.h"
#include "app/mcp/McpSimulation.h"
#include "control/InspectionDocument.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;

static std::vector<wire::Field> Fields(std::span<const std::byte> bytes)
{
	auto result = wire::DecodeFields(bytes);
	if (result.error != wire::DecodeError::None)
		throw std::runtime_error("Runtime returned malformed inspection data");
	return std::move(result.fields);
}
static bool IdentityName(std::string_view name)
{
	static const std::set<std::string_view> identities{"id", "identity", "session", "generation", "frame_id", "trigger_frame", "space_revision",
		"transaction_id", "applied_frame", "seed", "revision_id", "recording_id", "source_id", "operation_id", "game_id"};
	return identities.contains(name);
}
static Json Node(std::span<const std::byte> bytes, std::size_t depth, std::size_t& count, std::string* label = nullptr)
{
	if (depth > 32 || ++count > 65536)
		throw std::runtime_error("Runtime inspection document exceeds its structural bound");
	wire::Message node;
	node.fields = Fields(bytes);
	const auto* name = node.Find(1);
	const auto* type = node.Find(2);
	const auto* data = node.Find(3);
	if (!name || !name->AsText() || !type || !type->AsU32() || !data)
		throw std::runtime_error("Runtime returned an invalid inspection node");
	const auto key = std::string(*name->AsText());
	if (label)
		*label = key;
	constexpr std::uint64_t safe = (1ULL << 53) - 1;
	switch (static_cast<wire::InspectionType>(*type->AsU32()))
	{
	case wire::InspectionType::Null:
		if (!data->value.empty())
			throw std::runtime_error("Invalid null inspection value");
		return nullptr;
	case wire::InspectionType::Boolean:
		if (const auto value = data->AsBool())
			return *value;
		break;
	case wire::InspectionType::Unsigned:
	case wire::InspectionType::Revision:
		if (const auto value = data->AsU64())
			return IdentityName(key) || *type->AsU32() == static_cast<std::uint32_t>(wire::InspectionType::Revision) || *value > safe
					   ? Json(std::to_string(*value)) : Json(*value);
		break;
	case wire::InspectionType::Signed:
		if (const auto value = data->AsI64())
			return *value > static_cast<std::int64_t>(safe) || *value < -static_cast<std::int64_t>(safe)
					   ? Json(std::to_string(*value)) : Json(*value);
		break;
	case wire::InspectionType::Float:
		if (const auto value = data->AsF32())
		{
			if (std::isfinite(*value))
				return *value;
			if (std::isnan(*value))
				return "NaN";
			return std::signbit(*value) ? "-Infinity" : "Infinity";
		}
		break;
	case wire::InspectionType::Text:
		if (const auto value = data->AsText())
			return std::string(*value);
		break;
	case wire::InspectionType::Object:
	{
		Json object = Json::object();
		for (const auto& field : Fields(data->value))
		{
			std::string childName;
			auto value = Node(field.value, depth + 1, count, &childName);
			if (childName.empty() || object.contains(childName))
				throw std::runtime_error("Runtime inspection object has invalid field names");
			object[childName] = std::move(value);
		}
		return object;
	}
	case wire::InspectionType::Array:
	{
		Json array = Json::array();
		for (const auto& page : Fields(data->value))
			for (const auto& field : Fields(page.value))
			{
				std::string childName;
				auto value = Node(field.value, depth + 1, count, &childName);
				if (!childName.empty())
					throw std::runtime_error("Runtime inspection array has a named element");
				array.push_back(std::move(value));
			}
		return array;
	}
	}
	throw std::runtime_error("Runtime returned an invalid inspection value");
}
Json InspectionJson(std::span<const std::byte> document)
{
	std::size_t count = 0;
	return Node(document, 0, count);
}
} // namespace simxr::app::mcp
