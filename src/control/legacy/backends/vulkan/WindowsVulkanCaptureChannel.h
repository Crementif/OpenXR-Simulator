#pragma once

#include "control/legacy/LegacyFileChannel.h"
#include "control/legacy/WindowsLegacyCapture.h"
#include "runtime/Session.h"

namespace simxr
{
class WindowsVulkanCaptureChannel final : public LegacyCaptureChannel
{
  public:
	WindowsVulkanCaptureChannel(std::shared_ptr<mcp::WindowsLegacyChannel> files,
								std::shared_ptr<mcp::WindowsLegacyCapture> capture, std::shared_ptr<Session> session,
								std::function<mcp::HeadPoseCommand()> head,
								std::function<void(const mcp::HeadPoseCommand&)> step);
	LegacyFileRequests TakeRequests() override;
	bool Publish(LegacyOutputFile file, std::string document) override;
	bool SaveScreenshot(LegacyScreenshot& screenshot) override;
	bool DrainScreenshots(std::chrono::milliseconds timeout) override;
	bool FinishCaptures(std::chrono::milliseconds timeout) override;
	bool Log(std::string message) override;
	LegacyFileStatus Status() const override;

  private:
	mcp::BurstFrameMeta BurstMetadata(const mcp::HeadPoseCommand& head) const;
	const std::shared_ptr<mcp::WindowsLegacyChannel> m_files;
	const std::shared_ptr<mcp::WindowsLegacyCapture> m_capture;
	const std::shared_ptr<Session> m_session;
	const std::function<mcp::HeadPoseCommand()> m_head;
	const std::function<void(const mcp::HeadPoseCommand&)> m_step;
	mcp::HeadPoseCommand m_pendingStep;
	std::uint64_t m_burstGeneration = 0;
};
} // namespace simxr
