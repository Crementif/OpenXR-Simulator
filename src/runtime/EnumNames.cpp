#include "runtime/EnumNames.h"
#include "Pch.h"

#include <openxr/openxr_reflection.h>

namespace simxr
{
static std::string_view ResultName(XrResult value) noexcept
{
	switch (value)
	{
#define SIMXR_ENUM_NAME(name, value)                                                                                   \
	case name:                                                                                                         \
		return #name;
		XR_LIST_ENUM_XrResult(SIMXR_ENUM_NAME)
#undef SIMXR_ENUM_NAME
	}
	return {};
}

static std::string_view StructureTypeName(XrStructureType value) noexcept
{
	switch (value)
	{
#define SIMXR_ENUM_NAME(name, value)                                                                                   \
	case name:                                                                                                         \
		return #name;
		XR_LIST_ENUM_XrStructureType(SIMXR_ENUM_NAME)
#undef SIMXR_ENUM_NAME
	}
	return {};
}

static XrResult WriteEnumName(std::string_view name, std::string_view prefix, std::int32_t value, char* buffer,
							  std::size_t capacity) noexcept
{
	if (!buffer)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!name.empty())
	{
		const auto length = std::min(name.size(), capacity - 1);
		std::copy_n(name.data(), length, buffer);
		buffer[length] = '\0';
		return XR_SUCCESS;
	}
	std::copy(prefix.begin(), prefix.end(), buffer);
	const auto result = std::to_chars(buffer + prefix.size(), buffer + capacity - 1, value);
	*result.ptr = '\0';
	return result.ec == std::errc{} ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

XrResult ResultToString(XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) noexcept
{
	return WriteEnumName(ResultName(value), value < 0 ? "XR_UNKNOWN_FAILURE_" : "XR_UNKNOWN_SUCCESS_",
						 static_cast<std::int32_t>(value), buffer, XR_MAX_RESULT_STRING_SIZE);
}

XrResult StructureTypeToString(XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) noexcept
{
	return WriteEnumName(StructureTypeName(value), "XR_UNKNOWN_STRUCTURE_TYPE_", static_cast<std::int32_t>(value),
						 buffer, XR_MAX_STRUCTURE_NAME_SIZE);
}
} // namespace simxr
