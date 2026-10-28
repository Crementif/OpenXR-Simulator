#pragma once

#include "simulation/TrackingTypes.h"

#include <array>
#include <openxr/openxr.h>
#include <string>
#include <vector>

namespace simxr::headset
{
class OpenXrRuntime;
struct PhysicalTrackerSample
{
	std::string persistentPath;
	std::string rolePath;
	TrackerRole role = TrackerRole::Unassigned;
	bool needsBinding = false;
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
};

class HeadsetTrackers
{
  public:
	explicit HeadsetTrackers(OpenXrRuntime& runtime);
	~HeadsetTrackers();
	HeadsetTrackers(const HeadsetTrackers&) = delete;
	HeadsetTrackers& operator=(const HeadsetTrackers&) = delete;
	// Must precede attachment of the caller-owned action set.
	void Open(XrSession session, XrActionSet actionSet);
	// Called after synchronizing that action set.
	std::vector<PhysicalTrackerSample> Sample(XrTime time, XrSpace baseSpace, bool locate = true);
	bool Available() const;
	void Close();

  private:
	struct RoleAction
	{
		XrPath path = XR_NULL_PATH;
		XrAction action = XR_NULL_HANDLE;
		XrSpace space = XR_NULL_HANDLE;
		TrackerRole role = TrackerRole::Unassigned;
	};
	std::string PathString(XrPath path) const;
	std::vector<XrViveTrackerPathsHTCX> Enumerate() const;
	OpenXrRuntime& m_runtime;
	XrSession m_session = XR_NULL_HANDLE;
	PFN_xrEnumerateViveTrackerPathsHTCX m_enumerate = nullptr;
	std::vector<RoleAction> m_roles;
	std::vector<RoleAction> m_devices;
};
} // namespace simxr::headset
