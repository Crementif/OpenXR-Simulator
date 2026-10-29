#pragma once

#include "VulkanProbeSupport.h"
#include "app/render/VulkanDeviceAccess.h"

#include <array>
#include <cstdint>
#include <vector>

namespace simxr::app
{
struct CompanionProofFrame
{
	std::uint32_t id;
	VkImageView view;
	VkSampler sampler;
	VkSemaphore sampleReady;
	VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
	std::uint32_t width = 64;
	std::uint32_t height = 64;
};

// Qualification-only receiver for the fixed three-frame FD proof protocol.
// The renderer device outlives this object; drain its UI submissions before
// destroying registered views. Producer waits use only the separate IO queue.
class CompanionFrameReceiver
{
public:
	explicit CompanionFrameReceiver(VulkanDeviceAccess access, int channel = 3);
	~CompanionFrameReceiver();
	CompanionFrameReceiver(const CompanionFrameReceiver&) = delete;
	CompanionFrameReceiver& operator=(const CompanionFrameReceiver&) = delete;
	void Poll();
	std::vector<CompanionProofFrame> ReadyFrames() const;
	bool AllCopied() const;
	void MarkSampled(std::uint32_t id);
	void Finish(bool verifyPixels = false);

private:
	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};

	struct Frame
	{
		Image output;
		VkSemaphore sampleReady = VK_NULL_HANDLE;
		VkFence completion = VK_NULL_HANDLE;
		bool submitted = false;
		bool copied = false;
		bool sampled = false;
	};

	void Drain() noexcept;
	void Destroy(Image& image);
	void CreateImage(Image& image, VkImageUsageFlags usage, bool external);
	void Allocate(Image& image, VkDeviceSize size, std::uint32_t type, int descriptor = -1);
	VkSemaphore Semaphore(int descriptor = -1);
	void Initialize();
	VkCommandBuffer Begin();
	void SubmitCopy(std::uint32_t index);
	void Verify();
	VulkanDeviceAccess m_access;
	int m_channel;
	probe::FileDescriptor m_ownedChannel;
	bool m_initialized = false;
	bool m_finished = false;
	std::uint32_t m_received = 0;
	Image m_source;
	VkSemaphore m_ready = VK_NULL_HANDLE;
	VkSemaphore m_released = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkCommandPool m_commands = VK_NULL_HANDLE;
	std::array<Frame, 3> m_frames;
};
} // namespace simxr::app
