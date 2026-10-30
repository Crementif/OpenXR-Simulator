#pragma once

#include "simulation/TrackingTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace simxr::app
{
struct ObservedSetup;

struct HeadsetHostAlignment
{
	float x = 0, z = 0, yaw = 0, floor = 0;
};

HeadsetHostAlignment HeadsetAlignmentFor(const ObservedSetup& observed);

struct HeadsetHostOptions
{
	std::filesystem::path executable;
	std::filesystem::path runtimeManifest;
	std::string endpoint;
	std::uint64_t session = 0;
	std::uint64_t generation = 0;
	std::uint64_t connectionEpoch = 0;
	std::optional<HeadsetHostAlignment> alignment;
	std::optional<float> localEyeHeight;
	bool takeOver = false;
	bool trackers = false;
	std::optional<BodyJointSet> body;
	bool discover = false;
};
enum class HeadsetHostPhase
{
	Idle,
	Starting,
	Running,
	Stopping,
	Exited,
	Failed
};
enum class HeadsetInputState
{
	Waiting,
	Ready,
	Suspended
};
enum class HeadsetOutputState
{
	Waiting,
	Live,
	Inspection,
	Hidden
};

struct HeadsetEyeSize
{
	std::uint32_t width = 0, height = 0;
	bool operator==(const HeadsetEyeSize&) const = default;
};

struct HeadsetHostSnapshot
{
	HeadsetHostPhase phase = HeadsetHostPhase::Idle;
	// The host's report, without the status lines that drive this snapshot: key=value
	// facts with their latest values in first-seen order, runtime extensions and
	// layers, and the remaining log lines.
	std::vector<std::pair<std::string, std::string>> details;
	std::vector<std::string> extensions;
	std::string diagnostics;
	std::string error;
	int exitCode = 0;
	bool forcedStop = false;
	bool discovering = false;
	HeadsetInputState input = HeadsetInputState::Waiting;
	HeadsetOutputState output = HeadsetOutputState::Waiting;
	std::optional<std::array<HeadsetEyeSize, 2>> recommendedEyes;
	std::optional<bool> trackerSupport;
	std::array<std::optional<bool>, 3> bodySupport;
	std::optional<std::uint32_t> connectedTrackers;
};

// UI-thread commands; the worker alone owns the native process and its pipes.
// Running means the process exists, not that tracking/display attachment is ready.
class HeadsetHost
{
public:
	~HeadsetHost();
	void Start(HeadsetHostOptions options);
	void Stop();
	void Realign(HeadsetHostAlignment alignment);
	bool Restarting() const;
	// Poll from the UI thread; matching targets may complete a discovery connection or realignment.
	void ObserveTarget(const std::string& endpoint, std::uint64_t session, std::uint64_t generation,
					   std::uint64_t connectionEpoch);
	HeadsetHostSnapshot Snapshot() const;

private:
	void Run(std::stop_token stop, HeadsetHostOptions options);
	mutable std::mutex m_mutex;
	HeadsetHostSnapshot m_snapshot;
	std::jthread m_worker;
	std::optional<HeadsetHostOptions> m_target, m_restart;
};
} // namespace simxr::app
