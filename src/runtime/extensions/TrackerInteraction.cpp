#include "runtime/extensions/TrackerInteraction.h"
#include "runtime/validation/StructureChain.h"
#include "simulation/BodyModel.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <tuple>

namespace simxr
{
TrackerInteraction::TrackerInteraction(ActionRegistry& registry, const EnabledExtensions& enabled, EventQueue& events)
	: m_registry(registry), m_events(events),
	  m_enabled(enabled.Contains(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME))
{
	if (m_enabled)
	{
		TrackingSnapshot initial;
		initial.time = {Duration{1}};
		for (std::size_t index = 0; index < initial.trackers.size(); ++index)
			initial.trackers[index].device = initial.input.trackers[index];
		if (XR_FAILED(Publish(initial)))
			throw std::runtime_error("Cannot initialize tracker inventory");
	}
}

void TrackerInteraction::BeginSession()
{
	std::lock_guard lock(m_mutex);
	// Persistent tracker paths survive a session; its deterministic clock does not.
	m_time = {};
}

XrResult TrackerInteraction::Publish(const TrackingSnapshot& snapshot, std::vector<XrPath>* changedPaths)
{
	if (!m_enabled)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (snapshot.time.value.count() <= 0)
		return XR_ERROR_TIME_INVALID;
	std::vector<TrackerPathState> next;
	for (const auto& sample : snapshot.trackers)
	{
		TrackerPathState paths{sample.device.identity, XR_NULL_PATH, XR_NULL_PATH, sample.device.connected};
		if (!paths.identity.value || std::any_of(next.begin(), next.end(),
												 [&](const auto& value)
												 {
													 return value.identity == paths.identity;
												 }))
			return XR_ERROR_VALIDATION_FAILURE;
		try
		{
			auto result =
				m_registry.StringToPath(BodyModel::TrackerPersistentPath(paths.identity), paths.persistentPath);
			if (XR_FAILED(result))
				return result;
			auto role = BodyModel::TrackerRolePath(sample.device.role);
			if (!role.empty())
			{
				result = m_registry.StringToPath(role, paths.rolePath);
				if (XR_FAILED(result))
					return result;
			}
		}
		catch (const std::invalid_argument&)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (paths.connected && paths.rolePath &&
			std::any_of(next.begin(), next.end(),
						[&](const auto& value)
						{
							return value.connected && value.rolePath == paths.rolePath;
						}))
			return XR_ERROR_VALIDATION_FAILURE;
		next.push_back(paths);
	}
	std::sort(next.begin(), next.end(),
			  [](const auto& a, const auto& b)
			  {
				  return a.identity.value < b.identity.value;
			  });
	std::lock_guard lock(m_mutex);
	if (snapshot.time < m_time)
		return XR_ERROR_TIME_INVALID;
	std::vector<XrPath> changed;
	// Stage notifications while changed-path storage can still allocate.
	std::array<TrackerConnectedEvent, std::tuple_size_v<decltype(snapshot.trackers)>> connected{};
	std::size_t connectedCount = 0;
	auto remember = [&](const TrackerPathState& paths)
	{
		for (auto path : {paths.persistentPath, paths.rolePath})
			if (path && std::find(changed.begin(), changed.end(), path) == changed.end())
				changed.push_back(path);
	};
	for (const auto& paths : next)
	{
		auto previous = std::find_if(m_paths.begin(), m_paths.end(),
									 [&](const auto& value)
									 {
										 return value.identity == paths.identity;
									 });
		bool metadataChanged = previous == m_paths.end()
								   ? paths.connected
								   : previous->connected != paths.connected || previous->rolePath != paths.rolePath;
		if (!metadataChanged)
			continue;
		remember(paths);
		if (previous != m_paths.end())
			remember(*previous);
		if (paths.connected || (previous != m_paths.end() && previous->rolePath != paths.rolePath))
			connected[connectedCount++] = {paths.persistentPath, paths.rolePath};
	}
	for (const auto& previous : m_paths)
		if (std::none_of(next.begin(), next.end(),
						 [&](const auto& paths)
						 {
							 return paths.identity == previous.identity;
						 }))
			remember(previous);
	m_paths = std::move(next);
	m_time = snapshot.time;
	if (changedPaths)
		*changedPaths = std::move(changed);
	for (std::size_t index = 0; index < connectedCount; ++index)
		m_events.Push(connected[index]);
	return XR_SUCCESS;
}

XrResult TrackerInteraction::Enumerate(std::uint32_t capacity, std::uint32_t* count,
									   XrViveTrackerPathsHTCX* paths) const
{
	if (!m_enabled)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!count || (capacity && !paths))
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(m_mutex);
	*count = static_cast<std::uint32_t>(std::count_if(m_paths.begin(), m_paths.end(),
													  [](const auto& value)
													  {
														  return value.connected;
													  }));
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (std::uint32_t index = 0; index < *count; ++index)
		if (paths[index].type != XR_TYPE_VIVE_TRACKER_PATHS_HTCX ||
			XR_FAILED(StructureChain(paths[index].next).Validate({})))
			return XR_ERROR_VALIDATION_FAILURE;
	std::uint32_t index = 0;
	for (const auto& value : m_paths)
		if (value.connected)
		{
			paths[index].persistentPath = value.persistentPath;
			paths[index].rolePath = value.rolePath;
			++index;
		}
	return XR_SUCCESS;
}

void TrackerInteraction::ContributeInput(const TrackingSnapshot& snapshot, InputSnapshot& input) const
{
	auto previous = std::move(input.devices);
	input.devices.clear();
	for (const auto& device : previous)
		if (device.identity.kind != InputDeviceKind::Tracker)
			input.devices.push_back(device);
	if (!m_enabled)
		return;
	for (const auto& sample : snapshot.trackers)
	{
		InputDevice device{{InputDeviceKind::Tracker, sample.device.identity.value},
						   {BodyModel::TrackerPersistentPath(sample.device.identity)},
						   "/interaction_profiles/htc/vive_tracker_htcx",
						   sample.device.connected,
						   true,
						   {}};
		auto role = BodyModel::TrackerRolePath(sample.device.role);
		if (!role.empty())
			device.userPaths.emplace_back(role);
		if (auto old = std::find_if(previous.begin(), previous.end(),
									[&](const auto& value)
									{
										return value.identity == device.identity;
									});
			old != previous.end())
			device.controls = old->controls;
		input.devices.push_back(std::move(device));
	}
}

PoseSample TrackerInteraction::Pose(const TrackingSnapshot& snapshot, TrackerIdentity identity) const
{
	if (!m_enabled)
		return {};
	for (const auto& sample : snapshot.trackers)
		if (sample.device.identity == identity)
			return sample.pose;
	return {};
}

std::vector<TrackerPathState> TrackerInteraction::Paths() const
{
	std::lock_guard lock(m_mutex);
	return m_paths;
}
} // namespace simxr
