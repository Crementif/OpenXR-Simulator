#pragma once

#include "app/services/InputHistory.h"
#include <cstddef>

namespace simxr::app
{
// Read-only views of one recorded frame's body, gaze and tracker input, for
// device pages while looking back and for the Frame page.
void DrawPastBody(const InputSample& sample);
void DrawPastGaze(const InputSample& sample);
void DrawPastTrackers(const InputSample& sample, std::size_t selected);
// One line each for body, gaze and connected trackers.
void DrawInputSummary(const InputSample& sample);
} // namespace simxr::app
