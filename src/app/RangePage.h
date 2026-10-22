#pragma once

#include "app/RuntimeView.h"
#include "automation/Recording.h"

#include <array>
#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace simxr::app
{
class PreviewImages;
class RuntimeSession;
// Writes a range's frame metadata as CSV (on a worker) and its retained
// snapshots as PPM files, one export request at a time. The snapshots stay
// protected from eviction exactly while the export holds them.
class RangeExportTask
{
  public:
	explicit RangeExportTask(RuntimeSession& session) : m_session(session)
	{
	}
	void Start(const RangeSel& range, std::size_t first, std::size_t last, const std::string& path);
	void Poll();
	bool Busy() const;
	const std::string& Metadata() const noexcept
	{
		return m_metadata;
	}
	const std::string& Images() const noexcept
	{
		return m_images;
	}

  private:
	// Releases the eviction protection on every exit path (done, cancelled, disconnected, failed).
	class Protection
	{
	  public:
		Protection(PreviewImages& preview, std::span<const std::uint64_t> images);
		~Protection();
		Protection(const Protection&) = delete;
		Protection& operator=(const Protection&) = delete;

	  private:
		PreviewImages& m_preview;
	};
	void Finish(std::string result);
	RuntimeSession& m_session;
	std::future<std::string> m_csv;
	std::vector<std::pair<std::uint64_t, std::string>> m_queue;
	std::optional<Protection> m_protection;
	std::uint64_t m_epoch = 0;
	std::size_t m_next = 0;
	bool m_requested = false;
	std::string m_metadata, m_images;
};
// Inspector page for a selected range of frames.
class RangePage
{
  public:
	explicit RangePage(RuntimeSession& session) : m_session(session), m_export(session)
	{
	}
	// Returns a take to replay when one was saved and the user asked to replay it.
	std::optional<automation::SavedRecording> Draw(const RangeSel& range, ShellState& shell);
	void Poll()
	{
		m_export.Poll();
	}
	bool Exporting() const
	{
		return m_export.Busy();
	}

	// The take written by the last Save as take, for the library to select.
	std::optional<std::string> TakeSaved()
	{
		return std::exchange(m_saved, {});
	}

  private:
	void SaveTake(const RangeSel& range, std::size_t first, std::size_t last, ShellState& shell);
	RuntimeSession& m_session;
	RangeExportTask m_export;
	std::array<char, 512> m_path{"frames.csv"};
	std::array<char, 128> m_take{};
	std::optional<std::string> m_saved;
	std::optional<automation::SavedRecording> m_last;
	std::string m_takeStatus;
};
} // namespace simxr::app
