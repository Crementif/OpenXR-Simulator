#pragma once

#include "app/ShellState.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace simxr::app
{
// Something worth knowing that does not stop the session: a rejected input,
// a preview hiccup, a skipped history page, a failed export.
struct Notice
{
	// At most about six words; the same title replaces an older notice.
	std::string title;
	std::string detail;
	// An optional one-word button; running it dismisses the notice.
	std::string action;
	std::function<void()> run;
};
// The shell's notice toasts at the top centre of the preview. They never change
// the run state or the border and disappear after a few seconds.
class Notices
{
  public:
	void Push(Notice notice);
	void Draw(ShellRect area);
	// The newest notice still shown, or nullptr.
	const Notice* Latest() const;
	// Every recent notice, oldest first, after its toast is gone.
	struct Logged
	{
		std::string title, detail;
		std::chrono::steady_clock::time_point time;
	};
	const std::vector<Logged>& Log() const
	{
		return m_log;
	}
	// A notice arrived since the history was last opened.
	bool Unseen() const
	{
		return m_unseen;
	}
	void MarkSeen()
	{
		m_unseen = false;
	}

  private:
	struct Entry
	{
		Notice notice;
		std::chrono::steady_clock::time_point shown;
	};
	std::vector<Entry> m_entries;
	std::vector<Logged> m_log;
	bool m_unseen = false;
};
} // namespace simxr::app
