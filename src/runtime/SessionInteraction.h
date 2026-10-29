#pragma once

#include "simulation/ActionRegistry.h"
#include "simulation/HapticState.h"

namespace simxr
{
class Instance;

// Session serializes mutations. The parent instance and its action registry
// outlive this owner; public action/haptic snapshots use their own reader locks.
class SessionInteraction
{
public:
	SessionInteraction(XrSession session, Instance& instance);
	void Reset();
	void SetFocused(bool focused, RuntimeTime time);
	void LoseFocus(RuntimeTime time);
	XrResult Attach(const XrSessionActionSetsAttachInfo& info, const InputSnapshot& input);
	XrResult Sync(const XrActionsSyncInfo& info, const InputSnapshot& input, RuntimeTime time, bool focused);
	void PublishProfileChange(bool changed, bool running);
	XrResult ApplyHaptic(const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback, RuntimeTime time,
						 bool focused);
	XrResult StopHaptic(const XrHapticActionInfo* info, RuntimeTime time, bool focused);
	ActionSyncState& Actions();
	HapticState& Haptics();

private:
	void RetireInactiveHaptics(RuntimeTime time);
	XrSession m_session;
	Instance& m_instance;
	ActionSyncState m_actions;
	HapticState m_haptics;
	bool m_focused = false;
	bool m_profileChangePending = false;
};
} // namespace simxr
