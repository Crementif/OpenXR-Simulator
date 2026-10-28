#pragma once

#include "automation/Recording.h"

#include <filesystem>
#include <fstream>
#include <stop_token>
#include <variant>

namespace simxr::automation
{
struct GameRecordingState
{
	RecordedGamePage origin;
	std::uint64_t next = 0, total = 0;
	bool complete = false, gap = false, failed = false, uncorrelated = false;
};
using RecordingArchiveEvent = std::variant<RecordingPage, RecordedGamePage>;
struct RecordingArchiveState
{
	InputRecording origin;
	std::uint64_t identity = 0, next = 0, session = 0, generation = 0;
	bool complete = false, truncated = false;
	std::optional<GameRecordingState> game;
};
// One worker owns the journal. Only a successfully flushed Append may be acknowledged.
class RecordingArchiveWriter
{
  public:
	RecordingArchiveWriter(const std::filesystem::path& directory, const control::Message& start,
						   const std::optional<control::Message>& gameOrigin = {});
	std::uint64_t Append(const control::Message& page);
	void AppendGame(const control::Message& page, std::string_view phase = "page");
	void Finish(const control::Message& stopped);
	const RecordingArchiveState& State() const
	{
		return m_state;
	}

  private:
	void Write(const control::Message& message);
	std::ofstream m_output;
	RecordingArchiveState m_state;
	bool m_failed = false, m_gameFormat = false;
};
// Reads one bounded page at a time; an incomplete tail never becomes a complete take.
class RecordingArchiveReader
{
  public:
	explicit RecordingArchiveReader(const std::filesystem::path& directory);
	std::optional<RecordingPage> Next(std::stop_token stop = {});
	std::optional<RecordingArchiveEvent> NextEvent();
	const RecordingArchiveState& State() const
	{
		return m_state;
	}

  private:
	std::optional<control::Message> Read();
	std::ifstream m_input;
	RecordingArchiveState m_state;
	bool m_end = false, m_gameFormat = false;
};
} // namespace simxr::automation
