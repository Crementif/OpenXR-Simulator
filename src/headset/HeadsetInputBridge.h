#pragma once

#include "headset/HeadsetActions.h"
#include "headset/HeadsetHaptics.h"
#include "headset/HeadsetTracking.h"
#include "platform/Clock.h"
#include "presentation/FrameTypes.h"

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace simxr::control
{
class LocalControlClient;
}
namespace simxr::headset
{
struct HeadsetInputTarget
{
	std::string endpoint;
	std::uint64_t session = 0;
	SessionGeneration generation;
	XrPosef worldFromPhysical{};
	std::uint64_t spaceRevision = 1;
	std::optional<HeadsetAlignment> alignment;
	bool takeOver = false;
	bool displayTiming = false;
	bool trackers = false;
	bool body = false;
	bool requireNeutral = false;
};
enum class HeadsetInputPhase
{
	Connecting,
	Ready,
	Suspended,
	Failed,
	Stopped
};
struct HeadsetInputStatus
{
	HeadsetInputPhase phase = HeadsetInputPhase::Connecting;
	std::uint64_t sent = 0;
	std::uint64_t predictions = 0;
	std::uint64_t queuedSamples = 0, dequeuedSamples = 0, rejectedSamples = 0, discardedSamples = 0;
	std::size_t queueDepth = 0, queueHighWater = 0;
	Duration maximumQueueDelay{}, maximumForwardAge{};
	std::optional<XrPosef> worldFromInitial;
	std::string error;
};
// Owns the IPC worker. The OpenXR thread only copies observations into a bounded queue.
class HeadsetInputBridge
{
  public:
	explicit HeadsetInputBridge(HeadsetInputTarget target);
	~HeadsetInputBridge();
	bool Publish(const HeadsetInputSample& input, NativeClockDomain domain, std::int64_t observed,
				 std::int64_t predicted, std::optional<std::int64_t> bodyReturned = {});
	HeadsetInputStatus Status() const;
	std::array<std::optional<HeadsetVibration>, 2> ConsumeHaptics(bool enabled);
	void Stop();

  private:
	struct Observation
	{
		HeadsetInputSample input;
		NativeClockDomain domain;
		std::int64_t observed = 0, predicted = 0;
		HostTime received;
		std::optional<std::int64_t> bodyReturned;
	};
	void Run(std::stop_token stop) noexcept;
	void Fail(const char* error) noexcept;
	HeadsetInputTarget m_target;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::array<Observation, 64> m_queue{};
	std::size_t m_first = 0, m_count = 0;
	HeadsetInputStatus m_status;
	HeadsetHaptics m_haptics;
	HostTime m_inputDeadline{};
	std::shared_ptr<control::LocalControlClient> m_client;
	std::jthread m_worker;
};
} // namespace simxr::headset
