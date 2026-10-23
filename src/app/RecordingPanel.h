#pragma once

#include "app/services/RecordingController.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>
#include <string>

namespace simxr::app
{
// The input recorder behind the top bar's Rec button: start, stop and the
// "Save take" popover that names and writes the take.
class RecordingPanel
{
  public:
	RecordingPanel();
	void SetTarget(RecordingTarget target);
	void Poll(bool supported);
	RecordingSnapshot Snapshot() const;
	void RequestStart()
	{
		m_startRequested = true;
	}
	void RequestStop()
	{
		m_stopRequested = true;
	}
	// Draw right after the Rec button; the popovers open from the requests above.
	void DrawPopovers(bool supported);
	// The path of a take this recorder just saved.
	std::optional<std::string> TakeSaved()
	{
		return std::exchange(m_saved, {});
	}

  private:
	void Start();
	void Save();
	RecordingController m_controller;
	RecordingTarget m_target;
	std::optional<RecordingTarget> m_popupTarget;
	std::filesystem::path m_directory;
	std::chrono::steady_clock::time_point m_nextRead{};
	std::string m_error;
	std::optional<std::string> m_saved;
	bool m_saving = false, m_startRequested = false, m_stopRequested = false, m_discarded = false;
	std::array<char, 128> m_name{};
};
} // namespace simxr::app
