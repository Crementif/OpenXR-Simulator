#pragma once

#include <functional>
#include <string>

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
} // namespace simxr::app
