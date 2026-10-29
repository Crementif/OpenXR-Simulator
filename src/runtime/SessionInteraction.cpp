#include "runtime/SessionInteraction.h"
#include "Pch.h"
#include "runtime/Instance.h"

namespace simxr
{
SessionInteraction::SessionInteraction(XrSession session, Instance& instance)
	: m_session(session), m_instance(instance), m_actions(instance.Actions())
{}

void SessionInteraction::Reset()
{
	m_haptics.Reset();
	m_actions.Reset();
	m_focused = false;
}

void SessionInteraction::SetFocused(bool focused, RuntimeTime time)
{
	if (m_focused && !focused)
		m_haptics.LoseFocus(time);
	m_actions.SetFocused(focused);
	m_focused = focused;
}

void SessionInteraction::LoseFocus(RuntimeTime time)
{
	m_haptics.LoseFocus(time);
	m_actions.SetFocused(false);
	m_focused = false;
}

XrResult SessionInteraction::Attach(const XrSessionActionSetsAttachInfo& info, const InputSnapshot& input)
{
	return m_actions.Attach(info, input);
}

XrResult SessionInteraction::Sync(const XrActionsSyncInfo& info, const InputSnapshot& input, RuntimeTime time,
								  bool focused)
{
	bool profilesChanged = false;
	const auto result = m_actions.Sync(info, input, focused, &profilesChanged);
	if (XR_FAILED(result))
		return result;
	m_profileChangePending |= profilesChanged;
	RetireInactiveHaptics(std::max(time, input.time));
	return result;
}

void SessionInteraction::PublishProfileChange(bool changed, bool running)
{
	m_profileChangePending |= changed;
	if (!running || !m_profileChangePending)
		return;
	m_instance.Events().Push(InteractionProfileEvent{m_session});
	m_profileChangePending = false;
}

void SessionInteraction::RetireInactiveHaptics(RuntimeTime time)
{
	for (const auto& output : m_haptics.Snapshot(time))
	{
		if (output.status != HapticStatus::Active)
			continue;
		const XrHapticActionInfo target{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, output.action, output.subactionPath};
		std::vector<InputDeviceId> devices;
		const auto result = m_actions.OutputTargets(target, devices);
		if (XR_SUCCEEDED(result) && std::find(devices.begin(), devices.end(), output.device) != devices.end())
			continue;
		const std::array stale{output.device};
		m_haptics.Stop(stale, output.action, time, true);
	}
}

XrResult SessionInteraction::ApplyHaptic(const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback,
										 RuntimeTime time, bool focused)
{
	if (!info || !feedback || feedback->type != XR_TYPE_HAPTIC_VIBRATION)
		return XR_ERROR_VALIDATION_FAILURE;
	std::vector<InputDeviceId> targets;
	const auto result = m_actions.OutputTargets(*info, targets);
	if (XR_FAILED(result))
		return result;
	return m_haptics.Apply(targets, info->action, info->subactionPath,
						   *reinterpret_cast<const XrHapticVibration*>(feedback), time, focused);
}

XrResult SessionInteraction::StopHaptic(const XrHapticActionInfo* info, RuntimeTime time, bool focused)
{
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	std::vector<InputDeviceId> targets;
	const auto result = m_actions.OutputTargets(*info, targets);
	if (XR_FAILED(result))
		return result;
	return m_haptics.Stop(targets, info->action, time, focused);
}

ActionSyncState& SessionInteraction::Actions()
{
	return m_actions;
}

HapticState& SessionInteraction::Haptics()
{
	return m_haptics;
}
} // namespace simxr
