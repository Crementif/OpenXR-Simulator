#pragma once

#include "app/services/RecordingCapture.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace simxr::app
{
// Captures directly to a new archive; completed takes need no separate save step.
class RecordingPanel
{
  public:
	RecordingPanel();
	void SetTarget(RecordingTarget target);
	void Poll(bool supported);
	RecordingCaptureSnapshot Snapshot() const;
	void RequestStart()
	{
		m_startRequested = true;
		m_stopRequested = false;
	}
	void RequestStop()
	{
		m_stopRequested = true;
		m_startRequested = false;
	}
	void DrawPopovers(bool supported);
	std::optional<std::string> TakeSaved()
	{
		return std::exchange(m_saved, {});
	}

  private:
	RecordingCapture m_capture;
	RecordingTarget m_target;
	std::filesystem::path m_directory;
	std::string m_error;
	std::optional<std::string> m_saved;
	bool m_startRequested = false, m_stopRequested = false;
	bool m_wasBusy = false, m_showResult = false, m_retired = false;
};
} // namespace simxr::app
