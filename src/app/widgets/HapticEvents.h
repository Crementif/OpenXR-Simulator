#pragma once

#include <cstdint>

namespace simxr::app
{
class HapticFeedback;
void DrawHapticEvents(const HapticFeedback& feedback, std::uint32_t hand, bool supported);
} // namespace simxr::app
