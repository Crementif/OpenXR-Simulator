#include "simulation/ActionRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace simxr
{
namespace
{
struct ProfileComponent
{
	std::string_view profile;
	std::string_view extension;
	std::string_view title;
	std::string_view user;
	std::string_view component;
	XrActionType type;
	bool system;
};

constexpr ProfileComponent s_components[]{
#include "simulation/InteractionProfiles.inc"
};

bool ValidPath(std::string_view path)
{
	if (path.size() < 2 || path.size() >= XR_MAX_PATH_LENGTH || path.front() != '/' || path.back() == '/')
		return false;
	bool segmentHasName = false;
	for (char character : path.substr(1))
	{
		if (character == '/')
		{
			if (!segmentHasName)
				return false;
			segmentHasName = false;
			continue;
		}
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
			  character == '-' || character == '.'))
			return false;
		segmentHasName |= character != '.';
	}
	return segmentHasName;
}

bool ValidName(std::string_view name)
{
	if (name.empty())
		return false;
	for (char character : name)
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
			  character == '-' || character == '.'))
			return false;
	return true;
}

bool ValidUtf8(std::string_view text)
{
	for (std::size_t index = 0; index < text.size();)
	{
		auto first = static_cast<unsigned char>(text[index++]);
		if (first < 0x80)
			continue;
		std::uint32_t value;
		std::size_t continuation;
		std::uint32_t minimum;
		if (first >= 0xc2 && first <= 0xdf)
		{
			value = first & 0x1f;
			continuation = 1;
			minimum = 0x80;
		}
		else if (first >= 0xe0 && first <= 0xef)
		{
			value = first & 0x0f;
			continuation = 2;
			minimum = 0x800;
		}
		else if (first >= 0xf0 && first <= 0xf4)
		{
			value = first & 7;
			continuation = 3;
			minimum = 0x10000;
		}
		else
			return false;
		if (continuation > text.size() - index)
			return false;
		for (std::size_t byte = 0; byte < continuation; ++byte)
		{
			auto next = static_cast<unsigned char>(text[index++]);
			if ((next & 0xc0) != 0x80)
				return false;
			value = (value << 6) | (next & 0x3f);
		}
		if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
			return false;
	}
	return true;
}

template <std::size_t Size> std::string_view BoundedString(const char (&text)[Size])
{
	return {text, static_cast<std::size_t>(std::find(text, text + Size, '\0') - text)};
}

bool Compatible(XrActionType action, XrActionType source)
{
	if (action == source)
		return true;
	return (action == XR_ACTION_TYPE_BOOLEAN_INPUT && source == XR_ACTION_TYPE_FLOAT_INPUT) ||
		   (action == XR_ACTION_TYPE_FLOAT_INPUT && source == XR_ACTION_TYPE_BOOLEAN_INPUT);
}

bool PersistentTrackerPath(std::string_view path)
{
	constexpr std::string_view prefix = "/user/vive_tracker_htcx/serial/";
	return path.starts_with(prefix) && path.size() > prefix.size() &&
		   path.find('/', prefix.size()) == std::string_view::npos;
}

bool UserAllowed(std::string_view path, const EnabledExtensions& enabled)
{
	if (path == "/user/head" || path == "/user/gamepad" || path == "/user/hand/left" || path == "/user/hand/right")
		return true;
	if (PersistentTrackerPath(path))
		return enabled.Contains(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
	return std::any_of(
		std::begin(s_components), std::end(s_components), [&](const auto& entry)
		{ return entry.user == path && (entry.extension.empty() || enabled.Contains(entry.extension)); });
}

struct DeviceView
{
	InputDeviceId identity;
	std::vector<XrPath> users;
	XrPath profile = XR_NULL_PATH;
	bool connected = false;
	bool poseAvailable = false;
	ControllerControls controls;
};

bool SetSelected(const ActionDefinition& action, const DeviceView& device, const std::vector<XrActiveActionSet>& sets)
{
	return std::any_of(sets.begin(), sets.end(),
					   [&](const auto& set)
					   {
						   return set.actionSet == action.set && (set.subactionPath == XR_NULL_PATH ||
																  std::find(device.users.begin(), device.users.end(),
																			set.subactionPath) != device.users.end());
					   });
}

std::string SourceKey(const ActionBinding& binding, InputDeviceId device)
{
	auto component = binding.component;
	auto last = component.find_last_of('/');
	if (last != std::string::npos && last > component.find('/', 1))
		component.resize(last);
	return std::to_string(static_cast<int>(device.kind)) + ":" + std::to_string(device.value) + component;
}

XrResult Devices(ActionRegistry& registry, const InputSnapshot& input, std::vector<DeviceView>& devices,
				 std::unordered_map<XrPath, XrPath>& profiles)
{
	auto result = InputState::Validate(input);
	if (XR_FAILED(result))
		return result;
	std::vector<InputDevice> inputs;
	for (std::size_t hand = 0; hand < 2; ++hand)
		inputs.push_back({{InputDeviceKind::Controller, hand},
						  {hand == 0 ? "/user/hand/left" : "/user/hand/right"},
						  input.preferredProfiles[hand],
						  input.controllers[hand].connected,
						  true,
						  input.controllers[hand]});
	inputs.insert(inputs.end(), input.devices.begin(), input.devices.end());
	for (const auto& source : inputs)
	{
		DeviceView device{source.identity, {}, XR_NULL_PATH, source.connected, source.poseAvailable, source.controls};
		for (const auto& text : source.userPaths)
		{
			XrPath path;
			result = registry.StringToPath(text, path);
			if (XR_FAILED(result))
				return result;
			if (!registry.IsUserPath(path))
				return XR_ERROR_PATH_UNSUPPORTED;
			device.users.push_back(path);
		}
		if (device.connected)
			device.profile = registry.SelectProfile(source.preferredProfile, device.users.front());
		for (auto user : device.users)
			if (device.profile || !profiles.contains(user))
				profiles[user] = device.profile;
		devices.push_back(std::move(device));
	}
	return XR_SUCCESS;
}

ActionStateValue ReadSource(const ControllerControls& input, const ActionBinding& binding)
{
	ActionStateValue value;
	value.active = true;
	value.inactiveReason = InactiveReason::None;
	auto path = std::string_view(binding.component);
	if (path.starts_with("/input/trigger"))
	{
		value.scalar = input.trigger;
		value.boolean = path.ends_with("/touch") ? input.triggerTouch : input.trigger > 0.5f;
	}
	else if (path.starts_with("/input/squeeze"))
	{
		value.scalar = path.ends_with("/force") ? input.gripForce : input.grip;
		value.boolean = input.grip > 0.5f;
	}
	else if (path.starts_with("/input/thumbstick") || path.starts_with("/input/trackpad"))
	{
		bool stick = path.starts_with("/input/thumbstick");
		value.vector = stick ? input.stick : input.trackpad;
		value.scalar = path.ends_with("/x")	  ? value.vector.x
					   : path.ends_with("/y") ? value.vector.y
											  : input.trackpadForce;
		value.boolean = path.ends_with("/touch") ? (stick ? input.thumbTouch : input.trackpadTouch)
												 : (stick ? input.thumbClick : input.trackpadClick);
	}
	else if (path == "/input/a/click" || path == "/input/x/click" || path == "/input/select/click")
		value.boolean = input.primary;
	else if (path == "/input/a/touch" || path == "/input/x/touch")
		value.boolean = input.primaryTouch;
	else if (path == "/input/b/click" || path == "/input/y/click")
		value.boolean = input.secondary;
	else if (path == "/input/b/touch" || path == "/input/y/touch")
		value.boolean = input.secondaryTouch;
	else if (path == "/input/menu/click")
		value.boolean = input.menu;
	else if (path == "/input/back/click")
		value.boolean = input.back;
	else if (path == "/input/thumbrest/touch")
		value.boolean = input.thumbrestTouch;
	else if (path != "/input/grip/pose" && path != "/input/aim/pose" && path != "/input/gaze_ext/pose" &&
			 path != "/output/haptic")
	{
		value.active = false;
		value.inactiveReason = InactiveReason::Unbound;
	}
	if (binding.sourceType == XR_ACTION_TYPE_BOOLEAN_INPUT)
		value.scalar = value.boolean ? 1.0f : 0.0f;
	return value;
}

void Combine(ActionStateValue& target, const ActionStateValue& source)
{
	if (!source.active)
		return;
	target.active = true;
	target.inactiveReason = InactiveReason::None;
	target.boolean |= source.boolean;
	if (std::abs(source.scalar) > std::abs(target.scalar))
		target.scalar = source.scalar;
	if (source.vector.x * source.vector.x + source.vector.y * source.vector.y >
		target.vector.x * target.vector.x + target.vector.y * target.vector.y)
		target.vector = source.vector;
}

void Compare(ActionStateValue& value, const ActionStateValue& before, XrActionType type, RuntimeTime time)
{
	if (!value.active)
	{
		value.boolean = false;
		value.scalar = 0;
		value.vector = {};
		value.changed = false;
		value.changeTime = {};
		return;
	}
	bool different = type == XR_ACTION_TYPE_BOOLEAN_INPUT ? value.boolean != before.boolean
					 : type == XR_ACTION_TYPE_FLOAT_INPUT ? value.scalar != before.scalar
					 : type == XR_ACTION_TYPE_VECTOR2F_INPUT
						 ? value.vector.x != before.vector.x || value.vector.y != before.vector.y
						 : false;
	value.changed = before.active && different;
	value.changeTime = different || !before.active ? time : before.changeTime;
}
} // namespace

ActionRegistry::ActionRegistry(XrInstance owner, const EnabledExtensions& enabled) : m_owner(owner), m_enabled(enabled)
{
}

XrResult ActionRegistry::StringToPath(std::string_view text, XrPath& path)
{
	if (!ValidPath(text))
		return XR_ERROR_PATH_FORMAT_INVALID;
	std::lock_guard lock(m_mutex);
	auto found = m_pathIds.find(std::string(text));
	if (found != m_pathIds.end())
	{
		path = found->second;
		return XR_SUCCESS;
	}
	if (m_nextPath == std::numeric_limits<XrPath>::max())
		return XR_ERROR_PATH_COUNT_EXCEEDED;
	auto identity = m_nextPath++;
	m_paths.emplace(identity, text);
	m_pathIds.emplace(text, identity);
	path = identity;
	return XR_SUCCESS;
}

XrResult ActionRegistry::PathToString(XrPath path, std::uint32_t capacity, std::uint32_t* count, char* buffer) const
{
	if (!count || (capacity && !buffer))
		return XR_ERROR_VALIDATION_FAILURE;
	auto text = PathString(path);
	if (!text)
		return XR_ERROR_PATH_INVALID;
	*count = static_cast<std::uint32_t>(text->size() + 1);
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::memcpy(buffer, text->c_str(), *count);
	return XR_SUCCESS;
}

std::optional<std::string> ActionRegistry::PathString(XrPath path) const
{
	std::lock_guard lock(m_mutex);
	auto found = m_paths.find(path);
	return found == m_paths.end() ? std::nullopt : std::optional{found->second};
}

XrResult ActionRegistry::CreateSet(const XrActionSetCreateInfo& info, XrActionSet& handle)
{
	auto name = BoundedString(info.actionSetName);
	auto localized = BoundedString(info.localizedActionSetName);
	if (info.type != XR_TYPE_ACTION_SET_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	if (name.size() == sizeof(info.actionSetName) || !ValidName(name))
		return XR_ERROR_NAME_INVALID;
	if (localized.empty() || localized.size() == sizeof(info.localizedActionSetName) || !ValidUtf8(localized))
		return XR_ERROR_LOCALIZED_NAME_INVALID;
	std::lock_guard lock(m_mutex);
	for (auto existing : m_sets.Children(m_owner))
	{
		auto set = m_sets.Find(existing);
		if (!set->live)
			continue;
		if (set->name == name)
			return XR_ERROR_NAME_DUPLICATED;
		if (set->localizedName == localized)
			return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
	}
	auto set = m_sets.Create(m_owner,
							 [&](XrActionSet identity)
							 {
								 return std::make_unique<ActionSetDefinition>(ActionSetDefinition{
									 identity, std::string(name), std::string(localized), info.priority, false});
							 });
	handle = set;
	return XR_SUCCESS;
}

XrResult ActionRegistry::DestroySet(XrActionSet handle)
{
	std::lock_guard lock(m_mutex);
	auto set = m_sets.Find(handle, m_owner);
	if (!set || !set->live)
		return XR_ERROR_HANDLE_INVALID;
	for (auto action : m_actions.Children(handle))
		m_actions.Find(action)->live = false;
	set->live = false;
	return XR_SUCCESS;
}

XrResult ActionRegistry::CreateAction(XrActionSet setHandle, const XrActionCreateInfo& info, XrAction& handle)
{
	if (info.type != XR_TYPE_ACTION_CREATE_INFO || (info.countSubactionPaths && !info.subactionPaths))
		return XR_ERROR_VALIDATION_FAILURE;
	if (info.actionType != XR_ACTION_TYPE_BOOLEAN_INPUT && info.actionType != XR_ACTION_TYPE_FLOAT_INPUT &&
		info.actionType != XR_ACTION_TYPE_VECTOR2F_INPUT && info.actionType != XR_ACTION_TYPE_POSE_INPUT &&
		info.actionType != XR_ACTION_TYPE_VIBRATION_OUTPUT)
		return XR_ERROR_VALIDATION_FAILURE;
	auto name = BoundedString(info.actionName);
	auto localized = BoundedString(info.localizedActionName);
	if (name.size() == sizeof(info.actionName) || !ValidName(name))
		return XR_ERROR_NAME_INVALID;
	if (localized.empty() || localized.size() == sizeof(info.localizedActionName) || !ValidUtf8(localized))
		return XR_ERROR_LOCALIZED_NAME_INVALID;
	std::lock_guard lock(m_mutex);
	auto set = m_sets.Find(setHandle, m_owner);
	if (!set || !set->live)
		return XR_ERROR_HANDLE_INVALID;
	if (set->attached)
		return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
	std::set<XrPath> paths;
	for (std::uint32_t index = 0; index < info.countSubactionPaths; ++index)
	{
		auto path = m_paths.find(info.subactionPaths[index]);
		if (path == m_paths.end())
			return XR_ERROR_PATH_INVALID;
		if (!paths.insert(path->first).second)
			return XR_ERROR_PATH_UNSUPPORTED;
		bool recognized = UserAllowed(path->second, m_enabled);
		if (!recognized)
			return XR_ERROR_PATH_UNSUPPORTED;
	}
	for (auto existing : m_actions.Children(setHandle))
	{
		auto action = m_actions.Find(existing);
		if (!action->live)
			continue;
		if (action->name == name)
			return XR_ERROR_NAME_DUPLICATED;
		if (action->localizedName == localized)
			return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
	}
	auto action = m_actions.Create(setHandle,
								   [&](XrAction identity)
								   {
									   auto definition = std::make_unique<ActionDefinition>();
									   definition->handle = identity;
									   definition->set = setHandle;
									   definition->name = name;
									   definition->localizedName = localized;
									   definition->type = info.actionType;
									   if (info.countSubactionPaths)
										   definition->subactionPaths.assign(
											   info.subactionPaths, info.subactionPaths + info.countSubactionPaths);
									   return definition;
								   });
	handle = action;
	return XR_SUCCESS;
}

XrResult ActionRegistry::DestroyAction(XrAction handle)
{
	std::lock_guard lock(m_mutex);
	auto action = m_actions.Find(handle);
	if (!action || !action->live)
		return XR_ERROR_HANDLE_INVALID;
	action->live = false;
	return XR_SUCCESS;
}

std::optional<ActionDefinition> ActionRegistry::Action(XrAction handle) const
{
	auto resource = ActionResource(handle);
	return resource && resource->live ? resource : std::nullopt;
}

std::optional<ActionSetDefinition> ActionRegistry::Set(XrActionSet handle) const
{
	auto resource = SetResource(handle);
	return resource && resource->live ? resource : std::nullopt;
}

std::optional<ActionDefinition> ActionRegistry::ActionResource(XrAction handle) const
{
	std::lock_guard lock(m_mutex);
	auto action = m_actions.Find(handle);
	return action ? std::optional{*action.operator->()} : std::nullopt;
}

std::optional<ActionSetDefinition> ActionRegistry::SetResource(XrActionSet handle) const
{
	std::lock_guard lock(m_mutex);
	auto set = m_sets.Find(handle, m_owner);
	return set ? std::optional{*set.operator->()} : std::nullopt;
}

std::vector<ActionDefinition> ActionRegistry::Actions() const
{
	auto resources = Resources();
	std::erase_if(resources, [](const auto& resource) { return !resource.live; });
	return resources;
}

std::vector<ActionDefinition> ActionRegistry::Resources() const
{
	std::lock_guard lock(m_mutex);
	std::vector<ActionDefinition> result;
	for (auto set : m_sets.Children(m_owner))
		for (auto action : m_actions.Children(set))
			result.push_back(*m_actions.Find(action).operator->());
	std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
	return result;
}

const ActionSetDefinition* ActionInspectionDictionary::FindSet(XrActionSet handle) const noexcept
{
	const auto found = std::find_if(sets.begin(), sets.end(), [&](const auto& set) { return set.handle == handle; });
	return found == sets.end() ? nullptr : &*found;
}

const ActionDefinition* ActionInspectionDictionary::FindAction(XrAction handle) const noexcept
{
	const auto found =
		std::find_if(actions.begin(), actions.end(), [&](const auto& action) { return action.handle == handle; });
	return found == actions.end() ? nullptr : &*found;
}

std::optional<std::string_view> ActionInspectionDictionary::PathName(XrPath path) const noexcept
{
	if (path == XR_NULL_PATH)
		return std::string_view{};
	const auto found = std::find_if(paths.begin(), paths.end(), [&](const auto& value) { return value.path == path; });
	return found == paths.end() ? std::nullopt : std::optional<std::string_view>{found->name};
}

XrResult ActionRegistry::BuildInspectionDictionary(std::span<const XrActionSet> sets,
												   const ActionSyncSnapshot& snapshot,
												   std::shared_ptr<const ActionInspectionDictionary>& dictionary) const
{
	auto prepared = std::make_shared<ActionInspectionDictionary>();
	std::lock_guard lock(m_mutex);
	for (const auto handle : sets)
	{
		const auto set = m_sets.Find(handle, m_owner);
		if (!set)
			return XR_ERROR_HANDLE_INVALID;
		prepared->sets.push_back(*set.operator->());
		// Attach prepares its immutable record before mutating registry flags.
		prepared->sets.back().attached = true;
		for (const auto action : m_actions.Children(handle))
			prepared->actions.push_back(*m_actions.Find(action).operator->());
	}
	std::sort(prepared->sets.begin(), prepared->sets.end(),
			  [](const auto& left, const auto& right)
			  {
				  return left.name != right.name ? left.name < right.name
												 : std::less<XrActionSet>{}(left.handle, right.handle);
			  });
	const auto actionName = [&](XrAction handle)
	{
		const auto* action = prepared->FindAction(handle);
		const auto* set = action ? prepared->FindSet(action->set) : nullptr;
		return std::pair{set ? std::string_view{set->name} : std::string_view{},
						 action ? std::string_view{action->name} : std::string_view{}};
	};
	std::sort(prepared->actions.begin(), prepared->actions.end(),
			  [&](const auto& left, const auto& right)
			  {
				  const auto leftName =
					  std::pair{std::string_view{prepared->FindSet(left.set)->name}, std::string_view{left.name}};
				  const auto rightName =
					  std::pair{std::string_view{prepared->FindSet(right.set)->name}, std::string_view{right.name}};
				  return leftName != rightName ? leftName < rightName
											   : std::less<XrAction>{}(left.handle, right.handle);
			  });
	const auto appendBinding = [&](const ActionBinding& binding)
	{
		if (prepared->FindAction(binding.action) &&
			std::find(prepared->bindings.begin(), prepared->bindings.end(), binding) == prepared->bindings.end())
			prepared->bindings.push_back(binding);
	};
	for (const auto& binding : m_attachedBindings)
		appendBinding(binding);
	for (const auto& [profile, bindings] : m_bindings)
		for (const auto& binding : bindings)
		{
			const auto action = m_actions.Find(binding.action);
			if (action && !m_sets.Find(action->set)->attached)
				appendBinding(binding);
		}
	std::vector<XrPath> requiredPaths;
	const auto path = [&](XrPath value)
	{
		if (value != XR_NULL_PATH)
			requiredPaths.push_back(value);
	};
	const auto bindingPaths = [&](const ActionBinding& binding)
	{
		path(binding.profile);
		path(binding.source);
		path(binding.userPath);
	};
	for (const auto& action : prepared->actions)
		for (const auto subaction : action.subactionPaths)
			path(subaction);
	for (const auto& [user, profile] : snapshot.profiles)
	{
		path(user);
		path(profile);
	}
	for (const auto& active : snapshot.activeSets)
	{
		if (!prepared->FindSet(active.actionSet))
			return XR_ERROR_HANDLE_INVALID;
		path(active.subactionPath);
	}
	for (const auto& action : snapshot.actions)
	{
		if (!prepared->FindAction(action.definition.handle))
			return XR_ERROR_HANDLE_INVALID;
		for (const auto& [user, value] : action.values)
			path(user);
		for (const auto& binding : action.bindings)
		{
			if (binding.action != action.definition.handle)
				return XR_ERROR_HANDLE_INVALID;
			appendBinding(binding);
		}
		for (const auto& source : action.sources)
		{
			if (source.binding.action != action.definition.handle)
				return XR_ERROR_HANDLE_INVALID;
			appendBinding(source.binding);
			for (const auto user : source.userPaths)
				path(user);
		}
	}
	for (const auto& binding : prepared->bindings)
		bindingPaths(binding);
	std::sort(requiredPaths.begin(), requiredPaths.end());
	requiredPaths.erase(std::unique(requiredPaths.begin(), requiredPaths.end()), requiredPaths.end());
	for (const auto required : requiredPaths)
	{
		const auto found = m_paths.find(required);
		if (found == m_paths.end())
			return XR_ERROR_PATH_INVALID;
		prepared->paths.push_back({required, found->second});
	}
	std::sort(prepared->paths.begin(), prepared->paths.end(), [](const auto& left, const auto& right)
			  { return left.name != right.name ? left.name < right.name : left.path < right.path; });
	std::sort(prepared->bindings.begin(), prepared->bindings.end(),
			  [&](const auto& left, const auto& right)
			  {
				  const auto key = [&](const auto& binding)
				  {
					  return std::tuple{actionName(binding.action),			 *prepared->PathName(binding.profile),
										*prepared->PathName(binding.source), *prepared->PathName(binding.userPath),
										std::string_view{binding.component}, binding.sourceType};
				  };
				  const auto leftKey = key(left), rightKey = key(right);
				  return leftKey != rightKey ? leftKey < rightKey : std::less<XrAction>{}(left.action, right.action);
			  });
	dictionary = std::move(prepared);
	return XR_SUCCESS;
}

XrResult ActionRegistry::SuggestBindings(const XrInteractionProfileSuggestedBinding& info)
{
	if (info.type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING ||
		(info.countSuggestedBindings && !info.suggestedBindings))
		return XR_ERROR_VALIDATION_FAILURE;
	auto profileText = PathString(info.interactionProfile);
	if (!profileText)
		return XR_ERROR_PATH_INVALID;
	auto component = std::find_if(std::begin(s_components), std::end(s_components),
								  [&](const auto& entry) { return entry.profile == *profileText; });
	if (component == std::end(s_components) ||
		(!component->extension.empty() && !m_enabled.Contains(component->extension)))
		return XR_ERROR_PATH_UNSUPPORTED;
	std::vector<ActionBinding> prepared;
	for (std::uint32_t index = 0; index < info.countSuggestedBindings; ++index)
	{
		const auto& source = info.suggestedBindings[index];
		auto action = Action(source.action);
		if (!action)
			return XR_ERROR_HANDLE_INVALID;
		auto set = Set(action->set);
		if (!set)
			return XR_ERROR_HANDLE_INVALID;
		if (set->attached)
			return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
		auto path = PathString(source.binding);
		if (!path)
			return XR_ERROR_PATH_INVALID;
		std::string_view user;
		std::string_view componentPath;
		for (const auto& entry : s_components)
		{
			if (entry.profile != *profileText || entry.system)
				continue;
			if (path->size() == entry.user.size() + entry.component.size() && path->starts_with(entry.user) &&
				std::string_view(*path).substr(entry.user.size()) == entry.component)
			{
				user = entry.user;
				componentPath = entry.component;
				break;
			}
			if (*profileText == "/interaction_profiles/htc/vive_tracker_htcx" && path->ends_with(entry.component))
			{
				auto candidate = std::string_view(*path).substr(0, path->size() - entry.component.size());
				if (PersistentTrackerPath(candidate))
				{
					user = candidate;
					componentPath = entry.component;
					break;
				}
			}
		}
		auto match = std::find_if(std::begin(s_components), std::end(s_components), [&](const auto& entry)
								  { return entry.profile == *profileText && entry.component == componentPath; });
		if (user.empty() || match == std::end(s_components) || !Compatible(action->type, match->type))
			return XR_ERROR_PATH_UNSUPPORTED;
		XrPath userPath;
		auto result = StringToPath(user, userPath);
		if (XR_FAILED(result))
			return result;
		if (!action->subactionPaths.empty() && std::find(action->subactionPaths.begin(), action->subactionPaths.end(),
														 userPath) == action->subactionPaths.end())
			return XR_ERROR_PATH_UNSUPPORTED;
		prepared.push_back({source.action, info.interactionProfile, source.binding, userPath,
							std::string(componentPath), match->type});
	}
	std::lock_guard lock(m_mutex);
	for (const auto& binding : prepared)
	{
		auto action = m_actions.Find(binding.action);
		if (!action || !action->live)
			return XR_ERROR_HANDLE_INVALID;
		if (!m_sets.Find(action->set)->live)
			return XR_ERROR_HANDLE_INVALID;
		if (m_sets.Find(action->set)->attached)
			return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
	}
	m_bindings[info.interactionProfile] = std::move(prepared);
	if (std::find(m_profiles.begin(), m_profiles.end(), info.interactionProfile) == m_profiles.end())
		m_profiles.push_back(info.interactionProfile);
	return XR_SUCCESS;
}

XrResult ActionRegistry::Attach(std::span<const XrActionSet> sets)
{
	std::lock_guard lock(m_mutex);
	std::set<XrActionSet> unique;
	for (auto handle : sets)
		if (!m_sets.Find(handle, m_owner) || !m_sets.Find(handle, m_owner)->live)
			return XR_ERROR_HANDLE_INVALID;
		else if (!unique.insert(handle).second)
			return XR_ERROR_VALIDATION_FAILURE;
	// Later suggestions for unattached resources must not replace bindings
	// already attached by another session, including retired action resources.
	auto retained = m_attachedBindings;
	for (const auto& [profile, bindings] : m_bindings)
		for (const auto& binding : bindings)
		{
			const auto action = m_actions.Find(binding.action);
			if (action && !m_sets.Find(action->set)->attached && unique.contains(action->set))
				retained.push_back(binding);
		}
	m_attachedBindings = std::move(retained);
	for (auto handle : sets)
		m_sets.Find(handle)->attached = true;
	return XR_SUCCESS;
}

std::vector<ActionBinding> ActionRegistry::Bindings(XrAction action, XrPath profile) const
{
	std::lock_guard lock(m_mutex);
	std::vector<ActionBinding> result;
	const auto resource = m_actions.Find(action);
	if (resource && m_sets.Find(resource->set)->attached)
	{
		for (const auto& binding : m_attachedBindings)
			if (binding.action == action && binding.profile == profile)
				result.push_back(binding);
		return result;
	}
	const auto bindings = m_bindings.find(profile);
	if (bindings != m_bindings.end())
		for (const auto& binding : bindings->second)
			if (binding.action == action)
				result.push_back(binding);
	return result;
}

XrPath ActionRegistry::SelectProfile(std::string_view preferred, XrPath userPath) const
{
	std::lock_guard lock(m_mutex);
	auto user = m_paths.find(userPath);
	auto compatible = [&](XrPath profile)
	{
		if (!userPath)
			return true;
		if (user == m_paths.end())
			return false;
		return std::any_of(std::begin(s_components), std::end(s_components),
						   [&](const auto& entry)
						   {
							   return entry.profile == m_paths.at(profile) &&
									  (entry.user == user->second ||
									   (entry.profile == "/interaction_profiles/htc/vive_tracker_htcx" &&
										PersistentTrackerPath(user->second)));
						   });
	};
	for (auto profile : m_profiles)
		if (m_paths.at(profile) == preferred && compatible(profile))
			return profile;
	for (auto profile : m_profiles)
		if (compatible(profile))
			return profile;
	return XR_NULL_PATH;
}

bool ActionRegistry::IsUserPath(XrPath path) const
{
	auto text = PathString(path);
	return text && UserAllowed(*text, m_enabled);
}

std::string ActionRegistry::ProfileTitle(XrPath profile) const
{
	auto text = PathString(profile);
	for (const auto& component : s_components)
		if (text && component.profile == *text)
			return std::string(component.title);
	return {};
}

XrResult ActionRegistry::LocalizedName(XrPath source, XrPath profile, XrInputSourceLocalizedNameFlags flags,
									   std::string& name) const
{
	constexpr auto knownFlags = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
								XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT |
								XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
	if (!flags || (flags & ~knownFlags))
		return XR_ERROR_VALIDATION_FAILURE;
	auto path = PathString(source);
	auto profilePath = PathString(profile);
	if (!path)
		return XR_ERROR_PATH_INVALID;
	if (!profilePath)
		return XR_ERROR_PATH_UNSUPPORTED;
	auto component = std::find_if(
		std::begin(s_components), std::end(s_components),
		[&](const auto& entry)
		{
			return entry.profile == *profilePath &&
				   ((path->starts_with(entry.user) &&
					 std::string_view(*path).substr(entry.user.size()) == entry.component) ||
					(entry.profile == "/interaction_profiles/htc/vive_tracker_htcx" &&
					 path->ends_with(entry.component) &&
					 PersistentTrackerPath(std::string_view(*path).substr(0, path->size() - entry.component.size()))));
		});
	if (component == std::end(s_components))
		return XR_ERROR_PATH_UNSUPPORTED;
	name.clear();
	auto append = [&](std::string_view value)
	{
		if (!name.empty())
			name += " / ";
		name += value;
	};
	if (flags & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT)
		append(path->starts_with("/user/hand/left/")	? "Left Hand"
			   : path->starts_with("/user/hand/right/") ? "Right Hand"
			   : path->starts_with("/user/eyes_ext/")
				   ? "Eye Gaze"
				   : std::string_view(*path).substr(0, path->size() - component->component.size()));
	if (flags & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT)
		append(component->title);
	if (flags & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT)
		append(component->component.substr(component->component.find('/', 1) + 1));
	return XR_SUCCESS;
}

XrResult ActionRegistry::ValidateSubaction(const ActionDefinition& action, XrPath path) const
{
	if (path == XR_NULL_PATH)
		return XR_SUCCESS;
	if (!PathString(path))
		return XR_ERROR_PATH_INVALID;
	return std::find(action.subactionPaths.begin(), action.subactionPaths.end(), path) == action.subactionPaths.end()
			   ? XR_ERROR_PATH_UNSUPPORTED
			   : XR_SUCCESS;
}

std::optional<std::size_t> ActionRegistry::Hand(XrPath path) const
{
	auto text = PathString(path);
	if (text == "/user/hand/left")
		return 0;
	if (text == "/user/hand/right")
		return 1;
	return {};
}

ActionSyncState::ActionSyncState(ActionRegistry& registry) : m_registry(registry) {}

XrResult ActionSyncState::Attach(const XrSessionActionSetsAttachInfo& info, const InputSnapshot& input)
{
	if (info.type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO || !info.countActionSets || !info.actionSets)
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(m_mutex);
	if (m_attached)
		return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
	std::vector<DeviceView> devices;
	std::unordered_map<XrPath, XrPath> profiles;
	auto result = Devices(m_registry, input, devices, profiles);
	if (XR_FAILED(result))
		return result;
	std::vector<XrActionSet> prepared(info.actionSets, info.actionSets + info.countActionSets);
	ActionSyncSnapshot next;
	for (auto definition : m_registry.Resources())
	{
		if (std::find(prepared.begin(), prepared.end(), definition.set) == prepared.end())
			continue;
		ActionSnapshot action;
		action.definition = std::move(definition);
		for (const auto& device : devices)
			for (auto binding : m_registry.Bindings(action.definition.handle, device.profile))
				if (std::find(device.users.begin(), device.users.end(), binding.userPath) != device.users.end())
				{
					action.sources.push_back({device.identity, binding, device.users, false});
					action.bindings.push_back(std::move(binding));
				}
		next.actions.push_back(std::move(action));
	}
	result = m_registry.BuildInspectionDictionary(prepared, next, next.dictionary);
	if (XR_FAILED(result))
		return result;
	for (auto& action : next.actions)
		action.definition = *next.dictionary->FindAction(action.definition.handle);
	// All snapshot allocation precedes registry attachment and publication.
	result = m_registry.Attach(prepared);
	if (XR_FAILED(result))
		return result;
	m_sets = std::move(prepared);
	m_snapshot = std::move(next);
	m_attached = true;
	return XR_SUCCESS;
}

XrResult ActionSyncState::Sync(const XrActionsSyncInfo& info, const InputSnapshot& input, bool focused,
							   bool* profilesChanged)
{
	if (info.type != XR_TYPE_ACTIONS_SYNC_INFO || (info.countActiveActionSets && !info.activeActionSets))
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(m_mutex);
	if (!m_attached)
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	ActionSyncSnapshot next;
	next.serial = m_snapshot.serial + 1;
	next.time = input.time;
	next.focused = focused;
	next.inputRevision = input.revision;
	if (info.countActiveActionSets)
		next.activeSets.assign(info.activeActionSets, info.activeActionSets + info.countActiveActionSets);
	auto definitions = m_registry.Resources();
	for (const auto& active : next.activeSets)
	{
		if (!m_registry.Set(active.actionSet))
			return XR_ERROR_HANDLE_INVALID;
		if (std::find(m_sets.begin(), m_sets.end(), active.actionSet) == m_sets.end())
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		if (active.subactionPath)
		{
			if (!m_registry.PathString(active.subactionPath))
				return XR_ERROR_PATH_INVALID;
			bool used = std::any_of(definitions.begin(), definitions.end(),
									[&](const auto& action)
									{
										return action.set == active.actionSet &&
											   std::find(action.subactionPaths.begin(), action.subactionPaths.end(),
														 active.subactionPath) != action.subactionPaths.end();
									});
			if (!used)
				return XR_ERROR_PATH_UNSUPPORTED;
		}
	}
	std::vector<DeviceView> devices;
	auto deviceResult = Devices(m_registry, input, devices, next.profiles);
	if (XR_FAILED(deviceResult))
		return deviceResult;
	std::unordered_map<std::string, std::uint32_t> priorities;
	for (const auto& definition : definitions)
	{
		if (std::find(m_sets.begin(), m_sets.end(), definition.set) == m_sets.end())
			continue;
		ActionSnapshot action;
		action.definition = definition;
		for (const auto& device : devices)
			for (auto binding : m_registry.Bindings(definition.handle, device.profile))
			{
				if (std::find(device.users.begin(), device.users.end(), binding.userPath) == device.users.end())
					continue;
				action.bindings.push_back(binding);
				action.sources.push_back({device.identity, binding, device.users, false});
				if (focused && device.connected &&
					(definition.type != XR_ACTION_TYPE_POSE_INPUT || device.poseAvailable) &&
					SetSelected(definition, device, next.activeSets))
				{
					auto priority = m_registry.SetResource(definition.set)->priority;
					auto& highest = priorities[SourceKey(binding, device.identity)];
					highest = std::max(highest, priority);
				}
			}
		next.actions.push_back(std::move(action));
	}
	for (auto& action : next.actions)
	{
		ActionStateValue aggregate;
		aggregate.inactiveReason = focused ? InactiveReason::Unbound : InactiveReason::NotFocused;
		for (const auto& device : devices)
		{
			ActionStateValue value;
			value.inactiveReason = !focused ? InactiveReason::NotFocused
								   : !SetSelected(action.definition, device, next.activeSets)
									   ? InactiveReason::InactiveSet
								   : !device.connected ? InactiveReason::Disconnected
								   : (action.definition.type == XR_ACTION_TYPE_POSE_INPUT && !device.poseAvailable)
									   ? InactiveReason::Unavailable
									   : InactiveReason::Unbound;
			if (value.inactiveReason == InactiveReason::Unbound)
				for (auto& source : action.sources)
				{
					if (source.device != device.identity)
						continue;
					if (priorities[SourceKey(source.binding, device.identity)] >
						m_registry.SetResource(action.definition.set)->priority)
					{
						value.inactiveReason = InactiveReason::HigherPriority;
						continue;
					}
					auto inputValue = ReadSource(device.controls, source.binding);
					if (action.definition.type == XR_ACTION_TYPE_POSE_INPUT && !device.poseAvailable)
						inputValue = {};
					if (action.definition.type == XR_ACTION_TYPE_BOOLEAN_INPUT &&
						source.binding.sourceType == XR_ACTION_TYPE_FLOAT_INPUT)
						inputValue.boolean = inputValue.scalar > 0.5f;
					source.active = inputValue.active;
					Combine(value, inputValue);
				}
			for (auto user : device.users)
			{
				auto [stored, inserted] = action.values.try_emplace(user, value);
				if (!inserted)
					Combine(stored->second, value);
			}
			Combine(aggregate, value);
		}
		action.values[XR_NULL_PATH] = aggregate;
		auto previous = std::find_if(m_snapshot.actions.begin(), m_snapshot.actions.end(), [&](const auto& before)
									 { return before.definition.handle == action.definition.handle; });
		for (auto& [path, value] : action.values)
		{
			ActionStateValue before;
			if (previous != m_snapshot.actions.end())
				if (auto old = previous->values.find(path); old != previous->values.end())
					before = old->second;
			Compare(value, before, action.definition.type, input.time);
		}
	}
	const auto dictionaryResult = m_registry.BuildInspectionDictionary(m_sets, next, next.dictionary);
	if (XR_FAILED(dictionaryResult))
		return dictionaryResult;
	for (auto& action : next.actions)
		action.definition = *next.dictionary->FindAction(action.definition.handle);
	if (m_snapshot.dictionary && *m_snapshot.dictionary == *next.dictionary)
		next.dictionary = m_snapshot.dictionary;
	const bool changed = next.profiles != m_snapshot.profiles;
	m_snapshot = std::move(next);
	m_queryFocused = focused;
	if (profilesChanged)
		*profilesChanged = changed;
	return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XrResult ActionSyncState::Value(const XrActionStateGetInfo& info, XrActionType type, ActionStateValue& value,
								bool retained) const
{
	if (info.type != XR_TYPE_ACTION_STATE_GET_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	auto definition = retained ? m_registry.ActionResource(info.action) : m_registry.Action(info.action);
	if (!definition)
		return XR_ERROR_HANDLE_INVALID;
	if (definition->type != type)
		return XR_ERROR_ACTION_TYPE_MISMATCH;
	auto result = m_registry.ValidateSubaction(*definition, info.subactionPath);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(m_mutex);
	if (std::find(m_sets.begin(), m_sets.end(), definition->set) == m_sets.end())
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	auto action = std::find_if(m_snapshot.actions.begin(), m_snapshot.actions.end(),
							   [&](const auto& state) { return state.definition.handle == info.action; });
	value = {};
	if (action != m_snapshot.actions.end())
	{
		if (auto found = action->values.find(info.subactionPath); found != action->values.end())
			value = found->second;
	}
	if (!m_queryFocused)
	{
		value = {};
		value.inactiveReason = InactiveReason::NotFocused;
	}
	return XR_SUCCESS;
}

XrResult ActionSyncState::GetBoolean(const XrActionStateGetInfo& info, XrActionStateBoolean& state) const
{
	if (state.type != XR_TYPE_ACTION_STATE_BOOLEAN)
		return XR_ERROR_VALIDATION_FAILURE;
	ActionStateValue value;
	auto result = Value(info, XR_ACTION_TYPE_BOOLEAN_INPUT, value);
	if (XR_FAILED(result))
		return result;
	state.isActive = value.active;
	state.currentState = value.boolean;
	state.changedSinceLastSync = value.changed;
	state.lastChangeTime = value.changeTime.value.count();
	return XR_SUCCESS;
}

XrResult ActionSyncState::GetFloat(const XrActionStateGetInfo& info, XrActionStateFloat& state) const
{
	if (state.type != XR_TYPE_ACTION_STATE_FLOAT)
		return XR_ERROR_VALIDATION_FAILURE;
	ActionStateValue value;
	auto result = Value(info, XR_ACTION_TYPE_FLOAT_INPUT, value);
	if (XR_FAILED(result))
		return result;
	state.isActive = value.active;
	state.currentState = value.scalar;
	state.changedSinceLastSync = value.changed;
	state.lastChangeTime = value.changeTime.value.count();
	return XR_SUCCESS;
}

XrResult ActionSyncState::GetVector(const XrActionStateGetInfo& info, XrActionStateVector2f& state) const
{
	if (state.type != XR_TYPE_ACTION_STATE_VECTOR2F)
		return XR_ERROR_VALIDATION_FAILURE;
	ActionStateValue value;
	auto result = Value(info, XR_ACTION_TYPE_VECTOR2F_INPUT, value);
	if (XR_FAILED(result))
		return result;
	state.isActive = value.active;
	state.currentState = value.vector;
	state.changedSinceLastSync = value.changed;
	state.lastChangeTime = value.changeTime.value.count();
	return XR_SUCCESS;
}

XrResult ActionSyncState::GetPose(const XrActionStateGetInfo& info, XrActionStatePose& state) const
{
	if (state.type != XR_TYPE_ACTION_STATE_POSE)
		return XR_ERROR_VALIDATION_FAILURE;
	ActionStateValue value;
	auto result = Value(info, XR_ACTION_TYPE_POSE_INPUT, value);
	if (XR_FAILED(result))
		return result;
	state.isActive = value.active;
	return XR_SUCCESS;
}

XrResult ActionSyncState::OutputDevices(const XrHapticActionInfo& info, std::vector<std::size_t>& devices) const
{
	std::vector<InputDeviceId> targets;
	auto result = OutputTargets(info, targets);
	if (XR_FAILED(result))
		return result;
	devices.clear();
	for (auto target : targets)
		if (target.kind == InputDeviceKind::Controller)
			devices.push_back(static_cast<std::size_t>(target.value));
	return XR_SUCCESS;
}

XrResult ActionSyncState::OutputTargets(const XrHapticActionInfo& info, std::vector<InputDeviceId>& devices) const
{
	if (info.type != XR_TYPE_HAPTIC_ACTION_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, info.action, info.subactionPath};
	ActionStateValue ignored;
	auto result = Value(get, XR_ACTION_TYPE_VIBRATION_OUTPUT, ignored);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(m_mutex);
	devices.clear();
	if (!m_queryFocused)
		return XR_SUCCESS;
	for (const auto& action : m_snapshot.actions)
		if (action.definition.handle == info.action)
			for (const auto& source : action.sources)
				if (source.active &&
					(!info.subactionPath || std::find(source.userPaths.begin(), source.userPaths.end(),
													  info.subactionPath) != source.userPaths.end()) &&
					std::find(devices.begin(), devices.end(), source.device) == devices.end())
					devices.push_back(source.device);
	return XR_SUCCESS;
}

XrResult ActionSyncState::PoseSources(XrAction handle, XrPath subaction,
									  std::vector<ResolvedActionSource>& sources) const
{
	return Sources(handle, subaction, sources, false);
}

XrResult ActionSyncState::SpaceSources(XrAction handle, XrPath subaction,
									   std::vector<ResolvedActionSource>& sources) const
{
	return Sources(handle, subaction, sources, true);
}

XrResult ActionSyncState::Sources(XrAction handle, XrPath subaction, std::vector<ResolvedActionSource>& sources,
								  bool retained) const
{
	XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, handle, subaction};
	ActionStateValue ignored;
	auto result = Value(info, XR_ACTION_TYPE_POSE_INPUT, ignored, retained);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(m_mutex);
	sources.clear();
	if (!m_queryFocused)
		return XR_SUCCESS;
	for (const auto& action : m_snapshot.actions)
		if (action.definition.handle == handle)
			for (const auto& source : action.sources)
				if (source.active && source.binding.sourceType == XR_ACTION_TYPE_POSE_INPUT &&
					(!subaction ||
					 std::find(source.userPaths.begin(), source.userPaths.end(), subaction) != source.userPaths.end()))
					sources.push_back(source);
	return XR_SUCCESS;
}

XrResult ActionSyncState::BoundSources(XrAction handle, std::vector<XrPath>& paths) const
{
	auto definition = m_registry.Action(handle);
	if (!definition)
		return XR_ERROR_HANDLE_INVALID;
	std::lock_guard lock(m_mutex);
	if (std::find(m_sets.begin(), m_sets.end(), definition->set) == m_sets.end())
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	paths.clear();
	for (const auto& action : m_snapshot.actions)
		if (action.definition.handle == handle)
			for (const auto& binding : action.bindings)
				if (std::find(paths.begin(), paths.end(), binding.source) == paths.end())
					paths.push_back(binding.source);
	return XR_SUCCESS;
}

XrResult ActionSyncState::CurrentProfile(XrPath userPath, XrPath& profile) const
{
	if (!m_registry.PathString(userPath))
		return XR_ERROR_PATH_INVALID;
	if (!m_registry.IsUserPath(userPath))
		return XR_ERROR_PATH_UNSUPPORTED;
	std::lock_guard lock(m_mutex);
	if (!m_attached)
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	auto found = m_snapshot.profiles.find(userPath);
	profile = found == m_snapshot.profiles.end() ? XR_NULL_PATH : found->second;
	return XR_SUCCESS;
}

ActionSyncSnapshot ActionSyncState::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

void ActionSyncState::Reset()
{
	std::lock_guard lock(m_mutex);
	m_snapshot.serial = 0;
	m_snapshot.time = {};
	m_snapshot.focused = false;
	m_snapshot.inputRevision = 0;
	m_snapshot.activeSets.clear();
	for (auto& action : m_snapshot.actions)
	{
		action.values.clear();
		for (auto& source : action.sources)
			source.active = false;
	}
	m_queryFocused = false;
}

void ActionSyncState::SetFocused(bool focused)
{
	std::lock_guard lock(m_mutex);
	m_queryFocused = focused;
}
} // namespace simxr
