#pragma once

#include "control/legacy/LegacyFileChannel.h"
#include "control/legacy/WindowsLegacyChannel.h"

namespace simxr
{
class WindowsVulkanCaptureChannel final : public LegacyCaptureChannel
{
  public:
	explicit WindowsVulkanCaptureChannel(std::shared_ptr<mcp::WindowsLegacyChannel> files);
	LegacyFileRequests TakeRequests() override;
	bool Publish(LegacyOutputFile file, std::string document) override;
	bool SaveScreenshot(LegacyScreenshot& screenshot) override;
	bool DrainScreenshots(std::chrono::milliseconds timeout) override;
	bool Log(std::string message) override;
	LegacyFileStatus Status() const override;

  private:
	const std::shared_ptr<mcp::WindowsLegacyChannel> m_files;
};
} // namespace simxr
