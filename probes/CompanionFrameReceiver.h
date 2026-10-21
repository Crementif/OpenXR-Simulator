#pragma once

#include "app/VulkanDeviceAccess.h"

#include <cstdint>
#include <memory>
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
	struct State;
	std::unique_ptr<State> m_state;
};
}
