#pragma once

#include "presentation/PresentationController.h"

#include <chrono>
#include <memory>
#include <optional>

namespace simxr
{
// Single advancement owner: one pending GPU result and one completed fallback.
// VirtualDisplay serializes calls and joins its worker before Reset at shutdown.
class RotationalDisplay
{
  public:
	PresentationImage Process(const PresentationTick& baseline, const PoseSample& head);
	void Reset() noexcept;

  private:
	struct Image
	{
		std::shared_ptr<const FrameOutput> output;
		PoseSample head;
		std::chrono::steady_clock::time_point submitted;
	};
	std::optional<FrameKey> m_source;
	std::optional<Image> m_pending;
	std::optional<Image> m_ready;
	XrResult m_failure = XR_SUCCESS;
};
} // namespace simxr
