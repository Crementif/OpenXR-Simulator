#pragma once

#include "app/RecordingController.h"

#include <array>
#include <optional>

namespace simxr::app
{
class RecordingPanel
{
  public:
	RecordingPanel();
	void SetTarget(RecordingTarget target);
	std::optional<automation::SavedRecording> Draw(bool supported, bool replayAvailable);

  private:
	RecordingController m_controller;
	RecordingTarget m_target;
	std::uint64_t m_offset = 0;
	std::array<char, 2048> m_path{};
};
} // namespace simxr::app
