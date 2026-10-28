#pragma once

#include "automation/Recording.h"
#include <filesystem>
#include <stop_token>

namespace simxr::automation
{
// Inclusive original applied-frame endpoints. The first frame becomes editable
// initialization; subsequent outcomes retain their original frame/runtime anchors.
InputRecording ReadRecordingRange(const std::filesystem::path& directory, FrameId first, FrameId last,
								  std::stop_token stop = {});
} // namespace simxr::automation
