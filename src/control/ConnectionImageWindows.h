#pragma once

#include "control/ProtocolTypes.h"
#include "presentation/FrameImageHistory.h"

#include <cstdint>
#include <memory>

namespace simxr::control
{
struct ImageWindowReader
{
	ControlSessionId session;
	std::shared_ptr<FrameImageWindow> window;
};
// One window per connection. Detached readers must outlive the request gate.
class ConnectionImageWindows
{
  public:
	std::uint64_t Store(ControlSessionId session, std::shared_ptr<FrameImageWindow> window);
	ImageWindowReader Read(std::uint64_t id) const;
	ImageWindowReader Release(std::uint64_t id) noexcept;

  private:
	std::uint64_t m_next = 1, m_id = 0;
	ImageWindowReader m_reader;
};
} // namespace simxr::control
