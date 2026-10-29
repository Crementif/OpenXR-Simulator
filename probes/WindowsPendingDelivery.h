#pragma once

#include "graphics/SharedImageTypes.h"
#include "platform/windows/WindowsControl.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr
{
class VulkanDevice;
namespace control
{
class VulkanImageConsumer;
}
namespace probe
{
int RunWindowsPendingConsumer(bool gated, std::uint32_t runtimeProcess);

class WindowsPendingDelivery
{
  public:
	explicit WindowsPendingDelivery(bool gated = false, std::uint32_t runtimeProcess = 0);
	~WindowsPendingDelivery();
	WindowsPendingDelivery(const WindowsPendingDelivery&) = delete;
	WindowsPendingDelivery& operator=(const WindowsPendingDelivery&) = delete;
	// Called before teardown of the first session, then during its replacement.
	void Hold();
	void VerifyPending();
	void ReleaseAndVerify();
	void CheckWatchdog() const;

  private:
	static constexpr std::size_t DeliveryCount = 8;
	const bool m_gated;
	const std::uint32_t m_runtimeProcess;
	control::windows::Handle m_pipe, m_process;
	void Command(std::uint8_t command) const;
	std::shared_ptr<VulkanDevice> m_device;
	std::vector<const char*> m_extensions;
	std::array<std::unique_ptr<control::VulkanImageConsumer>, DeliveryCount> m_consumers;
	std::array<SharedImageKey, DeliveryCount> m_keys{};
	VkSemaphore m_gate = VK_NULL_HANDLE;
	VkFence m_gateRetired = VK_NULL_HANDLE;
	PFN_vkSignalSemaphoreKHR m_signal = nullptr;
	ImageCompletionKind m_completion = ImageCompletionKind::Fence;
	bool m_timelineEnabled = false;
	bool m_active = false, m_held = false, m_verified = false, m_complete = false;
	std::mutex m_watchdogMutex;
	std::condition_variable m_watchdogChanged;
	std::atomic<bool> m_watchdogExpired{false};
	bool m_stopWatchdog = false;
	std::thread m_watchdog;
	void StartWatchdog();
	void StopWatchdog();
};
} // namespace probe
} // namespace simxr
