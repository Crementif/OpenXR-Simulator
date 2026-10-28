#pragma once

#include "app/services/RangeExportTask.h"
#include "app/services/ShellState.h"
#include "automation/Recording.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace simxr::app
{
class RuntimeSession;
class RangePage
{
  public:
	explicit RangePage(RuntimeSession& session) : m_session(session), m_export(session) {}
	std::optional<automation::SavedRecording> Draw(const RangeSel& range, ShellState& shell);
	void Poll()
	{
		m_export.Poll();
	}
	bool Exporting() const
	{
		return m_export.Busy();
	}
	std::optional<automation::SavedRecording> TakePlayRequest()
	{
		return std::exchange(m_play, {});
	}

  private:
	void SaveTake(const RangeSel& range, std::size_t first, std::size_t last, ShellState& shell, bool replace);
	RuntimeSession& m_session;
	RangeExportTask m_export;
	std::array<char, 512> m_path{"frames.csv"};
	std::array<char, 512> m_videoPath{"clip.mp4"};
	std::array<char, 128> m_take{};
	std::optional<automation::SavedRecording> m_last, m_play;
	std::string m_takeStatus;
	std::optional<RangeSel> m_range;
	bool m_replace = false;
};
} // namespace simxr::app
