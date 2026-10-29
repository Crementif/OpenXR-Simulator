#pragma once

#include "headset/HeadsetHaptics.h"
#include "headset/HeadsetImageProbe.h"
#include "headset/HeadsetSpace.h"
#include "presentation/FrameTypes.h"
#include "simulation/TrackingTypes.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <openxr/openxr.h>
#include <optional>

namespace simxr::headset
{
class OpenXrRuntime;
class HeadsetGraphics;
class HeadsetTestPattern;
class HeadsetActions;
class HeadsetBody;
class HeadsetImages;
class HeadsetOutput;
struct HeadsetInputSample;
struct HeadsetImageTiming;

struct HeadsetFrameTiming
{
	std::uint64_t frames = 0, projected = 0, inspected = 0, empty = 0, repeated = 0;
	std::chrono::nanoseconds maximumWait{}, maximumEndFrame{};
	XrTime physicalPrediction = 0;
	std::uint64_t sourceSession = 0, imageTick = 0;
	FrameKey sourceFrame;
	std::optional<std::chrono::nanoseconds> sourceAge;
};
enum class HeadsetOutputState
{
	Waiting,
	Live,
	Inspection,
	Hidden
};

class HeadsetSession
{
public:
	HeadsetSession(OpenXrRuntime& runtime, HeadsetGraphics& graphics);
	~HeadsetSession();
	HeadsetSession(const HeadsetSession&) = delete;
	HeadsetSession& operator=(const HeadsetSession&) = delete;
	void Open(std::optional<HeadsetImageSource> source = {}, bool trackers = false,
			  std::optional<BodyJointSet> body = {});
	void SetCalibration(std::optional<XrPosef> worldFromInitial);
	void Run(std::optional<std::uint32_t> frames, const std::function<void(const HeadsetInputSample&)>& observe = {},
			 const std::function<bool()>& stopRequested = {});
	bool Close();

	HeadsetOutputState OutputState() const
	{
		return m_outputState;
	}

	HeadsetFrameTiming Timing() const
	{
		return m_timing;
	}

	HeadsetImageTiming ImageTiming() const;
	void ApplyHaptics(const std::array<std::optional<HeadsetVibration>, 2>& commands);

private:
	void PollEvents();
	bool Frame(bool render, const std::function<void(const HeadsetInputSample&)>& observe);
	OpenXrRuntime& m_runtime;
	HeadsetGraphics& m_graphics;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_space = XR_NULL_HANDLE;
	XrReferenceSpaceType m_referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	HeadsetSpace m_spaceHistory;
	std::unique_ptr<HeadsetTestPattern> m_pattern;
	std::unique_ptr<HeadsetActions> m_actions;
	std::unique_ptr<HeadsetBody> m_body;
	std::unique_ptr<HeadsetImages> m_images;
	std::unique_ptr<HeadsetOutput> m_output;
	std::optional<XrPosef> m_worldFromInitial;
	std::optional<XrPosef> m_inspectionAnchor;
	std::uint64_t m_validHeadSamples = 0;
	std::array<std::uint64_t, 2> m_validGripSamples{};
	XrTime m_lastPrediction = 0;
	HeadsetFrameTiming m_timing;
	std::uint64_t m_lastPresentedImage = 0;
	HeadsetOutputState m_outputState = HeadsetOutputState::Waiting;
	bool m_running = false;
	bool m_exiting = false;
	bool m_exitRequested = false;
};
} // namespace simxr::headset
