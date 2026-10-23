#pragma once

#include "app/RigPanel.h"
#include "app/ShellState.h"
#include "app/widgets/Icons.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
class RuntimePanels;
// A script the Library asks to run once the session is free.
struct ScriptRun
{
	std::string path;
	ScriptOptions options;
};
// Saved takes and Luau scripts in one list (the timeline's drawer), with an
// inspector page for each: replay or run, transport, rename, delete, details and reports.
class Library
{
  public:
	Library();
	void Refresh();
	// The list: search, play buttons and opening a script by path. A click selects the entry's page;
	// a script's play button returns its run.
	std::optional<ScriptRun> DrawBrowser(Selection& selection, RuntimePanels& runtime, RigPanel& rigs);
	// The selected take's or script's page.
	std::optional<ScriptRun> Draw(Selection& selection, RuntimePanels& runtime, RigPanel& rigs);

  private:
	struct Entry
	{
		std::string path, name;
		bool take = false;
		std::uint64_t seed = 0;
		std::filesystem::file_time_type modified{};
		std::uintmax_t bytes = 0;
	};
	const Entry* Find(const std::string& path) const;
	void DrawTake(Selection& selection, const std::string& path, RuntimePanels& runtime, RigPanel& rigs);
	void DrawScript(Selection& selection, const std::string& path, RuntimePanels& runtime, RigPanel& rigs);
	// Name, rename, delete and close; false once the file is gone or the page closed.
	bool DrawHeader(Selection& selection, const std::string& path, Icon kind);
	void Play(const Entry& entry, RuntimePanels& runtime);
	std::filesystem::path m_takes, m_scripts;
	std::vector<Entry> m_entries;
	std::chrono::steady_clock::time_point m_nextScan{};
	std::array<char, 96> m_search{}, m_rename{};
	std::array<char, 2048> m_open{};
	std::string m_error, m_report;
	ScriptOptions m_options;
	std::optional<ScriptRun> m_run;
};
} // namespace simxr::app
