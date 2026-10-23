#include "../TestSupport.h"
#include "simulation/ActionRegistry.h"
#include "simulation/InputState.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SIMXR_TEST_TSAN_ALLOCATOR 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define SIMXR_TEST_TSAN_ALLOCATOR 1
#endif

#ifndef SIMXR_TEST_TSAN_ALLOCATOR
namespace
{
thread_local int failAllocationAfter = -1;
struct AllocationFailure
{
	explicit AllocationFailure(int after)
	{
		failAllocationAfter = after;
	}
	~AllocationFailure()
	{
		failAllocationAfter = -1;
	}
};
} // namespace

void* operator new(std::size_t size)
{
	if (failAllocationAfter == 0)
	{
		failAllocationAfter = -1;
		throw std::bad_alloc{};
	}
	if (failAllocationAfter > 0)
		--failAllocationAfter;
	if (void* result = std::malloc(size ? size : 1))
		return result;
	throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
	return ::operator new(size);
}
void operator delete(void* memory) noexcept
{
	std::free(memory);
}
void operator delete[](void* memory) noexcept
{
	std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
	std::free(memory);
}

#endif // TSan owns the global allocator interceptors.

using namespace simxr;

XrPath Path(ActionRegistry& registry, const char* text)
{
	XrPath path;
	Require(registry.StringToPath(text, path) == XR_SUCCESS, "Create path");
	return path;
}

XrActionSet Set(ActionRegistry& registry, const char* name, std::uint32_t priority = 0)
{
	XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(info.actionSetName, name);
	std::strcpy(info.localizedActionSetName, name);
	info.priority = priority;
	XrActionSet set;
	Require(registry.CreateSet(info, set) == XR_SUCCESS, "Create action set");
	return set;
}

XrAction Action(ActionRegistry& registry, XrActionSet set, const char* name, XrActionType type,
				std::span<const XrPath> paths)
{
	XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
	std::strcpy(info.actionName, name);
	std::strcpy(info.localizedActionName, name);
	info.actionType = type;
	info.countSubactionPaths = static_cast<std::uint32_t>(paths.size());
	info.subactionPaths = paths.data();
	XrAction action;
	Require(registry.CreateAction(set, info, action) == XR_SUCCESS, "Create action");
	return action;
}

void CheckDictionary(const ActionSyncSnapshot& snapshot)
{
	Require(snapshot.dictionary != nullptr, "Snapshot lacks an owned interpretation dictionary");
	const auto& dictionary = *snapshot.dictionary;
	Require(dictionary.PathName(XR_NULL_PATH) == "", "Null path lacks an explicit dictionary sentinel");
	for (const auto& action : snapshot.actions)
	{
		Require(dictionary.FindAction(action.definition.handle) &&
					*dictionary.FindAction(action.definition.handle) == action.definition &&
					dictionary.FindSet(action.definition.set),
				"Action definition/set cannot be joined using its own dictionary");
		for (const auto path : action.definition.subactionPaths)
			Require(dictionary.PathName(path).has_value(), "Subaction path is absent from dictionary");
		for (const auto& [path, value] : action.values)
			Require(dictionary.PathName(path).has_value(), "Value path is absent from dictionary");
		for (const auto& source : action.sources)
			for (const auto path : source.userPaths)
				Require(dictionary.PathName(path).has_value(), "Resolved-source user path is absent from dictionary");
	}
	for (const auto& binding : dictionary.bindings)
		Require(dictionary.FindAction(binding.action) && dictionary.PathName(binding.profile) &&
					dictionary.PathName(binding.source) && dictionary.PathName(binding.userPath),
				"Binding cannot be interpreted without the live registry");
	for (const auto& [user, profile] : snapshot.profiles)
		Require(dictionary.PathName(user) && dictionary.PathName(profile),
				"Profile map path is absent from dictionary");
	for (const auto& active : snapshot.activeSets)
		Require(dictionary.FindSet(active.actionSet) && dictionary.PathName(active.subactionPath),
				"Active-set filter is absent from dictionary");
	Require(std::is_sorted(dictionary.paths.begin(), dictionary.paths.end(),
						   [](const auto& left, const auto& right) { return left.name < right.name; }),
			"Path dictionary order is nondeterministic");
}

void HistoricalDictionaryAndFrozenBindings()
{
	ActionSyncSnapshot retained;
	XrAction retainedAction;
	XrActionSet retainedSet;
	XrPath left, right, oldSource, profile;
	{
		EnabledExtensions enabled;
		ActionRegistry registry(reinterpret_cast<XrInstance>(50), enabled);
		left = Path(registry, "/user/hand/left");
		right = Path(registry, "/user/hand/right");
		const std::array subactions{left, right};
		retainedSet = Set(registry, "historical", 42);
		const auto empty = Set(registry, "empty", 7);
		const auto unrelated = Set(registry, "unrelated", 99);
		const auto unrelatedPath = Path(registry, "/unrelated/path");
		const auto unrelatedAction = Action(registry, unrelated, "unattached", XR_ACTION_TYPE_POSE_INPUT, subactions);
		retainedAction = Action(registry, retainedSet, "select", XR_ACTION_TYPE_BOOLEAN_INPUT, subactions);
		profile = Path(registry, "/interaction_profiles/oculus/touch_controller");
		oldSource = Path(registry, "/user/hand/right/input/a/click");
		const auto simple = Path(registry, "/interaction_profiles/khr/simple_controller");
		const auto simpleSource = Path(registry, "/user/hand/right/input/select/click");
		const XrActionSuggestedBinding original{retainedAction, oldSource};
		const XrActionSuggestedBinding alternate{retainedAction, simpleSource};
		const XrInteractionProfileSuggestedBinding first{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														 profile, 1, &original};
		const XrInteractionProfileSuggestedBinding second{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														  simple, 1, &alternate};
		Require(registry.SuggestBindings(first) == XR_SUCCESS && registry.SuggestBindings(second) == XR_SUCCESS,
				"Prepare two historical profile bindings");
		ActionSyncState state(registry);
		const std::array sets{retainedSet, empty};
		const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 2, sets.data()};
		Require(state.Attach(attach) == XR_SUCCESS, "Attach historical resources");
		const auto attached = state.Snapshot();
		CheckDictionary(attached);
		Require(attached.dictionary->sets.size() == 2 && attached.dictionary->actions.size() == 1 &&
					!attached.dictionary->FindSet(unrelated) && !attached.dictionary->PathName(unrelatedPath) &&
					attached.dictionary->FindSet(retainedSet)->attached &&
					attached.dictionary->FindSet(retainedSet)->priority == 42 &&
					attached.dictionary->FindSet(empty)->name == "empty" && attached.profiles.empty(),
				"Attach dictionary lost empty sets, priorities, or included unrelated resources");
		InputSnapshot input;
		input.time = RuntimeTime{Duration{10}};
		input.controllers[1].primary = true;
		const XrActiveActionSet active{retainedSet, right};
		const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
		Require(state.Sync(sync, input, true) == XR_SUCCESS, "Synchronize historical state");
		retained = state.Snapshot();
		CheckDictionary(retained);
		auto malformed = retained;
		malformed.profiles[std::numeric_limits<XrPath>::max()] = profile;
		auto rejectedDictionary = retained.dictionary;
		Require(registry.BuildInspectionDictionary(sets, malformed, rejectedDictionary) == XR_ERROR_PATH_INVALID &&
					rejectedDictionary == retained.dictionary,
				"Unresolved profile path published a partial dictionary");
		malformed = retained;
		malformed.actions.front().values[std::numeric_limits<XrPath>::max()] = {};
		Require(registry.BuildInspectionDictionary(sets, malformed, rejectedDictionary) == XR_ERROR_PATH_INVALID &&
					rejectedDictionary == retained.dictionary,
				"Unresolved action-value path escaped dictionary validation");
		Require(retained.dictionary->FindAction(retainedAction)->localizedName == "select" &&
					retained.dictionary->FindSet(retainedSet)->localizedName == "historical" &&
					retained.actions.front().values.at(right).boolean && retained.profiles.at(right) == profile,
				"Retained snapshot lacks original names/profile/value");
		const auto unrelatedSource = Path(registry, "/user/hand/right/input/grip/pose");
		const XrActionSuggestedBinding unrelatedBinding{unrelatedAction, unrelatedSource};
		const XrInteractionProfileSuggestedBinding unrelatedSuggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
																	   nullptr, profile, 1, &unrelatedBinding};
		Require(registry.SuggestBindings(unrelatedSuggestion) == XR_SUCCESS,
				"Change suggestions for a new unattached set");
		input.time = RuntimeTime{Duration{20}};
		input.controllers[1].primary = false;
		Require(state.Sync(sync, input, true) == XR_SUCCESS && state.Snapshot().dictionary == retained.dictionary &&
					state.Snapshot().actions.front().values.at(right).active &&
					!state.Snapshot().dictionary->PathName(unrelatedSource),
				"Unattached suggestions changed live bindings or an otherwise identical attached dictionary");
		const auto beforeInvalid = state.Snapshot();
		const XrActiveActionSet invalid{unrelated, right};
		const XrActionsSyncInfo rejected{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &invalid};
		Require(state.Sync(rejected, input, false) == XR_ERROR_ACTIONSET_NOT_ATTACHED &&
					state.Snapshot().serial == beforeInvalid.serial &&
					state.Snapshot().dictionary == beforeInvalid.dictionary &&
					state.Snapshot().focused == beforeInvalid.focused,
				"Rejected Sync partially published serial, focus or dictionary");
		input.preferredProfiles[1] = "/interaction_profiles/khr/simple_controller";
		Require(state.Sync(sync, input, true) == XR_SUCCESS && state.Snapshot().profiles.at(right) == simple,
				"Change current profile for historical interpretation test");
		CheckDictionary(state.Snapshot());
		Require(registry.DestroyAction(retainedAction) == XR_SUCCESS && state.Sync(sync, input, true) == XR_SUCCESS &&
					!state.Snapshot().dictionary->FindAction(retainedAction)->live &&
					retained.dictionary->FindAction(retainedAction)->live,
				"Action retirement mutated historical live flags");
		Require(registry.DestroySet(retainedSet) == XR_SUCCESS && registry.DestroySet(empty) == XR_SUCCESS,
				"Retire historical sets");
		const XrActionsSyncInfo inactive{XR_TYPE_ACTIONS_SYNC_INFO};
		Require(state.Sync(inactive, input, true) == XR_SUCCESS &&
					!state.Snapshot().dictionary->FindSet(retainedSet)->live &&
					retained.dictionary->FindSet(retainedSet)->live,
				"Set retirement mutated historical names/flags");
		const auto retiredDictionary = state.Snapshot().dictionary;
		state.Reset();
		Require(state.Snapshot().dictionary == retiredDictionary && state.Snapshot().serial == 0 &&
					state.Snapshot().actions.front().values.empty(),
				"Reset lost dictionary ownership or retained action values");
		const auto replacement = Set(registry, "historical", 999);
		const auto replacementAction =
			Action(registry, replacement, "select", XR_ACTION_TYPE_BOOLEAN_INPUT, subactions);
		const XrActionSuggestedBinding newer{replacementAction, Path(registry, "/user/hand/left/input/x/click")};
		const XrInteractionProfileSuggestedBinding changed{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														   profile, 1, &newer};
		Require(registry.SuggestBindings(changed) == XR_SUCCESS &&
					registry.Bindings(retainedAction, profile).size() == 1 &&
					registry.Bindings(retainedAction, profile).front() ==
						ActionBinding{retainedAction, profile, oldSource, right, "/input/a/click",
									  XR_ACTION_TYPE_BOOLEAN_INPUT},
				"New unattached suggestions overwrote attached retired binding resources");
		ActionSyncState replacementState(registry);
		const XrSessionActionSetsAttachInfo replacementAttach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1,
															  &replacement};
		Require(replacementState.Attach(replacementAttach) == XR_SUCCESS &&
					replacementState.Snapshot().dictionary->FindSet(replacement)->priority == 999 &&
					!replacementState.Snapshot().dictionary->FindSet(retainedSet),
				"Replacement attachment mixed old resources");
		Require(registry.Bindings(retainedAction, profile).front().source == oldSource &&
					retained.dictionary->FindSet(retainedSet)->priority == 42,
				"Second attachment altered frozen original bindings/dictionary");
	}
	CheckDictionary(retained);
	Require(retained.dictionary->FindAction(retainedAction)->name == "select" &&
				retained.dictionary->FindSet(retainedSet)->priority == 42 &&
				retained.dictionary->PathName(oldSource) == "/user/hand/right/input/a/click" &&
				retained.dictionary->PathName(profile) == "/interaction_profiles/oculus/touch_controller" &&
				retained.activeSets.front().subactionPath == right,
			"Dictionary borrowed registry storage after instance/action lifetime ended");
}

void RegistryAdmissionRollback()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	unsigned pathFailures = 0;
	bool pathCompleted = false;
	for (int allocation = 0; allocation < 128; ++allocation)
	{
		EnabledExtensions enabled;
		ActionRegistry registry(reinterpret_cast<XrInstance>(70), enabled);
		const auto seed = Path(registry, "/seed");
		constexpr auto text = "/user/vive_tracker_htcx/serial/transactional_path";
		XrPath published = XR_NULL_PATH;
		bool failed = false;
		try
		{
			AllocationFailure failure(allocation);
			Require(registry.StringToPath(text, published) == XR_SUCCESS, "Intern transactional path");
		}
		catch (const std::bad_alloc&)
		{
			failed = true;
			++pathFailures;
		}
		Require(!failed || published == XR_NULL_PATH, "Failed path admission modified caller output");
		const auto retried = Path(registry, text);
		Require(Path(registry, text) == retried && registry.PathString(retried) == text,
				"Path retry did not round-trip to one identity");
		for (auto consumed = seed + 1; consumed < retried; ++consumed)
			Require(!registry.PathString(consumed), "Failed path admission left an orphaned reverse lookup");
		if (!failed)
		{
			pathCompleted = true;
			break;
		}
	}
	Require(pathCompleted && pathFailures, "Path allocation sweep did not complete");
	unsigned bindingFailures = 0;
	bool bindingsCompleted = false;
	for (int allocation = 0; allocation < 128; ++allocation)
	{
		EnabledExtensions enabled;
		ActionRegistry registry(reinterpret_cast<XrInstance>(71), enabled);
		const std::array paths{Path(registry, "/user/hand/left")};
		const auto set = Set(registry, "bindings");
		const auto action = Action(registry, set, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, paths);
		const auto profile = Path(registry, "/interaction_profiles/oculus/touch_controller");
		const XrActionSuggestedBinding binding{action, Path(registry, "/user/hand/left/input/trigger/value")};
		const XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														   profile, 1, &binding};
		bool failed = false;
		try
		{
			AllocationFailure failure(allocation);
			Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Suggest transactional binding");
		}
		catch (const std::bad_alloc&)
		{
			failed = true;
			++bindingFailures;
		}
		if (failed)
			Require(registry.Bindings(action, profile).empty() && registry.SelectProfile({}, paths[0]) == XR_NULL_PATH,
					"Failed binding admission partially published its profile");
		Require(registry.SuggestBindings(suggest) == XR_SUCCESS && registry.Bindings(action, profile).size() == 1 &&
					registry.SelectProfile({}, paths[0]) == profile,
				"Binding admission could not retry");
		if (!failed)
		{
			bindingsCompleted = true;
			break;
		}
	}
	Require(bindingsCompleted && bindingFailures, "Binding allocation sweep did not complete");
	std::cout << "Registry admission rollback covered " << pathFailures << " path and " << bindingFailures
			  << " binding allocations\n";
#endif
}

void ReservedSystemBindings()
{
	for (const auto profileName :
		 {"/interaction_profiles/htc/vive_controller", "/interaction_profiles/oculus/touch_controller",
		  "/interaction_profiles/valve/index_controller"})
	{
		EnabledExtensions enabled;
		ActionRegistry registry(reinterpret_cast<XrInstance>(70), enabled);
		const std::array paths{Path(registry, "/user/hand/left"), Path(registry, "/user/hand/right")};
		const auto profile = Path(registry, profileName);
		const auto set = Set(registry, "reserved-controls");
		const auto trigger = Action(registry, set, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, paths);
		const auto click = Action(registry, set, "trigger-click", XR_ACTION_TYPE_BOOLEAN_INPUT, paths);
		const auto system = Action(registry, set, "system", XR_ACTION_TYPE_BOOLEAN_INPUT, paths);
		const auto pose = Action(registry, set, "pose", XR_ACTION_TYPE_POSE_INPUT, paths);
		const auto triggerPath = Path(registry, "/user/hand/right/input/trigger/value");
		const auto systemPath = Path(registry, "/user/hand/right/input/system/click");
		const std::array bindings{XrActionSuggestedBinding{trigger, triggerPath},
								  XrActionSuggestedBinding{click, triggerPath},
								  XrActionSuggestedBinding{system, systemPath}};
		const XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														   profile, static_cast<std::uint32_t>(bindings.size()),
														   bindings.data()};
		Require(registry.SuggestBindings(suggest) == XR_SUCCESS,
				"Reserved system button rejected the entire controller profile");
		const XrActionSuggestedBinding incompatible{pose, systemPath};
		const XrInteractionProfileSuggestedBinding invalid{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														   profile, 1, &incompatible};
		Require(registry.SuggestBindings(invalid) == XR_ERROR_PATH_UNSUPPORTED &&
					registry.Bindings(trigger, profile).size() == 1 && registry.Bindings(system, profile).size() == 1,
				"Reserved path bypassed type validation or replaced accepted bindings on failure");

		ActionSyncState session(registry);
		const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
		Require(session.Attach(attach) == XR_SUCCESS, "Attach controller profile with reserved controls");
		InputSnapshot input;
		input.time = RuntimeTime{Duration{10}};
		input.preferredProfiles[1] = profileName;
		input.controllers[1].trigger = 0.75f;
		input.controllers[1].menu = input.controllers[1].back = input.controllers[1].primary = true;
		const XrActiveActionSet active{set, paths[1]};
		const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
		Require(session.Sync(sync, input, true) == XR_SUCCESS, "Synchronize controller with reserved controls");
		XrPath selected = XR_NULL_PATH;
		Require(session.CurrentProfile(paths[1], selected) == XR_SUCCESS && selected == profile,
				"Reserved control caused fallback from the requested controller profile");
		const XrActionStateGetInfo triggerGet{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, trigger, paths[1]};
		const XrActionStateGetInfo clickGet{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, click, paths[1]};
		const XrActionStateGetInfo systemGet{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, system, paths[1]};
		XrActionStateFloat triggerValue{XR_TYPE_ACTION_STATE_FLOAT};
		XrActionStateBoolean clickValue{XR_TYPE_ACTION_STATE_BOOLEAN}, systemValue{XR_TYPE_ACTION_STATE_BOOLEAN};
		Require(session.GetFloat(triggerGet, triggerValue) == XR_SUCCESS && triggerValue.isActive &&
					triggerValue.currentState == 0.75f && session.GetBoolean(clickGet, clickValue) == XR_SUCCESS &&
					clickValue.isActive && clickValue.currentState,
				"Reserved control suppressed usable bindings or float-to-boolean conversion");
		Require(session.GetBoolean(systemGet, systemValue) == XR_SUCCESS && !systemValue.isActive &&
					!systemValue.currentState,
				"Unavailable system control aliased an application button");
		input.time = RuntimeTime{Duration{20}};
		input.controllers[1].trigger = 0;
		Require(session.Sync(sync, input, true) == XR_SUCCESS &&
					session.GetFloat(triggerGet, triggerValue) == XR_SUCCESS && triggerValue.isActive &&
					triggerValue.currentState == 0 && session.GetBoolean(clickGet, clickValue) == XR_SUCCESS &&
					clickValue.isActive && !clickValue.currentState,
				"Controller bindings did not release after reserved suggestions were accepted");
	}
}

void InputPathLimits()
{
	InputState input;
	InputSnapshot valid;
	std::string path = "/user/vive_tracker_htcx/serial/";
	path.resize(XR_MAX_PATH_LENGTH - 1, 'a');
	valid.devices.push_back({{InputDeviceKind::Tracker, 1}, {path}});
	Require(input.Publish(valid) == XR_SUCCESS, "Reject maximum valid input path");
	auto invalid = valid;
	invalid.devices[0].userPaths[0] += 'a';
	Require(input.Publish(std::move(invalid)) == XR_ERROR_PATH_FORMAT_INVALID && input.Revision() == 1 &&
				input.Snapshot().devices[0].userPaths[0] == path,
			"Oversized input path bypassed admission validation");
}

void DictionaryAllocationRollback()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	EnabledExtensions enabled;
	ActionRegistry registry(reinterpret_cast<XrInstance>(60), enabled);
	const std::array paths{Path(registry, "/user/hand/left"), Path(registry, "/user/hand/right")};
	const auto set = Set(registry, "transactional", 3);
	const auto action = Action(registry, set, "transactional-trigger", XR_ACTION_TYPE_FLOAT_INPUT, paths);
	const auto profile = Path(registry, "/interaction_profiles/oculus/touch_controller");
	const XrActionSuggestedBinding binding{action, Path(registry, "/user/hand/right/input/trigger/value")};
	const XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, profile,
													   1, &binding};
	Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Prepare transactional dictionary binding");
	const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
	InputSnapshot input;
	input.time = RuntimeTime{Duration{10}};
	input.controllers[1].trigger = 0.75f;
	std::unique_ptr<ActionSyncState> state;
	unsigned attachFailures = 0;
	for (int allocation = 0; allocation < 1024; ++allocation)
	{
		auto candidate = std::make_unique<ActionSyncState>(registry);
		try
		{
			AllocationFailure failure(allocation);
			Require(candidate->Attach(attach, input) == XR_SUCCESS, "Attach failed without allocation error");
		}
		catch (const std::bad_alloc&)
		{
			++attachFailures;
			const auto unchanged = candidate->Snapshot();
			Require(!registry.Set(set)->attached && !unchanged.dictionary && unchanged.actions.empty() &&
						unchanged.serial == 0,
					"Attach allocation failure mutated registry attachment or published partial definitions");
			continue;
		}
		state = std::move(candidate);
		break;
	}
	Require(state && attachFailures > 10 && registry.Bindings(action, profile).size() == 1,
			"Attach allocation sweep did not reach all dictionary/frozen-binding preparation");
	const XrActiveActionSet active{set, paths[1]};
	const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
	Require(state->Sync(sync, input, true) == XR_SUCCESS, "Establish synchronized transactional baseline");
	const auto baseline = state->Snapshot();
	input.time = RuntimeTime{Duration{20}};
	input.controllers[1].trigger = 0.25f;
	unsigned syncFailures = 0;
	bool completed = false;
	for (int allocation = 0; allocation < 1024; ++allocation)
	{
		try
		{
			AllocationFailure failure(allocation);
			Require(state->Sync(sync, input, false) == XR_SESSION_NOT_FOCUSED, "Sync failed without allocation error");
		}
		catch (const std::bad_alloc&)
		{
			++syncFailures;
			const auto unchanged = state->Snapshot();
			XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
			const XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, paths[1]};
			Require(unchanged.serial == baseline.serial && unchanged.time == baseline.time && unchanged.focused &&
						unchanged.dictionary == baseline.dictionary && state->GetFloat(get, value) == XR_SUCCESS &&
						value.isActive && value.currentState == 0.75f,
					"Sync allocation failure published partial serial, focus, values or dictionary");
			continue;
		}
		completed = true;
		break;
	}
	Require(completed && syncFailures > 10 && state->Snapshot().serial == baseline.serial + 1 &&
				!state->Snapshot().focused && state->Snapshot().dictionary == baseline.dictionary,
			"Sync allocation sweep did not successfully retry an identical dictionary after rollback");
	std::cout << "Dictionary allocation rollback covered " << attachFailures << " Attach and " << syncFailures
			  << " Sync allocations\n";
#else
	std::cout << "Dictionary allocation fault injection covered by PCH/ASan; TSan owns allocator interceptors\n";
#endif
}

int main()
{
	EnabledExtensions enabled;
	ActionRegistry registry(reinterpret_cast<XrInstance>(1), enabled);
	XrPath ignored;
	for (auto text : {"/", "//foo", "/foo/", "/foo/../bar", "/foo/...", "/FOO", "/foo bar"})
		Require(registry.StringToPath(text, ignored) == XR_ERROR_PATH_FORMAT_INVALID, "Invalid semantic path");
	XrPath left = Path(registry, "/user/hand/left");
	XrPath right = Path(registry, "/user/hand/right");
	Require(Path(registry, "/user/hand/left") == left, "Stable path identity");
	std::uint32_t length;
	Require(registry.PathToString(left, 0, &length, nullptr) == XR_SUCCESS && length == 16, "Path size query");
	char shortBuffer[2];
	Require(registry.PathToString(left, 2, &length, shortBuffer) == XR_ERROR_SIZE_INSUFFICIENT, "Path capacity");
	std::array subactions{left, right};
	auto normal = Set(registry, "normal");
	auto high = Set(registry, "high", 2);
	auto trigger = Action(registry, normal, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, subactions);
	auto vector = Action(registry, normal, "vector", XR_ACTION_TYPE_VECTOR2F_INPUT, subactions);
	auto output = Action(registry, normal, "output", XR_ACTION_TYPE_VIBRATION_OUTPUT, subactions);
	auto priority = Action(registry, high, "priority", XR_ACTION_TYPE_FLOAT_INPUT, subactions);
	auto profile = Path(registry, "/interaction_profiles/oculus/touch_controller");
	std::array suggestions{XrActionSuggestedBinding{trigger, Path(registry, "/user/hand/left/input/trigger/value")},
						   XrActionSuggestedBinding{trigger, Path(registry, "/user/hand/right/input/trigger/value")},
						   XrActionSuggestedBinding{vector, Path(registry, "/user/hand/right/input/thumbstick")},
						   XrActionSuggestedBinding{output, Path(registry, "/user/hand/right/output/haptic")},
						   XrActionSuggestedBinding{priority, Path(registry, "/user/hand/right/input/trigger/value")}};
	XrInteractionProfileSuggestedBinding bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, profile,
												  static_cast<std::uint32_t>(suggestions.size()), suggestions.data()};
	Require(registry.SuggestBindings(bindings) == XR_SUCCESS, "Suggest valid bindings");
	auto badBinding = suggestions[0];
	badBinding.binding = Path(registry, "/user/hand/right/input/menu/click");
	XrInteractionProfileSuggestedBinding invalidBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														 profile, 1, &badBinding};
	Require(registry.SuggestBindings(invalidBindings) == XR_ERROR_PATH_UNSUPPORTED, "Wrong-hand component binding");
	Require(registry.Bindings(trigger, profile).size() == 2, "Failed transaction changed bindings");
	ActionSyncState session(registry);
	std::array sets{normal, high};
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 2, sets.data()};
	Require(session.Attach(attach) == XR_SUCCESS, "Attach sets");
	Require(session.Attach(attach) == XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "Duplicate attachment");
	XrPath selected;
	Require(session.CurrentProfile(right, selected) == XR_SUCCESS && selected == XR_NULL_PATH,
			"Profile selected before first sync");
	std::vector<XrPath> sources;
	Require(session.BoundSources(trigger, sources) == XR_SUCCESS && sources.size() == 2,
			"Resolved sources before sync");
	Require(registry.SuggestBindings(bindings) == XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "Attached bindings mutable");
	XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, trigger, right};
	XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
	Require(session.GetFloat(get, value) == XR_SUCCESS && !value.isActive, "New session inherited synchronized state");
	InputState raw;
	InputSnapshot input;
	input.time = {Duration{10}};
	input.controllers[0].trigger = 0.9f;
	input.controllers[1].trigger = 0.4f;
	input.controllers[1].stick = {0.6f, -0.7f};
	Require(raw.Publish(input) == XR_SUCCESS, "Publish raw controls");
	std::array active{XrActiveActionSet{normal, XR_NULL_PATH}};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, active.data()};
	Require(session.Sync(sync, raw.Snapshot(), true) == XR_SUCCESS, "Synchronize controls");
	Require(session.CurrentProfile(right, selected) == XR_SUCCESS && selected == profile,
			"First sync did not select profile");
	Require(session.GetFloat(get, value) == XR_SUCCESS && value.isActive && value.currentState == 0.4f &&
				!value.changedSinceLastSync,
			"Initial active state should not report a change");
	get.subactionPath = XR_NULL_PATH;
	Require(session.GetFloat(get, value) == XR_SUCCESS && value.currentState == 0.9f, "Aggregate source resolution");
	auto retained = session.Snapshot();
	input.time = {Duration{20}};
	input.controllers[1].trigger = 0.6f;
	Require(raw.Publish(input) == XR_SUCCESS, "Update raw controls");
	get.subactionPath = right;
	Require(session.GetFloat(get, value) == XR_SUCCESS && value.currentState == 0.4f, "Query bypassed sync");
	Require(session.Snapshot().serial == retained.serial, "Inspector advanced synchronization");
	Require(session.Sync(sync, raw.Snapshot(), true) == XR_SUCCESS, "Second synchronization");
	Require(session.GetFloat(get, value) == XR_SUCCESS && value.currentState == 0.6f && value.changedSinceLastSync &&
				value.lastChangeTime == 20,
			"Changed input timing");
	Require(retained.actions[0].values.size() == 3 && retained.serial == 1, "Retained snapshot mutated");
	std::array both{XrActiveActionSet{normal, XR_NULL_PATH}, XrActiveActionSet{high, right}};
	sync.countActiveActionSets = 2;
	sync.activeActionSets = both.data();
	Require(session.Sync(sync, raw.Snapshot(), true) == XR_SUCCESS, "Priority synchronization");
	Require(session.GetFloat(get, value) == XR_SUCCESS && !value.isActive && value.currentState == 0 &&
				!value.changedSinceLastSync && value.lastChangeTime == 0,
			"Higher priority did not suppress shared source");
	get.subactionPath = left;
	Require(session.GetFloat(get, value) == XR_SUCCESS && value.isActive && value.currentState == 0.9f,
			"Priority affected other hand");
	XrHapticActionInfo target{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, output, right};
	std::vector<std::size_t> devices;
	Require(session.OutputDevices(target, devices) == XR_SUCCESS && devices == std::vector<std::size_t>{1},
			"Haptic target resolution");
	Require(registry.DestroyAction(priority) == XR_SUCCESS && !registry.Action(priority) &&
				registry.ActionResource(priority),
			"Action retirement removed internal resource");
	Require(registry.DestroyAction(priority) == XR_ERROR_HANDLE_INVALID, "Retired action handle accepted");
	get.subactionPath = right;
	Require(session.Sync(sync, raw.Snapshot(), true) == XR_SUCCESS && session.GetFloat(get, value) == XR_SUCCESS &&
				!value.isActive,
			"Retired high-priority action stopped suppressing shared input");
	XrActionStateGetInfo deadPriority{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, priority, right};
	Require(session.GetFloat(deadPriority, value) == XR_ERROR_HANDLE_INVALID,
			"Retired action remained publicly queryable");
	session.SetFocused(false);
	Require(session.GetFloat(get, value) == XR_SUCCESS && !value.isActive, "Focus loss query");
	Require(session.Sync(sync, raw.Snapshot(), false) == XR_SESSION_NOT_FOCUSED, "Focus loss sync result");
	Require(session.GetFloat(get, value) == XR_SUCCESS && !value.isActive && value.lastChangeTime == 0,
			"Focus loss state");
	Require(session.Sync(sync, raw.Snapshot(), true) == XR_SUCCESS, "Focus recovery");
	Require(session.GetFloat(get, value) == XR_SUCCESS && !value.changedSinceLastSync, "Recovery fabricated change");
	ActionSyncState replacement(registry);
	Require(replacement.Attach(attach) == XR_SUCCESS, "Reattach immutable definitions to replacement session");
	Require(replacement.GetFloat(get, value) == XR_SUCCESS && !value.isActive, "State leaked between sessions");
	input.controllers[0].trigger = std::numeric_limits<float>::quiet_NaN();
	Require(raw.Publish(input) == XR_ERROR_VALIDATION_FAILURE && raw.Snapshot().revision == 2,
			"Rejected controls were published");
	Require(registry.DestroySet(normal) == XR_SUCCESS && !registry.Action(trigger), "Parent teardown");
	Require(session.GetFloat(get, value) == XR_ERROR_HANDLE_INVALID, "Destroyed action query");
	Require(registry.SetResource(normal) && !registry.Set(normal) && registry.ActionResource(trigger) &&
				registry.Resources().size() == 4 && registry.Actions().empty(),
			"Set retirement freed resources or exposed dead handles");
	Require(session.Sync(sync, raw.Snapshot(), true) == XR_ERROR_HANDLE_INVALID,
			"Retired set accepted as active-set argument");

	HistoricalDictionaryAndFrozenBindings();
	DictionaryAllocationRollback();
	RegistryAdmissionRollback();
	ReservedSystemBindings();
	InputPathLimits();
	std::cout << "Action registry tests passed\n";
}
