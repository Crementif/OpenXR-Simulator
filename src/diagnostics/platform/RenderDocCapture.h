#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace flicker
{
struct RenderDocResult
{
	bool attempted = false;
	bool succeeded = false;
};

class RenderDocCapture
{
  public:
	RenderDocCapture();
	RenderDocResult Trigger(uint64_t frame);

  private:
	bool m_enabled = false;
	std::string m_captureTemplate;
	uint32_t m_frameCount = 3;
	std::optional<uint64_t> m_lastTriggerFrame;
};

} // namespace flicker
