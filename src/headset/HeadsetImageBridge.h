#pragma once

#include "control/ImageInspection.h"
#include "headset/HeadsetImageProbe.h"
#include "presentation/FrameTypes.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace simxr::control
{
class LocalControlClient;
}

namespace simxr::headset
{
enum class HeadsetImageKind
{
	Image,
	Clear,
	Unchanged,
	CompletionChanged,
	Unavailable
};

struct HeadsetImagePacket
{
	HeadsetImageKind kind = HeadsetImageKind::Unavailable;
	std::uint64_t imageTick = 0, delivery = 0, session = 0;
	FrameKey frame;
	std::optional<std::chrono::nanoseconds> sourceAge;
	std::chrono::steady_clock::time_point requestedAt;
	control::Message source;
	control::ImageDescription description;
	std::vector<std::shared_ptr<control::TransferHandle>> handles;
};

std::optional<std::chrono::nanoseconds> HeadsetImageAge(const HeadsetImagePacket& packet,
														std::chrono::steady_clock::time_point now);
bool FreshHeadsetImage(const HeadsetImagePacket& packet, std::chrono::steady_clock::time_point now);

// Transport only. The physical frame thread owns imports, GPU submissions and
// completion. One delivery remains retained until that owner acknowledges it.
class HeadsetImageBridge
{
public:
	explicit HeadsetImageBridge(HeadsetImageSource source);
	~HeadsetImageBridge();
	bool Request(control::ImageDeviceIdentity identity, std::shared_ptr<control::TransferHandle> completion,
				 std::uint64_t afterImageTick = 0, ImageCompletionKind completionKind = ImageCompletionKind::Timeline);
	std::optional<HeadsetImagePacket> Take();
	bool Acknowledge(std::uint64_t delivery);
	std::string Error() const;
	// Closing transport never asserts GPU completion. The caller must retire or
	// preserve any offered native consumer before destroying its graphics device.
	void Stop();

private:
	enum class Phase
	{
		Idle,
		Requested,
		Delivered,
		ReleaseRequested,
		Failed,
		Stopped
	};

	struct Offer
	{
		control::ImageDeviceIdentity identity;
		std::shared_ptr<control::TransferHandle> completion;
		std::uint64_t afterImageTick;
		ImageCompletionKind completionKind;
	};

	void Run(std::stop_token stop) noexcept;
	HeadsetImageSource m_source;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	Phase m_phase = Phase::Idle;
	std::optional<Offer> m_offer;
	std::optional<HeadsetImagePacket> m_packet;
	std::uint64_t m_delivery = 0;
	std::string m_error;
	std::shared_ptr<control::LocalControlClient> m_client;
	std::jthread m_worker;
};
} // namespace simxr::headset
