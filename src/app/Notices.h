#pragma once

#include "app/ShellState.h"

#include <chrono>
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
};
// The shell's notice toasts at the top right of the preview. They never change
// the run state or the border and disappear after a few seconds.
class Notices
{
  public:
	void Push(Notice notice);
	void Draw(ShellRect area);
	// The newest notice still shown, or nullptr.
	const Notice* Latest() const;

  private:
	struct Entry
	{
		Notice notice;
		std::chrono::steady_clock::time_point shown;
	};
	std::vector<Entry> m_entries;
};
} // namespace simxr::app
