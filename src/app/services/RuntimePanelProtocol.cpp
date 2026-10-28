#include "app/services/RuntimePanelProtocol.h"
#include "Pch.h"

namespace simxr::app
{
namespace panel
{
std::uint64_t U64(const control::Message& message, std::uint16_t name, std::uint64_t fallback)
{
	const auto* field = message.Find(name);
	return field && field->AsU64() ? *field->AsU64() : fallback;
}
std::uint32_t U32(const control::Message& message, std::uint16_t name, std::uint32_t fallback)
{
	const auto* field = message.Find(name);
	return field && field->AsU32() ? *field->AsU32() : fallback;
}
bool Boolean(const control::Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	return field && field->AsBool() && *field->AsBool();
}
std::string Text(const control::Message& message, std::uint16_t name, const char* fallback)
{
	const auto* field = message.Find(name);
	return field && field->AsText() ? std::string(*field->AsText()) : fallback;
}
control::Message Nested(const control::Field* field)
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
std::uint64_t Frame(const control::Message& message)
{
	return U64(Nested(message.Find(control::tag::Frame)), control::tag::FrameId);
}
bool Terminal(std::string_view phase)
{
	return phase == "complete" || phase == "failed" || phase == "cancelled" || phase == "interrupted";
}
} // namespace panel
} // namespace simxr::app
