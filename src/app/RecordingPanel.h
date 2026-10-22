#pragma once

#include "app/RecordingController.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace simxr::app
{
class RecordingPanel
{
  public:
	RecordingPanel();
	void SetTarget(RecordingTarget target);
	void Poll(bool supported);
	RecordingSnapshot Snapshot() const;
	void Stop();
	void RequestStart()
	{
		m_startRequested = true;
	}
	// A take written elsewhere (Save as take): list and select it.
	void Added(const std::string& path)
	{
		RefreshFiles();
		SelectFile(path);
	}
	std::optional<automation::SavedRecording> Draw(bool supported, bool replayAvailable, bool replayActive = false);

  private:
	RecordingController m_controller;
	RecordingTarget m_target;
	void RefreshFiles();
	void Save();
	void SelectFile(const std::string& path);
	std::filesystem::path m_directory;
	std::vector<std::string> m_files;
	std::optional<automation::SavedRecording> m_selected;
	std::chrono::steady_clock::time_point m_nextRead{};
	std::string m_error;
	bool m_saving = false, m_startRequested = false;
	std::uint64_t m_offset = 0;
	std::array<char, 128> m_name{};
	std::array<char, 2048> m_path{};
};
} // namespace simxr::app
