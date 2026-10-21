#pragma once

#include "graphics/GpuCompletion.h"
#include <openxr/openxr.h>
#include <span>
#include <vector>

namespace simxr
{
struct FrameView
{
	XrPosef pose;
	XrFovf fov;
};
// A backend-owned private output and its producer completion observation.
// Polling must be thread-safe and nonblocking: no submissions, context changes,
// CPU waits, or mutation of native queue access. Complete permits selection;
// Complete and Failed are terminal observations.
// Failed does not establish GPU idleness. The backend independently preserves
// native resource safety after failure, cancellation, or the last CPU release.
class FrameOutput : public GpuCompletion
{
  public:
	explicit FrameOutput(std::span<const FrameView> views = {}) : m_views(views.begin(), views.end()) {}
	std::span<const FrameView> Views() const noexcept
	{
		return m_views;
	}
	~FrameOutput() override = default;

  private:
	const std::vector<FrameView> m_views;
};
} // namespace simxr
