#pragma once

namespace simxr::headset
{
class OpenXrRuntime;

struct HeadsetBodyCapabilities
{
	bool fb = false;
	bool fullBodyMETA = false;
	bool bd = false;
};

// Query only extensions enabled by OpenXrRuntime for the selected physical system.
HeadsetBodyCapabilities ReadHeadsetBodyCapabilities(const OpenXrRuntime& runtime);
} // namespace simxr::headset
