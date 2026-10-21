#include "../TestSupport.h"
#include "runtime/Session.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

namespace
{
thread_local int allocationCountdown = -1;
thread_local bool allocationFailed = false;
} // namespace

[[gnu::noinline]] void* operator new(std::size_t size)
{
	if (allocationCountdown >= 0 && allocationCountdown-- == 0)
	{
		allocationFailed = true;
		throw std::bad_alloc{};
	}
	if (void* memory = std::malloc(size ? size : 1))
		return memory;
	throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* memory) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

namespace
{
void TrackerPublicationFailure()
{
	using namespace simxr;
	for (int failure = 0; failure < 256; ++failure)
	{
		const std::array extensions{XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME};
		Instance instance(reinterpret_cast<XrInstance>(941), extensions, XR_MAKE_VERSION(1, 0, 27));
		auto& trackers = instance.Trackers();
		const auto previous = trackers.Paths();
		const auto eventCount = instance.Events().Observe().size();
		TrackingState state;
		for (auto& tracker : state.trackers)
			tracker.identity.value += 10;
		const auto snapshot = TrackingSystem::Evaluate({Duration{2}}, state, BodyModel{});
		std::vector<XrPath> changed{777};
		allocationFailed = false;
		allocationCountdown = failure;
		try
		{
			Require(trackers.Publish(snapshot, &changed) == XR_SUCCESS, "Publish replacement tracker inventory");
		}
		catch (const std::bad_alloc&)
		{
		}
		allocationCountdown = -1;
		const bool failed = allocationFailed;
		if (failed)
		{
			const auto unchanged = trackers.Paths();
			Require(unchanged.size() == previous.size() &&
						std::equal(unchanged.begin(), unchanged.end(), previous.begin(),
								   [](const auto& left, const auto& right)
								   {
									   return left.identity == right.identity &&
											  left.persistentPath == right.persistentPath &&
											  left.rolePath == right.rolePath && left.connected == right.connected;
								   }) &&
						changed == std::vector<XrPath>{777} && instance.Events().Observe().size() == eventCount,
					"Failed tracker publication leaked state or connection events");
			Require(trackers.Publish(snapshot, &changed) == XR_SUCCESS, "Retry tracker publication");
		}
		Require(instance.Events().Observe().size() == eventCount + state.trackers.size() &&
					trackers.Paths().front().identity == state.trackers.front().identity,
				"Tracker publication lost or duplicated committed connection events");
		if (!failed)
		{
			std::cout << "Tracker publication allocation sweep: " << failure << " failures\n";
			return;
		}
	}
	throw std::runtime_error("Tracker publication allocation sweep exceeded its bound");
}

void ProfileNotificationFailure()
{
	using namespace simxr;
	for (int failure = 0; failure < 1024; ++failure)
	{
		Instance instance(reinterpret_cast<XrInstance>(931), {}, XR_MAKE_VERSION(1, 0, 27));
		Session session(reinterpret_cast<XrSession>(932), instance);
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"Start profile notification session");
		auto& registry = instance.Actions();
		const auto path = [&](const char* name)
		{
			XrPath result = XR_NULL_PATH;
			Require(registry.StringToPath(name, result) == XR_SUCCESS, "Create profile path");
			return result;
		};
		XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strcpy(setInfo.actionSetName, "controls");
		std::strcpy(setInfo.localizedActionSetName, "Controls");
		XrActionSet actionSet = XR_NULL_HANDLE;
		Require(registry.CreateSet(setInfo, actionSet) == XR_SUCCESS, "Create profile action set");
		const auto userPath = path("/user/hand/left");
		XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(actionInfo.actionName, "trigger");
		std::strcpy(actionInfo.localizedActionName, "Trigger");
		actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
		actionInfo.countSubactionPaths = 1;
		actionInfo.subactionPaths = &userPath;
		XrAction action = XR_NULL_HANDLE;
		Require(registry.CreateAction(actionSet, actionInfo, action) == XR_SUCCESS, "Create profile action");
		const XrActionSuggestedBinding binding{action, path("/user/hand/left/input/trigger/value")};
		for (const auto* profile :
			 {"/interaction_profiles/oculus/touch_controller", "/interaction_profiles/htc/vive_controller"})
		{
			const XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
																 path(profile), 1, &binding};
			Require(registry.SuggestBindings(suggested) == XR_SUCCESS, "Suggest profile binding");
		}
		const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &actionSet};
		Require(session.Attach(attach) == XR_SUCCESS, "Attach profile action set");
		const XrActiveActionSet active{actionSet, XR_NULL_PATH};
		const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
		Require(session.SyncActions(sync) == XR_SUCCESS, "Synchronize initial profile");
		const auto profileEvents = [&]
		{
			const auto events = instance.Events().Observe();
			return std::count_if(events.begin(), events.end(),
								 [](const auto& event)
								 {
									 return std::holds_alternative<InteractionProfileEvent>(event.event);
								 });
		};
		const auto before = profileEvents();
		auto input = session.Input().Snapshot();
		input.preferredProfiles[0] = "/interaction_profiles/htc/vive_controller";
		Require(session.PublishInput(std::move(input)) == XR_SUCCESS, "Change preferred profile");
		allocationFailed = false;
		allocationCountdown = failure;
		XrResult result = XR_ERROR_OUT_OF_MEMORY;
		try
		{
			result = session.SyncActions(sync);
		}
		catch (const std::bad_alloc&)
		{
		}
		allocationCountdown = -1;
		Require((result == XR_SUCCESS || result == XR_ERROR_OUT_OF_MEMORY) && session.SyncActions(sync) == XR_SUCCESS,
				"Retry profile synchronization");
		XrPath profile = XR_NULL_PATH;
		Require(session.Actions().CurrentProfile(userPath, profile) == XR_SUCCESS &&
					profile == path("/interaction_profiles/htc/vive_controller") && profileEvents() == before + 1,
				"Allocation failure lost or duplicated the committed profile notification");
		if (!allocationFailed)
		{
			std::cout << "Profile notification allocation sweep: " << failure << " failures\n";
			return;
		}
	}
	throw std::runtime_error("Profile notification allocation sweep exceeded its bound");
}

void FailedStart()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(911), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(912), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	timing.cadence = 3;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS, "Ready failed-start session");
	bool rejected = false;
	try
	{
		session.Begin(begin, timing);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	const CommandTransaction command{1,
									 {CommandSourceKind::Manual, 1},
									 {FrameId{1}, {}},
									 CommandLatePolicy::Reject,
									 {TakeControl{ControlChannel::LeftControls}}};
	Require(rejected && !session.Snapshot().running && !session.Scheduler().Snapshot().running &&
				!session.Display().Snapshot().running &&
				session.Commands().Submit(command).result == CommandResult::Closed,
			"Failed start left command admission open");
	timing.cadence = 1;
	FramePrediction prediction;
	Require(session.Begin(begin, timing) == XR_SUCCESS &&
				session.Commands().Submit(command).result == CommandResult::Queued &&
				session.WaitFrame(prediction) == XR_SUCCESS && prediction.id == FrameId{1},
			"Failed start poisoned the next valid start");
}

void ReplacedGeneration()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(921), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(922), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
			"Start replaced-generation session");
	const auto generation = session.Snapshot().generation;
	FramePrediction prediction;
	const auto result = session.WaitFrame(
		prediction,
		[&](const FramePrediction&) -> XrResult
		{
			Require(session.RequestExit(timing.epoch) == XR_SUCCESS && session.End(timing.epoch) == XR_SUCCESS &&
						session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
					"Replace generation while native publication is in progress");
			throw std::bad_alloc{};
		});
	Require(result == XR_ERROR_SESSION_LOST && session.Snapshot().generation != generation &&
				!session.Snapshot().closed && session.WaitFrame(prediction) == XR_SUCCESS,
			"An old publication failure closed its replacement generation");
}

void PublicationFailure()
{
	using namespace simxr;
	unsigned retryable = 0;
	unsigned lost = 0;
	for (int failure = 0; failure < 1024; ++failure)
	{
		Instance instance(reinterpret_cast<XrInstance>(901), {}, XR_MAKE_VERSION(1, 0, 27));
		Session session(reinterpret_cast<XrSession>(902), instance);
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"Start publication failure session");
		const auto generation = session.Snapshot().generation;
		const CommandTransaction command{1,
										 {CommandSourceKind::Manual, 1},
										 {FrameId{1}, {}},
										 CommandLatePolicy::Reject,
										 {TakeControl{ControlChannel::LeftControls}}};
		Require(session.Commands().Submit(command).result == CommandResult::Queued, "Queue boundary command");
		FramePrediction prediction;
		XrResult result = XR_ERROR_RUNTIME_FAILURE;
		allocationFailed = false;
		allocationCountdown = failure;
		try
		{
			result = session.WaitFrame(prediction,
									   [&](const FramePrediction& admitted)
									   {
										   session.PublishTracking(admitted.displayTime, TrackingState{},
																   ViewSettings{}, admitted.id);
										   return XR_SUCCESS;
									   });
		}
		catch (const std::bad_alloc&)
		{
			result = XR_ERROR_OUT_OF_MEMORY;
		}
		allocationCountdown = -1;
		if (!allocationFailed)
		{
			Require(result == XR_SUCCESS && retryable && lost, "Sweep did not cover both sides of admission");
			std::cout << "Publication allocation sweep: " << retryable << " retryable, " << lost << " session loss\n";
			return;
		}
		Require(XR_FAILED(result), "Allocation failure disappeared");
		const auto pipeline = session.Scheduler().Snapshot();
		Require(!pipeline.waited, "Failed publication stranded an admitted frame in xrWaitFrame");
		if (result == XR_ERROR_SESSION_LOST)
		{
			++lost;
			const auto record = session.History().Find({generation, FrameId{1}});
			Require(session.Snapshot().closed && !pipeline.running && !session.Display().Snapshot().running && record &&
						record->stage == FrameStage::Cancelled,
					"Publication failure left live owners or an uncancelled frame");
			const auto diagnostics = session.History().Diagnostics();
			Require(std::count_if(diagnostics.events.begin(), diagnostics.events.end(),
								  [](const auto& event)
								  {
									  return event.kind == FrameTraceKind::WaitReturned &&
											 event.result == XR_ERROR_SESSION_LOST;
								  }) == 1,
					"Wait diagnostics did not report the actual publication failure");
			Require(session.Ready(timing.epoch) == XR_ERROR_SESSION_LOST, "Partially published session could restart");
		}
		else
		{
			++retryable;
			Require(result == XR_ERROR_OUT_OF_MEMORY && !session.Snapshot().closed &&
						session.WaitFrame(prediction) == XR_SUCCESS && prediction.id == FrameId{1},
					"Failure before admission consumed the frame or prevented retry");
		}
	}
	throw std::runtime_error("Publication allocation sweep exceeded its bound");
}
} // namespace

int main()
{
	try
	{
		PublicationFailure();
		ReplacedGeneration();
		FailedStart();
		ProfileNotificationFailure();
		TrackerPublicationFailure();
		return 0;
	}
	catch (const std::exception& error)
	{
		allocationCountdown = -1;
		std::cerr << error.what() << '\n';
		return 1;
	}
}
