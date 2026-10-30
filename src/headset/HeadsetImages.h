#pragma once

#include "control/backends/vulkan/VulkanImageConsumer.h"
#include "headset/HeadsetImageBridge.h"

#include <chrono>
#include <memory>
#include <vector>

namespace simxr::headset
{
class HeadsetGraphics;

struct HeadsetImageTiming
{
	std::uint64_t requests = 0, copies = 0, unchanged = 0, clears = 0, capacityWaits = 0;
	std::size_t retained = 0, maximumRetained = 0;
	std::chrono::nanoseconds maximumDelivery{}, maximumCopyObservation{};
};

struct HeadsetImageFrame
{
	std::unique_ptr<control::VulkanImageConsumer> pixels;
	HeadsetImagePacket metadata;
};

// All methods, including destruction, run on the physical frame thread.
// Presenters retain Current() through their own GPU completion. Copies are
// complete before publication; SampleReady must not be waited on each repeat.
class HeadsetImages
{
public:
	HeadsetImages(HeadsetGraphics& graphics, HeadsetImageSource source, std::shared_ptr<VulkanDevice> device = {});
	~HeadsetImages();

	void Tick();
	bool Close();

	std::shared_ptr<const HeadsetImageFrame> Current() const;
	bool Fresh() const;
	std::optional<std::chrono::nanoseconds> Age(std::chrono::steady_clock::time_point now) const;

	HeadsetImageTiming Timing() const
	{
		auto timing = m_timing;
		timing.retained = m_frames.size();
		return timing;
	}

private:
	// Advances one request, delivery or copy stage; false when it must wait.
	bool Step();
	void RetireUnused();

	HeadsetGraphics& m_graphics;
	std::shared_ptr<VulkanDevice> m_device;
	HeadsetImageBridge m_bridge;
	std::vector<std::shared_ptr<HeadsetImageFrame>> m_frames;
	std::shared_ptr<HeadsetImageFrame> m_current;
	std::unique_ptr<control::VulkanImageConsumer> m_pending;
	std::optional<HeadsetImagePacket> m_packet;
	std::chrono::steady_clock::time_point m_deadline;
	std::uint64_t m_imageTick = 0;
	HeadsetImageTiming m_timing;
	ImageCompletionKind m_completionKind = ImageCompletionKind::Timeline;
	std::chrono::steady_clock::time_point m_requestedAt{}, m_copyStarted{};
	bool m_requested = false, m_closed = false, m_retired = false, m_presentationAvailable = false;
	static constexpr std::size_t s_capacity = 3;
	static constexpr unsigned s_stepsPerTick = 8;
};
} // namespace simxr::headset
