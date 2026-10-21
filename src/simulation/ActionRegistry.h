#pragma once

#include "Types.h"
#include "runtime/ExtensionRegistry.h"
#include "runtime/HandleRegistry.h"
#include "simulation/InputState.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace simxr
{
struct ActionSetDefinition
{
	XrActionSet handle = XR_NULL_HANDLE;
	std::string name;
	std::string localizedName;
	std::uint32_t priority = 0;
	bool attached = false;
	bool live = true;
	bool operator==(const ActionSetDefinition&) const = default;
};

struct ActionDefinition
{
	XrAction handle = XR_NULL_HANDLE;
	XrActionSet set = XR_NULL_HANDLE;
	XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
	std::string name;
	std::string localizedName;
	std::vector<XrPath> subactionPaths;
	bool live = true;
	bool operator==(const ActionDefinition&) const = default;
};

struct ActionBinding
{
	XrAction action;
	XrPath profile;
	XrPath source;
	XrPath userPath;
	std::string component;
	XrActionType sourceType;
	bool operator==(const ActionBinding&) const = default;
};

struct ActionPathName
{
	XrPath path = XR_NULL_PATH;
	std::string name;
	bool operator==(const ActionPathName&) const = default;
};

// Immutable, owned interpretation of one attached action configuration. Handles
// are joins within this record, not live registry lookups or wire identities.
struct ActionInspectionDictionary
{
	std::vector<ActionSetDefinition> sets;
	std::vector<ActionDefinition> actions;
	std::vector<ActionBinding> bindings;
	std::vector<ActionPathName> paths;
	const ActionSetDefinition* FindSet(XrActionSet handle) const noexcept;
	const ActionDefinition* FindAction(XrAction handle) const noexcept;
	// XR_NULL_PATH resolves to an empty string sentinel, never a registry query.
	std::optional<std::string_view> PathName(XrPath path) const noexcept;
	bool operator==(const ActionInspectionDictionary&) const = default;
};

struct ActionSyncSnapshot;

class ActionRegistry
{
  public:
	ActionRegistry(XrInstance owner, const EnabledExtensions& enabled);
	XrResult StringToPath(std::string_view text, XrPath& path);
	XrResult PathToString(XrPath path, std::uint32_t capacity, std::uint32_t* count, char* buffer) const;
	std::optional<std::string> PathString(XrPath path) const;
	XrResult CreateSet(const XrActionSetCreateInfo& info, XrActionSet& handle);
	XrResult DestroySet(XrActionSet handle);
	XrResult CreateAction(XrActionSet set, const XrActionCreateInfo& info, XrAction& handle);
	XrResult DestroyAction(XrAction handle);
	std::optional<ActionDefinition> Action(XrAction handle) const;
	std::optional<ActionSetDefinition> Set(XrActionSet handle) const;
	std::vector<ActionDefinition> Actions() const;
	std::optional<ActionDefinition> ActionResource(XrAction handle) const;
	std::optional<ActionSetDefinition> SetResource(XrActionSet handle) const;
	std::vector<ActionDefinition> Resources() const;
	XrResult BuildInspectionDictionary(std::span<const XrActionSet> sets, const ActionSyncSnapshot& snapshot,
									   std::shared_ptr<const ActionInspectionDictionary>& dictionary) const;
	XrResult SuggestBindings(const XrInteractionProfileSuggestedBinding& info);
	XrResult Attach(std::span<const XrActionSet> sets);
	std::vector<ActionBinding> Bindings(XrAction action, XrPath profile) const;
	XrPath SelectProfile(std::string_view preferred, XrPath userPath = XR_NULL_PATH) const;
	bool IsUserPath(XrPath path) const;
	std::string ProfileTitle(XrPath profile) const;
	XrResult LocalizedName(XrPath source, XrPath profile, XrInputSourceLocalizedNameFlags flags,
						   std::string& name) const;
	XrResult ValidateSubaction(const ActionDefinition& action, XrPath path) const;
	std::optional<std::size_t> Hand(XrPath path) const;

  private:
	XrInstance m_owner;
	const EnabledExtensions& m_enabled;
	mutable std::mutex m_mutex;
	HandleRegistry<XrActionSet, ActionSetDefinition, XrInstance> m_sets;
	HandleRegistry<XrAction, ActionDefinition, XrActionSet> m_actions;
	std::unordered_map<XrPath, std::string> m_paths;
	std::unordered_map<std::string, XrPath> m_pathIds;
	XrPath m_nextPath = 1;
	std::vector<XrPath> m_profiles;
	std::unordered_map<XrPath, std::vector<ActionBinding>> m_bindings;
	std::vector<ActionBinding> m_attachedBindings;
};

enum class InactiveReason
{
	None,
	NotSynchronized,
	InactiveSet,
	NotFocused,
	Unbound,
	Disconnected,
	HigherPriority,
	Unavailable
};

struct ActionStateValue
{
	bool active = false;
	bool boolean = false;
	float scalar = 0;
	XrVector2f vector{};
	bool changed = false;
	RuntimeTime changeTime;
	InactiveReason inactiveReason = InactiveReason::NotSynchronized;
};

struct ResolvedActionSource
{
	InputDeviceId device;
	ActionBinding binding;
	std::vector<XrPath> userPaths;
	bool active = false;
};

struct ActionSnapshot
{
	ActionDefinition definition;
	std::unordered_map<XrPath, ActionStateValue> values;
	std::vector<ResolvedActionSource> sources;
	std::vector<ActionBinding> bindings;
};

struct ActionSyncSnapshot
{
	std::uint64_t serial = 0;
	RuntimeTime time;
	bool focused = false;
	std::uint64_t inputRevision = 0;
	std::unordered_map<XrPath, XrPath> profiles;
	std::vector<XrActiveActionSet> activeSets;
	std::vector<ActionSnapshot> actions;
	std::shared_ptr<const ActionInspectionDictionary> dictionary;
};

class ActionSyncState
{
  public:
	explicit ActionSyncState(ActionRegistry& registry);
	XrResult Attach(const XrSessionActionSetsAttachInfo& info, const InputSnapshot& input = {});
	XrResult Sync(const XrActionsSyncInfo& info, const InputSnapshot& input, bool focused);
	XrResult GetBoolean(const XrActionStateGetInfo& info, XrActionStateBoolean& state) const;
	XrResult GetFloat(const XrActionStateGetInfo& info, XrActionStateFloat& state) const;
	XrResult GetVector(const XrActionStateGetInfo& info, XrActionStateVector2f& state) const;
	XrResult GetPose(const XrActionStateGetInfo& info, XrActionStatePose& state) const;
	XrResult OutputDevices(const XrHapticActionInfo& info, std::vector<std::size_t>& devices) const;
	XrResult OutputTargets(const XrHapticActionInfo& info, std::vector<InputDeviceId>& devices) const;
	XrResult PoseSources(XrAction action, XrPath subaction, std::vector<ResolvedActionSource>& sources) const;
	XrResult SpaceSources(XrAction action, XrPath subaction, std::vector<ResolvedActionSource>& sources) const;
	XrResult BoundSources(XrAction action, std::vector<XrPath>& paths) const;
	XrResult CurrentProfile(XrPath userPath, XrPath& profile) const;
	ActionSyncSnapshot Snapshot() const;
	void SetFocused(bool focused);
	void Reset();

  private:
	XrResult Value(const XrActionStateGetInfo& info, XrActionType type, ActionStateValue& value,
				   bool retained = false) const;
	XrResult Sources(XrAction action, XrPath subaction, std::vector<ResolvedActionSource>& sources,
					 bool retained) const;
	ActionRegistry& m_registry;
	mutable std::mutex m_mutex;
	bool m_attached = false;
	bool m_queryFocused = false;
	std::vector<XrActionSet> m_sets;
	ActionSyncSnapshot m_snapshot;
};
} // namespace simxr
