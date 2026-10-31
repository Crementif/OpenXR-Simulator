#include "../TestSupport.h"
#include "runtime/EnumNames.h"
#include "runtime/NativeBindings.h"
#include "runtime/Runtime.h"
#include "runtime/SimulationExtensions.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <thread>

namespace
{
thread_local bool rejectTeardownAllocation = false;
}

[[gnu::noinline]] void* operator new(std::size_t size)
{
	if (rejectTeardownAllocation)
		throw std::bad_alloc{};
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
using namespace simxr;
using namespace std::chrono_literals;

XrInstanceCreateInfo InstanceInfo()
{
	XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(info.applicationInfo.applicationName, "Runtime ownership tests");
	return info;
}

void ApiVersions()
{
	Require(NegotiateApiVersion(XR_MAKE_VERSION(1, 0, 99), XR_MAKE_VERSION(1, 0, 99)) == XR_MAKE_VERSION(1, 0, 27),
			"Newer loader patch revision rejected a compatible runtime");
	Require(NegotiateApiVersion(XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0, 1)) == XR_MAKE_VERSION(1, 0, 27),
			"Older loader patch revision changed the reported runtime API");
	Require(NegotiateApiVersion(XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0)) == SupportedApiVersion,
			"Loader supporting multiple minor versions could not select core 1.1");
	Require(!NegotiateApiVersion(XR_MAKE_VERSION(1, 2, 0), XR_MAKE_VERSION(1, 2, 99)) &&
				!NegotiateApiVersion(0, XR_MAKE_VERSION(0, 9, 99)) &&
				!NegotiateApiVersion(XR_MAKE_VERSION(2, 0, 0), XR_MAKE_VERSION(2, 0, 99)),
			"Loader without a compatible interface version was accepted");
	Runtime runtime{ExtensionRegistry{SimulationExtensions()}};
	auto info = InstanceInfo();
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 99);
	Runtime::InstanceRef instance;
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS,
			"Instance creation disagreed with loader patch compatibility");
}

void EnumNames()
{
	struct ResultCase
	{
		XrResult value;
		const char* expected;
	};

	const ResultCase results[]{
		{XR_SUCCESS, "XR_SUCCESS"},
		{XR_ERROR_HANDLE_INVALID, "XR_ERROR_HANDLE_INVALID"},
		{XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB, "XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB"},
		{static_cast<XrResult>(123456789), "XR_UNKNOWN_SUCCESS_123456789"},
		{static_cast<XrResult>(-123456789), "XR_UNKNOWN_FAILURE_-123456789"},
		{static_cast<XrResult>(std::numeric_limits<std::int32_t>::min()), "XR_UNKNOWN_FAILURE_-2147483648"}};
	std::array<char, XR_MAX_RESULT_STRING_SIZE + 1> resultBuffer;
	for (const auto& test : results)
	{
		resultBuffer.fill('!');
		Require(ResultToString(test.value, resultBuffer.data()) == XR_SUCCESS &&
					std::memcmp(resultBuffer.data(), test.expected, std::strlen(test.expected) + 1) == 0 &&
					resultBuffer.back() == '!',
				"Result string did not preserve its canonical name or signed fallback");
	}

	struct StructureCase
	{
		XrStructureType value;
		const char* expected;
	};

	const StructureCase structures[]{
		{XR_TYPE_INSTANCE_PROPERTIES, "XR_TYPE_INSTANCE_PROPERTIES"},
		{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, "XR_TYPE_GRAPHICS_BINDING_D3D11_KHR"},
		{static_cast<XrStructureType>(123456789), "XR_UNKNOWN_STRUCTURE_TYPE_123456789"},
		{static_cast<XrStructureType>(2147483646), "XR_UNKNOWN_STRUCTURE_TYPE_2147483646"}};
	std::array<char, XR_MAX_STRUCTURE_NAME_SIZE + 1> structureBuffer;
	for (const auto& test : structures)
	{
		structureBuffer.fill('!');
		Require(StructureTypeToString(test.value, structureBuffer.data()) == XR_SUCCESS &&
					std::memcmp(structureBuffer.data(), test.expected, std::strlen(test.expected) + 1) == 0 &&
					structureBuffer.back() == '!',
				"Structure string did not preserve its canonical name or signed fallback");
	}
	structureBuffer.fill('!');
	Require(StructureTypeToString(XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT,
								  structureBuffer.data()) == XR_SUCCESS &&
				std::memcmp(structureBuffer.data(), "XR_TYPE_INTERACTION_RENDER_MODEL_TOP_LEVEL_USER_PATH_GET_INFO_EXT",
							XR_MAX_STRUCTURE_NAME_SIZE - 1) == 0 &&
				structureBuffer[XR_MAX_STRUCTURE_NAME_SIZE - 1] == '\0' && structureBuffer.back() == '!',
			"Long structure name did not truncate and terminate within the original API buffer");
	Require(ResultToString(XR_SUCCESS, nullptr) == XR_ERROR_VALIDATION_FAILURE &&
				StructureTypeToString(XR_TYPE_INSTANCE_PROPERTIES, nullptr) == XR_ERROR_VALIDATION_FAILURE,
			"Null string output was accepted");
}

void AllocationFreeShutdown()
{
	Runtime runtime{ExtensionRegistry{SimulationExtensions()}};
	auto info = InstanceInfo();
	const char* extension = XR_FB_BODY_TRACKING_EXTENSION_NAME;
	info.enabledExtensionCount = 1;
	info.enabledExtensionNames = &extension;
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS &&
				runtime.CreateSession(instance, session) == XR_SUCCESS,
			"Create teardown graph");
	XrSpace space{};
	Require(session->Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {}}, space) ==
				XR_SUCCESS,
			"Create teardown space");
	XrBodyTrackerFB body{};
	const XrBodyTrackerCreateInfoFB bodyInfo{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, nullptr,
											 XR_BODY_JOINT_SET_DEFAULT_FB};
	Require(session->Body().CreateFB(&bodyInfo, &body) == XR_SUCCESS, "Create teardown body tracker");
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	FramePrediction prediction;
	Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS &&
				session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
			"Begin pending application frame before teardown");
	bool allocated = false;
	{
		struct RestoreAllocation
		{
			~RestoreAllocation()
			{
				rejectTeardownAllocation = false;
			}
		} restore;

		rejectTeardownAllocation = true;
		try
		{
			runtime.Close();
		}
		catch (const std::bad_alloc&)
		{
			allocated = true;
		}
	}
	Require(!allocated, "Runtime graph teardown required a new allocation");
	Require(runtime.Instances().empty() && !runtime.FindSession(session) && session->Snapshot().closed &&
				!session->Spaces().Owns(space) && !session->Body().OwnsFB(body),
			"Shutdown left public child handles alive");
}

void GameBridgeLifetime()
{
	Runtime runtime{ExtensionRegistry{SimulationExtensions()}};
	const auto game = runtime.Game().Register(SIMXR_GAME_CAN_COMMAND);
	Require(game != 0, "Register game independently of XR");
	auto command = runtime.Game().Submit(game, 1, 1, SIMXR_GAME_COMMAND, "checkpoint");
	Require(command.has_value(), "Queue game command before XR creation");
	auto info = InstanceInfo();
	Runtime::InstanceRef instance;
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS && runtime.DestroyInstance(instance) == XR_SUCCESS,
			"Create and destroy optional XR instance");
	SimxrGameCommand delivered{};
	Require(runtime.Game().Poll(game, delivered) && delivered.id == command->id,
			"XR instance teardown interrupted the independent game bridge");
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS &&
				runtime.Game().Complete(game, delivered.id, true, "applied"),
			"XR recreation lost the game command");
	command = runtime.Game().Submit(game, 1, 2, SIMXR_GAME_COMMAND, "pending");
	Require(command.has_value(), "Queue game command before runtime shutdown");
	runtime.Close();
	const auto interrupted = runtime.Game().Read(game, 1, command->id);
	Require(interrupted && interrupted->phase == GameCommandPhase::Interrupted &&
				!runtime.Game().Poll(game, delivered) && !runtime.Game().Register(SIMXR_GAME_CAN_COMMAND),
			"Runtime shutdown left the game bridge accepting work");
}

void Lifecycle()
{
	ExtensionRegistry extensions(SimulationExtensions());
	RuntimeConfiguration configuration;
	configuration.maxInstances = 2;
	configuration.maxSessionsPerInstance = 2;
	Runtime runtime(extensions, configuration);
	Runtime::InstanceRef first, second;
	auto info = InstanceInfo();
	Require(runtime.CreateInstance(nullptr, first) == XR_ERROR_VALIDATION_FAILURE, "Null instance create info");
	info.applicationInfo.applicationName[0] = 0;
	Require(runtime.CreateInstance(&info, first) == XR_ERROR_NAME_INVALID, "Missing application name");
	info = InstanceInfo();
	info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 2, 0);
	Require(runtime.CreateInstance(&info, first) == XR_ERROR_API_VERSION_UNSUPPORTED, "Unsupported core version");
	info = InstanceInfo();
	info.createFlags = 1;
	Require(runtime.CreateInstance(&info, first) == XR_ERROR_VALIDATION_FAILURE, "Unsupported instance flags");
	info = InstanceInfo();
	const char* dependency = XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME;
	info.enabledExtensionCount = 1;
	info.enabledExtensionNames = &dependency;
	Require(runtime.CreateInstance(&info, first) == XR_ERROR_EXTENSION_NOT_PRESENT,
			"Runtime bypassed core 1.0 extension dependencies");
	info = InstanceInfo();
	Require(runtime.CreateInstance(&info, first) == XR_SUCCESS, "First instance");
	Require(runtime.CreateInstance(&info, second) == XR_SUCCESS, "Second instance");
	Runtime::InstanceRef extra;
	Require(runtime.CreateInstance(&info, extra) == XR_ERROR_LIMIT_REACHED && !extra, "Instance capacity");
	Require(runtime.Instances().size() == 2 && runtime.FindInstance(first), "Runtime instance ownership");
	Runtime::SessionRef session, sibling, other, exceeded;
	Require(runtime.CreateSession(XR_NULL_HANDLE, session) == XR_ERROR_HANDLE_INVALID, "Null session parent");
	Require(runtime.CreateSession(first, session, {}) == XR_ERROR_VALIDATION_FAILURE, "Missing session clock");
	Require(runtime.CreateSession(first, session) == XR_SUCCESS, "Create session");
	Require(runtime.CreateSession(first, sibling) == XR_SUCCESS, "Create sibling");
	Require(runtime.CreateSession(second, other) == XR_SUCCESS, "Create other parent's session");
	Require(runtime.CreateSession(first, exceeded) == XR_ERROR_LIMIT_REACHED, "Session capacity");
	Require(!runtime.FindSession(session, second) && runtime.FindSession(session, first), "Wrong parent accepted");
	Require(!runtime.FindInstance(reinterpret_cast<XrInstance>(static_cast<XrSession>(session))),
			"Session accepted as instance");
	Require(!runtime.FindSession(reinterpret_cast<XrSession>(static_cast<XrInstance>(first))),
			"Instance accepted as session");
	Require(session->Snapshot().state == XR_SESSION_STATE_IDLE, "Core creation published graphics-ready state");
	Require(session->Ready({Duration{1}}) == XR_SUCCESS, "Initialize session ready");
	FrameSchedulerConfig scheduler;
	scheduler.mode = ExecutionMode::Deterministic;
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
	begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	Require(session->Begin(begin, scheduler) == XR_SUCCESS, "Begin owned session");
	FramePrediction prediction;
	Require(session->WaitFrame(prediction) == XR_SUCCESS, "First wait");
	XrSpace space;
	Require(session->Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {}}, space) ==
				XR_SUCCESS,
			"Owned child space");
	auto waiting = std::async(std::launch::async, [session] {
		FramePrediction next;
		return session->WaitFrame(next);
	});
	Eventually([&] { return session->Scheduler().Snapshot().waitPending; }, "Owned wait did not block");
	const XrInstance staleInstance = first;
	const XrSession staleSession = session;
	std::weak_ptr<Instance> parentLifetime = first.Shared();
	Require(runtime.DestroyInstance(first) == XR_SUCCESS, "Destroy parent graph");
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_ERROR_SESSION_NOT_RUNNING,
			"Graph teardown did not cancel wait");
	Require(!runtime.FindInstance(staleInstance) && !runtime.FindSession(staleSession) && !runtime.FindSession(sibling),
			"Parent teardown left live handles");
	Require(session->Snapshot().closed && !session->Spaces().Owns(space), "Parent teardown left live domain children");
	Require(runtime.FindInstance(second) && runtime.FindSession(other), "Teardown affected another instance");
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	Require(first->Events().Poll(event) == XR_EVENT_UNAVAILABLE && !first->Events().Observe().empty(),
			"Event cleanup consumed observer history");
	Require(runtime.DestroySession(staleSession) == XR_ERROR_HANDLE_INVALID &&
				runtime.DestroyInstance(staleInstance) == XR_ERROR_HANDLE_INVALID,
			"Stale graph handle accepted");
	first = {};
	Require(!parentLifetime.expired(), "Retained child lost parent CPU state");
	session = {};
	sibling = {};
	Require(parentLifetime.expired(), "Graph teardown left an ownership cycle");
	Runtime::InstanceRef replacement;
	Require(runtime.CreateInstance(&info, replacement) == XR_SUCCESS &&
				static_cast<XrInstance>(replacement) != staleInstance,
			"Instance identity reused");
	runtime.Close();
	Require(runtime.Instances().empty() && other->Snapshot().closed, "Runtime close left objects active");
	Require(runtime.CreateInstance(&info, extra) == XR_ERROR_RUNTIME_FAILURE, "Closed runtime accepted creation");
}

struct NativeState
{
	std::function<void()> destroy;

	~NativeState()
	{
		destroy();
	}
};

void NativeLifetime()
{
	ExtensionRegistry extensions(SimulationExtensions());
	Runtime runtime(extensions);
	Runtime::InstanceRef instance;
	auto info = InstanceInfo();
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS, "Native parent");
	Runtime::SessionRef core;
	Require(runtime.CreateSession(instance, core) == XR_SUCCESS, "Native session");
	NativeBindings<XrSession, Session, NativeState> bindings;
	bool destroyed = false;
	bool ownerAlive = false;
	bool reentered = false;
	std::weak_ptr<Session> lifetime = core.Shared();
	const XrSession handle = core;
	auto object = std::make_unique<NativeState>();
	object->destroy = [&] {
		ownerAlive = !lifetime.expired();
		destroyed = true;
		reentered = !bindings.Remove(handle);
	};
	auto native = bindings.Attach(core, std::move(object));
	Require(bindings.Find(core) && !bindings.Find({}), "Native attachment lookup");
	Require(runtime.DestroyInstance(instance) == XR_SUCCESS, "Retire native parent");
	core = {};
	instance = {};
	Require(bindings.Remove(handle), "Remove native attachment");
	Require(!destroyed && !lifetime.expired(), "Native CPU reader lost owner");
	native = {};
	Require(destroyed && ownerAlive && reentered && lifetime.expired(),
			"Native destruction order or lock lifetime invalid");

	// Scope teardown must have the same callback-safe retirement as Remove.
	Runtime::InstanceRef nextInstance;
	Runtime::SessionRef nextCore;
	Require(runtime.CreateInstance(&info, nextInstance) == XR_SUCCESS &&
				runtime.CreateSession(nextInstance, nextCore) == XR_SUCCESS,
			"Scope native owner");
	bool scopeDestroyed = false;
	{
		NativeBindings<XrSession, Session, NativeState> scoped;
		const XrSession nextHandle = nextCore;
		auto scopedObject = std::make_unique<NativeState>();
		scopedObject->destroy = [&scoped, nextHandle, &scopeDestroyed] {
			scopeDestroyed = !scoped.Remove(nextHandle);
		};
		scoped.Attach(nextCore, std::move(scopedObject));
	}
	Require(scopeDestroyed, "Native table scope teardown reentered a partially destroyed table");
}

class ReentrantOutput final : public FrameOutput
{
public:
	std::function<void()> observe;
	std::function<void()> destroy;

	~ReentrantOutput() override
	{
		if (destroy)
			destroy();
	}

	std::atomic<CompletionStatus> completion{CompletionStatus::Pending};
	std::shared_ptr<FrameReservation> reservation;

	CompletionStatus PollCompletion() const noexcept override
	{
		observe();
		return completion.load();
	}
};

void ReusedSessionReference()
{
	ExtensionRegistry extensions(SimulationExtensions());
	Runtime runtime(extensions);
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	auto info = InstanceInfo();
	Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS &&
				runtime.CreateSession(instance, session) == XR_SUCCESS,
			"Reusable session owner");
	FramePool pool(1);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->ConfigureCapturePool(pool) == XR_SUCCESS && session->Ready(timing.epoch) == XR_SUCCESS &&
				session->Begin(begin, timing) == XR_SUCCESS,
			"Begin reusable session owner");
	session->Scheduler().Pause();
	Require(session->ReserveCapture().result == SnapshotAdmission::Accepted &&
				session->Scheduler().GrantPermits() == XR_SUCCESS,
			"Reserve pending snapshot for reuse");
	FramePrediction prediction;
	Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
			"Issue pending snapshot for reuse");
	auto output = std::make_shared<ReentrantOutput>();
	output->observe = [] {
	};
	output->reservation = session->Captures().ProducerReservation({session->Snapshot().generation, prediction.id});
	Require(pool.Consume(output->reservation), "Consume reuse snapshot capacity");
	bool destroyed = false;
	output->destroy = [&] {
		runtime.Instances();
		destroyed = true;
	};
	auto layers = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
													  std::vector<LayerDescription>{ProjectionLayerDescription{}});
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, prediction.displayTime.value.count(),
					   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  nullptr};
	Require(session->EndFrame(prediction, &end, layers, output, [] { return XR_SUCCESS; }) == XR_SUCCESS,
			"Submit retained snapshot for reuse");
	output.reset();
	const XrSession previous = session;
	Require(runtime.DestroySession(session) == XR_SUCCESS && !destroyed, "Closed CPU reader lost pending snapshot");
	Require(runtime.CreateSession(instance, session) == XR_SUCCESS && static_cast<XrSession>(session) != previous &&
				destroyed && pool.InUse() == 0,
			"Caller output publication destroyed a native reader under the runtime mutex");
}

void DebuggerLifetime()
{
	for (unsigned teardown = 0; teardown < 3; ++teardown)
	{
		ExtensionRegistry extensions(SimulationExtensions());
		Runtime runtime(extensions);
		Runtime::InstanceRef instance;
		Runtime::SessionRef session;
		auto info = InstanceInfo();
		Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS,
				"Debugger parent graph");
		const XrSession handle = session;
		FramePool pool(4);
		Require(session->ConfigureCapturePool(pool) == XR_SUCCESS, "Debugger native capacity");
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
				"Begin runtime-owned debug session");
		auto invalid = DebugControllerConfig{};
		invalid.ledgerCapacity = 0;
		bool rejected = false;
		try
		{
			runtime.Debugger(handle, invalid);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Invalid controller configuration published a service");
		std::vector<std::future<std::shared_ptr<DebugController>>> requests;
		for (unsigned index = 0; index < 8; ++index)
			requests.push_back(std::async(std::launch::async, [&] { return runtime.Debugger(handle); }));
		auto debugger = requests.front().get();
		Require(debugger != nullptr, "Runtime debugger creation");
		for (std::size_t index = 1; index < requests.size(); ++index)
			Require(requests[index].get() == debugger, "Concurrent creation published multiple controllers");
		Require(!runtime.Debugger(XR_NULL_HANDLE), "Debugger accepted an invalid session");
		auto lease = debugger->Acquire();
		Require(lease.has_value(), "Acquire runtime-owned controller lease");
		auto pause = debugger->Submit(lease->id, {{1}, DebugCommand::Pause});
		Require(pause.result == DebugAdmission::Accepted, "Queue runtime pause");
		Eventually([&] { return debugger->Read(pause.operation)->phase == DebugPhase::Complete; },
				   "Worker pause settlement");
		auto step = debugger->Submit(lease->id, {{2}, DebugCommand::Step});
		Require(step.result == DebugAdmission::Accepted, "Queue runtime step");
		Eventually([&] { return session->Scheduler().Snapshot().permits == 1; },
				   "Worker did not reserve before stepping");
		FramePrediction prediction;
		Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
				"Host admitted debug frame");
		auto output = std::make_shared<ReentrantOutput>();
		output->reservation = session->Captures().ProducerReservation({session->Snapshot().generation, prediction.id});
		Require(output->reservation && pool.Consume(output->reservation),
				"Capture capacity was not reserved before issue");
		std::atomic<unsigned> observations{0};
		output->observe = [&] {
			runtime.FindSession(handle);
			runtime.Instances();
			debugger->Snapshot();
			++observations;
		};
		auto layers = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														  std::vector<LayerDescription>{ProjectionLayerDescription{}});
		XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, prediction.displayTime.value.count(),
						   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  nullptr};
		Require(session->EndFrame(prediction, &end, layers, output, [] { return XR_SUCCESS; }) == XR_SUCCESS,
				"Submit observed pending debug frame");
		Eventually([&] { return observations.load() > 1; }, "Controller did not poll completion independently");
		auto destroy = std::async(std::launch::async, [&] {
			if (teardown == 0)
				return runtime.DestroySession(handle);
			if (teardown == 1)
				return runtime.DestroyInstance(instance);
			runtime.Close();
			return XR_SUCCESS;
		});
		Require(destroy.wait_for(3s) == std::future_status::ready && destroy.get() == XR_SUCCESS,
				"Runtime debugger teardown joined under a lock needed by a GPU observer");
		Require(session->Snapshot().closed && !runtime.FindSession(handle) && !runtime.Debugger(handle),
				"Parent teardown left a live debugger service");
		Require(!debugger->Acquire() && !debugger->Renew(lease->id) &&
					debugger->Submit(lease->id, {{3}, DebugCommand::Resume}).result == DebugAdmission::Closed,
				"Retained controller reopened a closed parent");
		Require(!session->Scheduler().Snapshot().running && !session->Display().Snapshot().running,
				"Runtime teardown resumed scheduler or display admission");
		output->completion = CompletionStatus::Complete;
		session->Display().PollRetired();
		session->Captures().Poll();
		Require(debugger->Read(step.operation)->phase == DebugPhase::Interrupted, "Shutdown left an operation pending");
		std::weak_ptr<Session> lifetime = session.Shared();
		session = {};
		debugger.reset();
		Require(lifetime.expired(), "Runtime service retained a session ownership cycle");
		output.reset();
		Require(pool.InUse() == 0, "Completed shutdown retained a private output reservation");
	}
}
} // namespace

int main()
{
	ApiVersions();
	EnumNames();
	AllocationFreeShutdown();
	GameBridgeLifetime();
	Lifecycle();
	NativeLifetime();
	ReusedSessionReference();
	DebuggerLifetime();
	std::cout << "Runtime graph tests passed\n";
}
