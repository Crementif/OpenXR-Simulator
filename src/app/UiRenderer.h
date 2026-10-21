#pragma once

#include "app/VulkanDeviceAccess.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace simxr::app
{
class Window;
struct ValidationLog
{
	std::atomic<std::uint32_t> errors{0};
};

class UiRenderer
{
  public:
	UiRenderer(Window& window, bool validation, std::shared_ptr<ValidationLog> log, bool screenshots = false,
			   bool proofConsumer = false);
	~UiRenderer();
	UiRenderer(const UiRenderer&) = delete;
	UiRenderer& operator=(const UiRenderer&) = delete;
	bool BeginFrame();
	bool Render(std::span<const VkSemaphore> readyWaits = {});
	VulkanDeviceAccess DeviceAccess() const;
	std::uint64_t CompletedFrames();
	void WaitIdle();
	void RequestScreenshot(std::string path);
	std::string DeviceName() const;
	std::uint64_t SubmittedFrames() const;
	std::uint32_t RebuildCount() const;

  private:
	struct Impl;
	std::shared_ptr<Impl> m_impl;
};
} // namespace simxr::app
