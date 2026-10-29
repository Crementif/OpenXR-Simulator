#pragma once

#include "app/render/VulkanDeviceAccess.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

union SDL_Event;

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
	void ProcessEvent(const SDL_Event& event);
	bool BeginFrame();
	// transfers records GPU copies before the UI pass in the same submission.
	bool Render(std::span<const VkSemaphore> readyWaits = {},
				const std::function<void(VkCommandBuffer)>& transfers = {});
	VulkanDeviceAccess DeviceAccess() const;
	std::uint64_t CompletedFrames();
	void WaitIdle();
	void RequestScreenshot(std::filesystem::path path);
	std::string DeviceName() const;
	std::uint64_t SubmittedFrames() const;
	std::uint32_t RebuildCount() const;

private:
	struct Resources;
	std::shared_ptr<Resources> m_resources;
	Window* m_window = nullptr;
	bool m_rendererReady = false;
	bool m_frameStarted = false;
	bool m_rebuild = false;
	bool m_screenshots = false;
	std::size_t m_frameSlot = 0;
	std::uint64_t m_submitted = 0, m_completed = 0;
	std::uint32_t m_creations = 0;
	std::string m_adapter;
	std::filesystem::path m_screenshotPath;
	static void CheckResult(VkResult result);
	void Initialize(Window& owner, bool validation, std::shared_ptr<ValidationLog> log, bool screenshots,
					bool proofConsumer);
	void CreateSwapchain();
	void PrepareReadback();
	void RecordReadback(VkCommandBuffer command, VkImage source);
	void WriteScreenshot();
};
} // namespace simxr::app
