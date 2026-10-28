#include "headset/HeadsetTrackers.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"

namespace simxr::headset
{
struct TrackerRoleBinding
{
	const char* name;
	TrackerRole role;
	std::uint32_t revision;
};
static constexpr std::array s_roles{TrackerRoleBinding{"handheld_object", TrackerRole::HandheldObject, 1},
									TrackerRoleBinding{"left_foot", TrackerRole::LeftFoot, 1},
									TrackerRoleBinding{"right_foot", TrackerRole::RightFoot, 1},
									TrackerRoleBinding{"left_shoulder", TrackerRole::LeftShoulder, 1},
									TrackerRoleBinding{"right_shoulder", TrackerRole::RightShoulder, 1},
									TrackerRoleBinding{"left_elbow", TrackerRole::LeftElbow, 1},
									TrackerRoleBinding{"right_elbow", TrackerRole::RightElbow, 1},
									TrackerRoleBinding{"left_knee", TrackerRole::LeftKnee, 1},
									TrackerRoleBinding{"right_knee", TrackerRole::RightKnee, 1},
									TrackerRoleBinding{"waist", TrackerRole::Waist, 1},
									TrackerRoleBinding{"chest", TrackerRole::Chest, 1},
									TrackerRoleBinding{"camera", TrackerRole::Camera, 1},
									TrackerRoleBinding{"keyboard", TrackerRole::Keyboard, 1},
									TrackerRoleBinding{"left_wrist", TrackerRole::LeftWrist, 3},
									TrackerRoleBinding{"right_wrist", TrackerRole::RightWrist, 3},
									TrackerRoleBinding{"left_ankle", TrackerRole::LeftAnkle, 3},
									TrackerRoleBinding{"right_ankle", TrackerRole::RightAnkle, 3}};

HeadsetTrackers::HeadsetTrackers(OpenXrRuntime& runtime) : m_runtime(runtime) {}
HeadsetTrackers::~HeadsetTrackers()
{
	Close();
}
void HeadsetTrackers::Close()
{
	if (m_runtime.ResourcesPreserved())
		return;
	for (const auto& role : m_roles)
		if (role.space)
			xrDestroySpace(role.space);
	for (const auto& device : m_devices)
		if (device.space)
			xrDestroySpace(device.space);
	m_devices.clear();
	m_roles.clear();
	m_enumerate = nullptr;
	m_session = XR_NULL_HANDLE;
}
bool HeadsetTrackers::Available() const
{
	return m_enumerate != nullptr;
}
void HeadsetTrackers::Open(XrSession session, XrActionSet actionSet)
{
	if (m_session)
		throw std::logic_error("Physical tracker provider is already open");
	m_session = session;
	std::uint32_t revision = 0;
	for (const auto& extension : m_runtime.Extensions())
		if (std::strcmp(extension.extensionName, XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME) == 0)
			revision = extension.extensionVersion;
	if (!revision)
		return;
	m_enumerate =
		reinterpret_cast<PFN_xrEnumerateViveTrackerPathsHTCX>(m_runtime.Procedure("xrEnumerateViveTrackerPathsHTCX"));
	if (!m_enumerate)
		throw std::runtime_error("Physical runtime did not expose tracker enumeration");
	const auto path = [&](const std::string& text)
	{
		XrPath value = XR_NULL_PATH;
		CheckXr(xrStringToPath(m_runtime.Instance(), text.c_str(), &value), "Create physical tracker path");
		return value;
	};
	m_roles.reserve(s_roles.size());
	std::vector<XrActionSuggestedBinding> bindings;
	const auto bind = [&](RoleAction& role, const std::string& prefix, const std::string& name)
	{
		role.path = path(prefix);
		XrActionCreateInfo action{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(action.actionName, name.c_str());
		std::strcpy(action.localizedActionName, name.c_str());
		action.actionType = XR_ACTION_TYPE_POSE_INPUT;
		action.countSubactionPaths = 1;
		action.subactionPaths = &role.path;
		CheckXr(xrCreateAction(actionSet, &action, &role.action), "Create physical tracker action");
		bindings.push_back({role.action, path(prefix + "/input/grip/pose")});
		XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		space.action = role.action;
		space.subactionPath = role.path;
		space.poseInActionSpace.orientation.w = 1;
		CheckXr(xrCreateActionSpace(session, &space, &role.space), "Create physical tracker space");
	};
	for (const auto& binding : s_roles)
	{
		if (binding.revision > revision)
			continue;
		auto& role = m_roles.emplace_back();
		role.role = binding.role;
		bind(role, std::string("/user/vive_tracker_htcx/role/") + binding.name, std::string("tracker_") + binding.name);
	}
	const auto devices = Enumerate();
	m_devices.reserve(devices.size());
	for (const auto& device : devices)
	{
		const auto persistent = PathString(device.persistentPath);
		if (persistent.empty() || std::any_of(m_devices.begin(), m_devices.end(),
											  [&](const RoleAction& prior)
											  {
												  return prior.path == device.persistentPath;
											  }))
			throw std::runtime_error("Physical runtime returned missing or duplicate tracker identities");
		const auto name = "tracker_device_" + std::to_string(m_devices.size());
		bind(m_devices.emplace_back(), persistent, name);
	}
	XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggestion.interactionProfile = path("/interaction_profiles/htc/vive_tracker_htcx");
	suggestion.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
	suggestion.suggestedBindings = bindings.data();
	CheckXr(xrSuggestInteractionProfileBindings(m_runtime.Instance(), &suggestion),
			"Suggest physical tracker bindings");
}
std::string HeadsetTrackers::PathString(XrPath path) const
{
	if (!path)
		return {};
	std::array<char, XR_MAX_PATH_LENGTH> text{};
	std::uint32_t length = 0;
	CheckXr(xrPathToString(m_runtime.Instance(), path, text.size(), &length, text.data()),
			"Read physical tracker path");
	if (length < 2 || length > text.size() || text[length - 1] != '\0' || std::strlen(text.data()) != length - 1)
		throw std::runtime_error("Physical runtime returned an invalid tracker path");
	return {text.data(), length - 1};
}
std::vector<XrViveTrackerPathsHTCX> HeadsetTrackers::Enumerate() const
{
	std::array<XrViveTrackerPathsHTCX, MaxTrackerDevices> paths;
	for (auto& path : paths)
		path = {XR_TYPE_VIVE_TRACKER_PATHS_HTCX};
	std::uint32_t count = 0;
	const auto result = m_enumerate(m_runtime.Instance(), paths.size(), &count, paths.data());
	if (result == XR_ERROR_SIZE_INSUFFICIENT || count > paths.size())
		throw std::runtime_error("Physical tracker inventory exceeds the 64-device limit");
	CheckXr(result, "Enumerate physical trackers");
	return {paths.begin(), paths.begin() + count};
}
std::vector<PhysicalTrackerSample> HeadsetTrackers::Sample(XrTime time, XrSpace baseSpace, bool locate)
{
	if (!Available())
		return {};
	const auto paths = Enumerate();
	const auto count = paths.size();
	std::vector<PhysicalTrackerSample> samples;
	samples.reserve(count);
	for (std::uint32_t index = 0; index < count; ++index)
	{
		const auto& path = paths[index];
		if (!path.persistentPath)
			throw std::runtime_error("Physical tracker has no persistent identity");
		PhysicalTrackerSample sample;
		sample.persistentPath = PathString(path.persistentPath);
		sample.rolePath = PathString(path.rolePath);
		for (const auto& prior : samples)
			if (prior.persistentPath == sample.persistentPath)
				throw std::runtime_error("Physical runtime returned duplicate tracker identities");
		const auto role = std::find_if(m_roles.begin(), m_roles.end(),
									   [&](const RoleAction& value)
									   {
										   return value.path == path.rolePath;
									   });
		if (role != m_roles.end())
			sample.role = role->role;
		const auto persistent = std::find_if(m_devices.begin(), m_devices.end(),
											 [&](const RoleAction& value)
											 {
												 return value.path == path.persistentPath;
											 });
		const RoleAction* selected = persistent == m_devices.end() ? nullptr : &*persistent;
		if (!selected && role != m_roles.end() &&
			std::count_if(paths.begin(), paths.end(),
						  [&](const XrViveTrackerPathsHTCX& value)
						  {
							  return value.rolePath == path.rolePath;
						  }) == 1)
			selected = &*role;
		sample.needsBinding = selected == nullptr;
		if (locate && selected)
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = selected->action;
			info.subactionPath = selected->path;
			XrActionStatePose active{XR_TYPE_ACTION_STATE_POSE};
			CheckXr(xrGetActionStatePose(m_session, &info, &active), "Read physical tracker activity");
			if (active.isActive)
				CheckXr(xrLocateSpace(selected->space, baseSpace, time, &sample.location), "Locate physical tracker");
		}
		samples.push_back(std::move(sample));
	}
	std::sort(samples.begin(), samples.end(),
			  [](const auto& left, const auto& right)
			  {
				  return left.persistentPath < right.persistentPath;
			  });
	return samples;
}
} // namespace simxr::headset
