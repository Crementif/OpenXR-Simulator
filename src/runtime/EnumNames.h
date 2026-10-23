#pragma once

#include <openxr/openxr.h>

namespace simxr
{
XrResult ResultToString(XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) noexcept;
XrResult StructureTypeToString(XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) noexcept;
} // namespace simxr
