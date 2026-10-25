#include "../TestSupport.h"
#include "control/ConnectionImages.h"
#include "control/ConnectionLeases.h"
#include "control/ConnectionRequests.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/HapticObservation.h"
#include "control/ControlService.h"
#include "control/ControlSessions.h"
#include "control/SimulationControl.h"
#include "runtime/Runtime.h"
#include "runtime/SimulationExtensions.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <new>
#include <thread>
#include <utility>

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
thread_local std::size_t failAllocationSize = 0;
thread_local std::size_t lastAllocationSize = 0;
thread_local int skipMatchingAllocations = 0;
thread_local bool allocationFailed = false;
thread_local bool failNextAllocation = false;
thread_local std::function<void()> onAllocationFailure;
struct MatchingAllocationFailure
{
	MatchingAllocationFailure(std::size_t size, int skip, std::function<void()> action = {})
	{
		failAllocationSize = size;
		skipMatchingAllocations = skip;
		allocationFailed = false;
		onAllocationFailure = std::move(action);
	}
	~MatchingAllocationFailure()
	{
		failAllocationSize = 0;
		onAllocationFailure = {};
	}
};
} // namespace
void* operator new(std::size_t size)
{
	lastAllocationSize = size;
	if (std::exchange(failNextAllocation, false))
	{
		allocationFailed = true;
		throw std::bad_alloc{};
	}
	if (size == failAllocationSize && failAllocationSize != 0 && skipMatchingAllocations-- == 0)
	{
		failAllocationSize = 0;
		allocationFailed = true;
		auto action = std::move(onAllocationFailure);
		if (action)
			action();
		throw std::bad_alloc{};
	}
	if (void* result = std::malloc(size ? size : 1))
		return result;
	throw std::bad_alloc{};
}
void* operator new[](std::size_t size)
{
	return ::operator new(size);
}
[[gnu::noinline]] void operator delete(void* memory) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete[](void* memory) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete[](void* memory, std::size_t) noexcept
{
	std::free(memory);
}
#endif

namespace
{
using namespace simxr;
namespace wire = simxr::control;
using namespace std::chrono_literals;

class TestClock final : public Clock
{
  public:
	~TestClock() override
	{
		if (onDestroyed)
			onDestroyed();
	}
	HostTime Now() const override
	{
		return HostTime{Duration{time.load()}};
	}
	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime) override
	{
		changed.wait_for(lock, 1ms);
	}
	std::atomic<std::int64_t> time{0};
	std::function<void()> onDestroyed;
};
wire::ErrorCode Error(const wire::Message& message)
{
	const auto* error = message.Find(wire::tag::Error);
	Require(message.flags == 1 && error && error->AsU32(), "Missing structured response error");
	return static_cast<wire::ErrorCode>(*error->AsU32());
}
std::uint64_t U64(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	Require(field && field->AsU64(), "Missing uint64 response field");
	return *field->AsU64();
}
std::string_view Text(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	Require(field && field->AsText(), "Missing text response field");
	return *field->AsText();
}
wire::Message Nested(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	Require(field != nullptr, "Missing nested response field");
	auto decoded = wire::DecodeFields(field->value);
	Require(decoded.error == wire::DecodeError::None, "Malformed nested response");
	wire::Message result;
	result.fields = std::move(decoded.fields);
	return result;
}

struct Client
{
	wire::ControlService& service;
	wire::ConnectionId connection;
	std::uint64_t next = 1, session = 0, generation = 0, lease = 0;
	explicit Client(wire::ControlService& service) : service(service)
	{
		const auto opened = service.Connect();
		Require(opened.has_value(), "Open control connection");
		connection = *opened;
	}
	wire::Message Request(wire::OpCode opcode, std::vector<wire::Field> fields = {})
	{
		wire::Message message;
		message.opcode = static_cast<std::uint16_t>(opcode);
		message.requestId = wire::RequestId{next++};
		message.fields = std::move(fields);
		return message;
	}
	wire::Message Call(wire::OpCode opcode, std::vector<wire::Field> fields = {})
	{
		const auto request = Request(opcode, std::move(fields));
		const auto response = service.Handle(connection, request);
		Require(response.requestId == request.requestId && response.opcode == request.opcode &&
					wire::Decode(wire::Encode(response)).message.has_value(),
				"Response identity/framing changed");
		return response;
	}
	void Hello()
	{
		Require(Error(Call(wire::OpCode::Hello, {wire::Field::Text(wire::tag::ClientName, "portable tests")})) ==
					wire::ErrorCode::Ok,
				"Negotiate control connection");
	}
	std::vector<wire::Field> Common() const
	{
		return {wire::Field::U64(wire::tag::Session, session), wire::Field::U64(wire::tag::Generation, generation)};
	}
	std::vector<wire::Field> Leased() const
	{
		auto result = Common();
		result.push_back(wire::Field::U64(wire::tag::Lease, lease));
		return result;
	}
	void Discover()
	{
		const auto response = Call(wire::OpCode::Sessions);
		Require(Error(response) == wire::ErrorCode::Ok, "Discover control sessions");
		const auto sessions = Nested(response, wire::tag::Sessions);
		Require(sessions.fields.size() == 1, "Expected one live core session");
		const auto entry = Nested(sessions, sessions.fields.front().tag);
		session = U64(entry, wire::tag::Session);
		generation = U64(entry, wire::tag::Generation);
	}
	void Acquire()
	{
		const auto response = Call(wire::OpCode::Acquire, Common());
		Require(Error(response) == wire::ErrorCode::Ok, "Acquire session control lease");
		lease = U64(response, wire::tag::Lease);
	}
	wire::Message Submit(std::uint64_t id, wire::CommandKind kind, std::vector<wire::Field> extras = {})
	{
		auto fields = Leased();
		fields.push_back(wire::Field::U64(wire::tag::CommandId, id));
		fields.push_back(wire::Field::U32(wire::tag::CommandKind, static_cast<std::uint32_t>(kind)));
		fields.insert(fields.end(), extras.begin(), extras.end());
		return Call(wire::OpCode::Submit, std::move(fields));
	}
	wire::Message Operation(std::uint64_t id)
	{
		auto fields = Common();
		fields.push_back(wire::Field::U64(wire::tag::OperationId, id));
		return Call(wire::OpCode::Operation, std::move(fields));
	}
	wire::Message Capture(std::uint64_t id)
	{
		auto fields = Common();
		fields.push_back(wire::Field::U64(wire::tag::CaptureId, id));
		return Call(wire::OpCode::Capture, std::move(fields));
	}
};

struct Fixture
{
	ExtensionRegistry extensions{SimulationExtensions()};
	Runtime runtime{extensions};
	std::shared_ptr<TestClock> clock = std::make_shared<TestClock>();
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	std::shared_ptr<DebugController> controller;
	wire::ControlService service{runtime, 2};
	FramePool pool{8};
	FrameSchedulerConfig config;
	explicit Fixture(bool capture = true, bool begin = true)
	{
		XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
		info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
		std::strcpy(info.applicationInfo.applicationName, "Control service tests");
		Require(runtime.CreateInstance(&info, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session, clock) == XR_SUCCESS,
				"Create control runtime/session");
		if (capture)
			Require(session->ConfigureCapturePool(pool) == XR_SUCCESS, "Configure fake composed capture capability");
		config.mode = ExecutionMode::Deterministic;
		config.epoch = RuntimeTime{100ns};
		config.period = 10ns;
		if (begin)
			Begin();
		DebugControllerConfig debugging;
		debugging.startWorker = false;
		controller = runtime.Debugger(session, debugging);
		Require(controller != nullptr, "Create independently polled real debugger");
	}
	void Begin()
	{
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
				"Begin control session");
	}
	void Restart()
	{
		Require(session->RequestExit(config.epoch) == XR_SUCCESS && session->End(config.epoch) == XR_SUCCESS,
				"End session generation");
		Begin();
	}
	FramePrediction Issue()
	{
		FramePrediction frame;
		Require(session->WaitFrame(frame) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
				"Issue controlled frame");
		return frame;
	}
	void Clear(FramePrediction frame)
	{
		Require(session->EndFrame(frame) == XR_SUCCESS, "Submit controlled zero-layer frame");
		controller->Poll();
	}
	~Fixture()
	{
		service.Close();
		runtime.Close();
	}
};

void LiveHaptics()
{
	Fixture f;
	Client client(f.service);
	client.Hello();
	client.Discover();
	const std::array devices{HapticDevice::LeftController, HapticDevice::RightController};
	const auto action = reinterpret_cast<XrAction>(1);
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_INFINITE_DURATION, 120, .5f};
	Require(f.session->Haptics().Apply(devices, action, XR_NULL_PATH, vibration, f.config.epoch, true) == XR_SUCCESS,
		"Apply live haptic output before any frame submission");
	const auto read = [&]
	{
		const auto response = client.Call(wire::OpCode::ReadHaptics, client.Common());
		Require(Error(response) == wire::ErrorCode::Ok, "Read live haptics without debugger lease");
		return wire::DecodeHaptics(response);
	};
	auto observed = read();
	Require(observed.events.size() == 2 && observed.events[0].device == devices[0] &&
		observed.events[1].device == devices[1] && observed.events[1].amplitude == .5f &&
		!observed.events[1].end && observed.events[1].status == HapticStatus::Active && !observed.overflow,
		"Live haptics preserve targets, parameters and infinite duration");
	vibration.duration = 10;
	Require(f.session->Haptics().Apply(std::span(devices).first(1), action, XR_NULL_PATH, vibration, f.config.epoch,
		true) == XR_SUCCESS, "Replace left haptic output");
	Require(f.session->Haptics().Stop(std::span(devices).last(1), action, f.config.epoch, true) == XR_SUCCESS,
		"Stop right haptic output");
	observed = read();
	Require(observed.events.size() == 3 && observed.events[0].status == HapticStatus::Replaced &&
		observed.events[1].status == HapticStatus::Stopped && observed.events[2].end == 110,
		"Live observation retains stop and replacement events");
	f.session->Haptics().LoseFocus(f.config.epoch);
	Require(read().events.back().status == HapticStatus::FocusLost, "Focus loss retires live output");
	for (int i = 0; i < 300; ++i)
		Require(f.session->Haptics().Apply(std::span(devices).first(1), action, XR_NULL_PATH, vibration,
			f.config.epoch, true) == XR_SUCCESS, "Fill bounded haptic history");
	observed = read();
	Require(observed.events.size() == wire::MaxObservedHaptics && observed.overflow &&
		observed.events.back().status == HapticStatus::Active, "Bound live history while retaining latest output");
	f.Restart();
	Require(Error(client.Call(wire::OpCode::ReadHaptics, client.Common())) == wire::ErrorCode::StaleGeneration,
		"Reject previous-run haptic reads");
	client.Discover();
	Require(read().events.empty(), "Session restart clears live haptic history");
	Client old(f.service);
	auto hello = old.Request(wire::OpCode::Hello);
	hello.minor = 5;
	const auto response = f.service.Handle(old.connection, hello);
	Require(!(U64(response, wire::tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::LiveHaptics)),
		"Older clients must not be offered live haptics");
	auto request = old.Request(wire::OpCode::ReadHaptics, client.Common());
	request.minor = 5;
	Require(Error(f.service.Handle(old.connection, request)) == wire::ErrorCode::UnsupportedCommand,
		"Older protocol rejects live haptic operation");
}

void ContinuousFrameHistory()
{
	Fixture f;
	Client client(f.service);
	client.Hello();
	client.Discover();
	const auto first = f.Issue();
	const auto empty = client.Call(wire::OpCode::ReadFrameHistory, client.Common());
	if (Error(empty) != wire::ErrorCode::Ok)
		throw std::runtime_error(std::string(Text(empty, wire::tag::Detail)));
	Require(Nested(empty, wire::tag::Frames).fields.empty(),
		"Unsettled frame entered continuous history");
	f.Clear(first);
	const auto second = f.Issue();
	f.Clear(second);
	const auto batch = client.Call(wire::OpCode::ReadFrameHistory, client.Common());
	Require(Error(batch) == wire::ErrorCode::Ok && Nested(batch, wire::tag::Frames).fields.size() == 2,
		"Multiple frame summaries collided in the wire page");
	auto fields = client.Common();
	fields.push_back(wire::Field::U64(wire::tag::Length, 1));
	const auto response = client.Call(wire::OpCode::ReadFrameHistory, fields);
	const auto frames = Nested(response, wire::tag::Frames);
	Require(Error(response) == wire::ErrorCode::Ok && frames.fields.size() == 1,
		"Continuous history pagination failed");
	const auto frame = Nested(frames, frames.fields.front().tag);
	Require(U64(frame, wire::tag::FrameId) == first.id.value &&
		Nested(frame, wire::tag::Prediction).Find(wire::tag::Time)->AsI64() == first.displayTime.value.count(),
		"Continuous history lost runtime frame identity or timestamp");
	fields.push_back(wire::Field::U64(wire::tag::Offset, first.id.value));
	const auto next = Nested(client.Call(wire::OpCode::ReadFrameHistory, fields), wire::tag::Frames);
	Require(next.fields.size() == 1 && U64(Nested(next, next.fields.front().tag), wire::tag::FrameId) == second.id.value,
		"Continuous history skipped or repeated a clear frame");
	fields = client.Common();
	fields.push_back(wire::Field::U64(wire::tag::Length, 129));
	Require(Error(client.Call(wire::OpCode::ReadFrameHistory, fields)) == wire::ErrorCode::Invalid,
		"Continuous history accepted an unbounded request");
	f.Restart();
	Require(Error(client.Call(wire::OpCode::ReadFrameHistory, client.Common())) == wire::ErrorCode::StaleGeneration,
		"Continuous history crossed a run boundary");
	client.Discover();
	const auto restarted = f.Issue();
	f.Clear(restarted);
	const auto newPage = Nested(client.Call(wire::OpCode::ReadFrameHistory, client.Common()), wire::tag::Frames);
	Require(newPage.fields.size() == 1 && U64(Nested(newPage, newPage.fields.front().tag), wire::tag::Generation) == client.generation,
		"New run history included retained records from the previous generation");
}
void RecordingPagesShrinkBeforeEncoding()
{
	Fixture f;
	Client client(f.service);
	client.Hello();
	client.Discover();
	Require(Error(client.Call(wire::OpCode::StartRecording, client.Common())) == wire::ErrorCode::Ok,
		"Start large recording fixture");
	BodyControls body;
	for (auto& joint : body.joints)
		joint = BodyJointOverride{XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true};
	CommandOutcome outcome;
	outcome.transaction.source = {CommandSourceKind::Mcp, client.connection.value};
	outcome.transaction.commands.assign(8, SetBodyControls{body});
	for (std::uint64_t id = 1; id <= 16; ++id)
	{
		outcome.transaction.id = id;
		Require(f.session->Commands().Recorder().Observe(outcome), "Record large transaction");
	}
	Require(wire::SimulationRecording(f.session->Commands().Recorder().Snapshot(0, 1)).Encode().size() <
		wire::MaxPayload - 4096, "Single transaction must fit the wire budget");
	bool oversized = false;
	try
	{
		wire::SimulationRecording(f.session->Commands().Recorder().Snapshot(0, 16));
	}
	catch (const std::length_error&)
	{
		oversized = true;
	}
	Require(oversized, "Recording fixture must exceed the document construction budget");
	std::uint64_t offset = 0;
	while (offset < 16)
	{
		auto fields = client.Common();
		fields.push_back(wire::Field::U64(wire::tag::Offset, offset));
		fields.push_back(wire::Field::U64(wire::tag::Length, 16));
		const auto response = client.Call(wire::OpCode::ReadRecording, std::move(fields));
		Require(Error(response) == wire::ErrorCode::Ok, "Oversized recording page did not shrink");
		const auto count = U64(response, wire::tag::Length);
		Require(U64(response, wire::tag::Offset) == offset && count && count < 16 && count <= 16 - offset,
			"Shrunk recording page lost progress or its offset");
		offset += count;
		Require(response.Find(wire::tag::End)->AsBool() == (offset == 16), "Incorrect recording end marker");
	}
}
void NegotiationPublicationRollback()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	int failures = 0;
	bool exhausted = false;
	for (int skip = 0; skip < 32; ++skip)
	{
		Fixture f;
		Client client(f.service);
		const auto hello = client.Request(wire::OpCode::Hello);
		wire::Message response;
		bool failed = false;
		{
			// Hello publishes four fields; include the replay-cache copy.
			MatchingAllocationFailure failure(4 * sizeof(wire::Field), skip);
			response = f.service.Handle(client.connection, hello);
			failed = allocationFailed;
		}
		if (!failed)
		{
			Require(Error(response) == wire::ErrorCode::Ok, "Hello failed without allocation injection");
			exhausted = true;
			break;
		}
		++failures;
		Require(Error(response) == wire::ErrorCode::Capacity, "Hello allocation failure was not reported");
		const auto retry = f.service.Handle(client.connection, hello);
		Require(Error(retry) == wire::ErrorCode::Ok, "Failed Hello publication stranded negotiation");
		Require(wire::Encode(f.service.Handle(client.connection, hello)) == wire::Encode(retry),
			"Retried Hello did not publish an exact replay");
		Require(Error(client.Call(wire::OpCode::Sessions)) == wire::ErrorCode::Ok,
			"Retried Hello did not enable connection requests");
	}
	Require(exhausted && failures > 0, "Hello publication allocation failure was not exercised");
#endif
}
void NegotiationAndReplay()
{
	Fixture f;
	Client client(f.service), other(f.service);
	Require(!f.service.Connect(), "Control service exceeded connection capacity");
	Require(Error(client.Call(wire::OpCode::Sessions)) == wire::ErrorCode::NotNegotiated,
			"Unnegotiated connection performed discovery");
	auto hello =
		client.Request(wire::OpCode::Hello, {wire::Field::Text(wire::tag::ClientName, "\xe7\x9c\xbc"),
											 wire::Field::U64(wire::tag::OptionalCapabilities, ~std::uint64_t{0}),
											 wire::Field::U32(65000, 9)});
	hello.minor = 22;
	const auto response = f.service.Handle(client.connection, hello);
	Require(Error(response) == wire::ErrorCode::Ok && response.Find(wire::tag::Minor)->AsU32() == wire::ProtocolMinor &&
				U64(response, wire::tag::Capabilities) ==
					(static_cast<std::uint64_t>(wire::Capability::DebugCommands) |
					 static_cast<std::uint64_t>(wire::Capability::SnapshotMetadata) |
				 static_cast<std::uint64_t>(wire::Capability::SnapshotPins) |
					 static_cast<std::uint64_t>(wire::Capability::PipelineInspection) |
					 static_cast<std::uint64_t>(wire::Capability::HistoricalCpuState) |
					 static_cast<std::uint64_t>(wire::Capability::ImmutableRevisions) |
					 static_cast<std::uint64_t>(wire::Capability::SimulationCommands) |
					 static_cast<std::uint64_t>(wire::Capability::CommandRecording) |
					 static_cast<std::uint64_t>(wire::Capability::FrameDiagnostics) |
					 static_cast<std::uint64_t>(wire::Capability::ProjectionHistory) |
					 static_cast<std::uint64_t>(wire::Capability::FrameHistory) |
					 static_cast<std::uint64_t>(wire::Capability::PoseSequences) |
					 static_cast<std::uint64_t>(wire::Capability::LiveHaptics)) &&
				response.Find(wire::tag::PayloadLimit)->AsU32() == wire::MaxPayload,
			"Hello negotiation/capability limits changed");
	Require(wire::Encode(f.service.Handle(client.connection, hello)) == wire::Encode(response),
			"Exact envelope retry did not replay the same response");
	auto changed = hello;
	changed.fields.push_back(wire::Field::U32(64000, 1));
	Require(Error(f.service.Handle(client.connection, changed)) == wire::ErrorCode::Conflict,
			"Same request ID changed payload accepted");
	Require(Error(client.Call(wire::OpCode::Hello)) == wire::ErrorCode::Conflict, "Connection renegotiated");
	Require(Error(f.service.Handle(client.connection, hello)) == wire::ErrorCode::Conflict,
			"Old envelope executed after replay cache advanced");
	client.Discover();
	Require(client.session != reinterpret_cast<std::uintptr_t>(static_cast<XrSession>(f.session)) &&
				client.generation == f.session->Snapshot().generation.value,
			"Discovery leaked a native handle identity");
	auto unknown = client.Request(wire::OpCode::Sessions);
	unknown.opcode = 60000;
	Require(Error(f.service.Handle(client.connection, unknown)) == wire::ErrorCode::UnsupportedCommand,
			"Unknown opcode lacked explicit error");
	Require(Error(client.Call(wire::OpCode::Sessions, {wire::Field::U32(65001, 0, true)})) ==
				wire::ErrorCode::UnsupportedField,
			"Unknown critical field accepted");
	Require(Error(client.Call(wire::OpCode::Sessions, {wire::Field::U32(65001, 0)})) == wire::ErrorCode::Ok,
			"Unknown optional field rejected");
	auto unsupported = client.Request(wire::OpCode::Sessions);
	unsupported.major = 2;
	Require(Error(f.service.Handle(client.connection, unsupported)) == wire::ErrorCode::UnsupportedVersion,
			"Unsupported major did not map to version error");
	Require(Error(other.Call(wire::OpCode::Hello,
							 {wire::Field::U64(wire::tag::RequiredCapabilities,
											   static_cast<std::uint64_t>(wire::Capability::FullFrameState))})) ==
				wire::ErrorCode::UnsupportedCommand,
			"Service advertised omitted full-frame metadata");
	other.Hello();
	f.service.Disconnect(other.connection);
	Require(Error(f.service.Handle(other.connection, other.Request(wire::OpCode::Sessions))) ==
					wire::ErrorCode::Closed &&
				f.service.Connect().has_value(),
			"Disconnected connection was active or did not free capacity");
}

void LegacyMinorNegotiation()
{
	Fixture f;
	Client client(f.service);
	auto hello = client.Request(wire::OpCode::Hello);
	hello.minor = 0;
	const auto response = f.service.Handle(client.connection, hello);
	Require(Error(response) == wire::ErrorCode::Ok && response.minor == 0 &&
				response.Find(wire::tag::Minor)->AsU32() == 0,
			"Legacy Hello response did not use negotiated minor zero");
	const auto sessions = client.Call(wire::OpCode::Sessions);
	Require(Error(sessions) == wire::ErrorCode::Ok && sessions.minor == 0,
			"Subsequent response silently upgraded negotiated minor");
	auto unknown = client.Request(wire::OpCode::Sessions);
	unknown.opcode = 60000;
	const auto rejected = f.service.Handle(client.connection, unknown);
	Require(Error(rejected) == wire::ErrorCode::UnsupportedCommand && rejected.minor == 0,
			"Command error silently upgraded negotiated minor");
	Client previous(f.service);
	auto versionFour = previous.Request(wire::OpCode::Hello);
	versionFour.minor = 4;
	const auto negotiated = f.service.Handle(previous.connection, versionFour);
	Require(Error(negotiated) == wire::ErrorCode::Ok && negotiated.minor == 4 &&
		!(U64(negotiated, wire::tag::Capabilities) & (static_cast<std::uint64_t>(wire::Capability::PoseSequences) |
					 static_cast<std::uint64_t>(wire::Capability::LiveHaptics))) &&
		Error(previous.Call(wire::OpCode::UploadSequence)) == wire::ErrorCode::UnsupportedCommand,
		"Minor four advertised or admitted sequence control");
}

void LeaseOwnerRetirementAndRollback()
{
	Fixture fixture;
	wire::ConnectionLeases owner;
	wire::ConnectionLeases::Retired retired;
	const wire::ControlSessionId sessionId{1};
	const auto generation = fixture.session->Snapshot().generation;
	const auto lease = owner.Acquire(sessionId, fixture.controller, *fixture.session.Shared(), generation, retired);
	Require(owner.Acquire(sessionId, fixture.controller, *fixture.session.Shared(), generation, retired).id == lease.id,
			"Repeated acquisition replaced the connection's live lease");
	wire::ConnectionLeases other;
	bool copiedLeaseRejected = false;
	try
	{
		other.Renew(sessionId, lease.id, *fixture.session.Shared(), generation);
	}
	catch (const wire::ControlRequestError& error)
	{
		copiedLeaseRejected = error.error == wire::ErrorCode::LeaseRequired;
	}
	Require(copiedLeaseRejected, "Lease owner accepted a copied native lease ID");
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	bool retirementFailed = false;
	{
		MatchingAllocationFailure failure(sizeof(std::shared_ptr<DebugController>), 0);
		try
		{
			owner.Prune({}, retired);
		}
		catch (const std::bad_alloc&)
		{
			retirementFailed = allocationFailed;
		}
	}
	Require(retirementFailed, "Lease-retirement allocation failure was not exercised");
	owner.Renew(sessionId, lease.id, *fixture.session.Shared(), generation);
	Require(fixture.controller->Snapshot().lease->id == lease.id,
			"Failed retirement partially removed the active lease");
#endif
	owner.Close();
	owner.Close();
	fixture.controller->Poll();
	Require(!fixture.controller->Snapshot().lease, "Closing the owner retained its controller lease");
	bool closedRejected = false;
	try
	{
		owner.Acquire(sessionId, fixture.controller, *fixture.session.Shared(), generation, retired);
	}
	catch (const wire::ControlRequestError& error)
	{
		closedRejected = error.error == wire::ErrorCode::Closed;
	}
	Require(closedRejected, "Closed lease owner admitted a new native lease");
}

void OwnershipAndExactClear()
{
	Fixture f;
	Client client(f.service), other(f.service);
	client.Hello();
	other.Hello();
	client.Discover();
	other.Discover();
	client.Acquire();
	Require(Error(other.Call(wire::OpCode::Acquire, other.Common())) == wire::ErrorCode::Busy,
			"Two clients acquired one controller lease");
	other.lease = client.lease;
	Require(Error(other.Call(wire::OpCode::Renew, other.Leased())) == wire::ErrorCode::LeaseRequired &&
				Error(other.Submit(1, wire::CommandKind::Pause)) == wire::ErrorCode::LeaseRequired,
			"Another connection used copied lease identity");
	Require(Error(client.Call(wire::OpCode::Renew, client.Leased())) == wire::ErrorCode::Ok, "Renew own lease");
	auto first = f.Issue();
	const auto admitted = client.Submit(1, wire::CommandKind::Pause);
	const auto pause = U64(admitted, wire::tag::OperationId);
	Require(Error(admitted) == wire::ErrorCode::Ok && Text(client.Operation(pause), wire::tag::Phase) == "queued",
			"Pause admission blocked for settlement");
	f.controller->Poll();
	const auto pending = client.Operation(pause);
	Require(Text(pending, wire::tag::Phase) == "pending" && Text(pending, wire::tag::Progress) == "pausing" &&
				!Nested(pending, wire::tag::Outstanding).fields.empty(),
			"Pause falsely settled with begun application frame");
	const auto status = client.Call(wire::OpCode::Status, client.Common());
	Require(
		Error(status) == wire::ErrorCode::Ok && status.Find(wire::tag::Manual)->AsBool() == true &&
			Nested(status, wire::tag::Pipeline).Find(wire::tag::Begun) &&
			(U64(status, wire::tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::ComposedCapture)) &&
			!(U64(status, wire::tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::ImageExport)),
		"Pending status blocked or misstated capture/export capabilities");
	f.Clear(first);
	Require(Text(client.Operation(pause), wire::tag::Phase) == "complete",
			"Pause did not settle after actual clear submission");
	const auto duplicate = client.Submit(1, wire::CommandKind::Pause);
	Require(Error(duplicate) == wire::ErrorCode::Ok && U64(duplicate, wire::tag::OperationId) == pause &&
				duplicate.Find(wire::tag::Duplicate)->AsBool() == true,
			"New envelope reran existing controller command");
	Require(Error(client.Submit(1, wire::CommandKind::Resume)) == wire::ErrorCode::Conflict,
			"Changed controller command reused old ID");
	const auto step = U64(client.Submit(2, wire::CommandKind::Step), wire::tag::OperationId);
	f.controller->Poll();
	const auto stepped = client.Operation(step);
	Require(Text(stepped, wire::tag::Phase) == "complete" && Text(stepped, wire::tag::Progress) == "clear" &&
				f.session->Scheduler().Snapshot().permits == 0,
			"Step did not inspect retained submission before new admission");
	const auto captureId = U64(stepped, wire::tag::CaptureId);
	const auto captured = client.Capture(captureId);
	const auto metadata = Nested(captured, wire::tag::Frame);
	const auto submission = Nested(metadata, wire::tag::Submission);
	Require(
		Text(captured, wire::tag::Phase) == "clear" && captured.Find(wire::tag::ImageAvailable)->AsBool() == false &&
			U64(metadata, wire::tag::FrameId) == first.id.value &&
			U64(metadata, wire::tag::Generation) == client.generation &&
			Text(metadata, wire::tag::Stage) == "submitted" &&
			Text(submission, wire::tag::OutputDisposition) == "clear" &&
			Text(submission, wire::tag::DropReason) == "none" && submission.Find(wire::tag::LayerCount)->AsU32() == 0 &&
			submission.Find(wire::tag::Time)->AsI64() == first.displayTime.value.count(),
		"Capture lost exact immutable frame identity/submission disposition");
	auto inspectFields = client.Common();
	inspectFields.push_back(wire::Field::U64(wire::tag::CaptureId, captureId));
	const auto inspected = client.Call(wire::OpCode::InspectFrame, inspectFields);
	Require(Error(inspected) == wire::ErrorCode::Ok && inspected.Find(wire::tag::Document) &&
				U64(Nested(inspected, wire::tag::Frame), wire::tag::FrameId) == first.id.value &&
				U64(client.Call(wire::OpCode::InspectFrame, inspectFields), wire::tag::InspectionId) ==
					U64(inspected, wire::tag::InspectionId),
			"CPU inspection changed exact frame identity or allocated a second reader");
	const auto inspectionId = U64(inspected, wire::tag::InspectionId);
	Require(f.session->Scheduler().Snapshot().permits == 0, "CPU inspection granted an application permit");
	const auto nextStep = U64(client.Submit(3, wire::CommandKind::Step), wire::tag::OperationId);
	f.controller->Poll();
	Require(f.session->Scheduler().Snapshot().permits == 1 &&
				Text(client.Operation(nextStep), wire::tag::Phase) == "pending",
			"Next step did not reserve one exact future admission");
	const auto cancel =
		U64(client.Submit(4, wire::CommandKind::Cancel, {wire::Field::U64(wire::tag::OperationId, nextStep)}),
			wire::tag::OperationId);
	f.controller->Poll();
	Require(Text(client.Operation(cancel), wire::tag::Phase) == "complete" &&
				Text(client.Operation(nextStep), wire::tag::Phase) == "cancelled" &&
				f.session->Scheduler().Snapshot().permits == 0,
			"Cancellation stranded unused permit or operation");
	const auto release =
		U64(client.Submit(5, wire::CommandKind::Release, {wire::Field::U64(wire::tag::CaptureId, captureId)}),
			wire::tag::OperationId);
	f.controller->Poll();
	Require(Text(client.Operation(release), wire::tag::Progress) == "released" &&
				Error(client.Capture(captureId)) == wire::ErrorCode::NotFound,
			"Released logical snapshot remained readable");
	const auto releasedInspection =
		client.Call(wire::OpCode::ReleaseInspection, {wire::Field::U64(wire::tag::InspectionId, inspectionId)});
	Require(Error(releasedInspection) == wire::ErrorCode::Ok &&
				releasedInspection.Find(wire::tag::Released)->AsBool() == true &&
				client.Call(wire::OpCode::ReleaseInspection, {wire::Field::U64(wire::tag::InspectionId, inspectionId)})
						.Find(wire::tag::Released)
						->AsBool() == false,
			"CPU inspection reader release was not independently idempotent");
	auto ack = client.Leased();
	ack.push_back(wire::Field::U64(wire::tag::CommandId, 5));
	Require(Error(client.Call(wire::OpCode::Acknowledge, ack)) == wire::ErrorCode::Ok &&
				Error(client.Operation(step)) == wire::ErrorCode::NotFound &&
				Error(client.Submit(2, wire::CommandKind::Step)) == wire::ErrorCode::Conflict,
			"Acknowledged command executed again or retained ledger");
	const auto resume = U64(client.Submit(6, wire::CommandKind::Resume), wire::tag::OperationId);
	f.controller->Poll();
	Require(Text(client.Operation(resume), wire::tag::Progress) == "resumed" &&
				!f.session->Scheduler().Snapshot().paused && !f.session->Display().Paused(),
			"Resume did not restore original execution mode");
	Require(Error(client.Call(wire::OpCode::Relinquish, client.Leased())) == wire::ErrorCode::Ok,
			"Relinquish control lease");
	f.controller->Poll();
	other.Acquire();
	const auto pauseOther = U64(other.Submit(1, wire::CommandKind::Pause), wire::tag::OperationId);
	f.controller->Poll();
	Require(Text(other.Operation(pauseOther), wire::tag::Phase) == "complete", "Second controller could not pause");
	f.service.Disconnect(other.connection);
	f.controller->Poll();
	Require(!f.session->Scheduler().Snapshot().paused && !f.controller->Snapshot().lease,
			"Disconnect did not recover session and relinquish lease");
}

void InspectionResponseAllocationRollback()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	Fixture f;
	Client client(f.service);
	client.Hello();
	client.Discover();
	client.Acquire();
	const auto frame = f.Issue();
	client.Submit(1, wire::CommandKind::Pause);
	f.controller->Poll();
	f.Clear(frame);
	const auto step = U64(client.Submit(2, wire::CommandKind::Step), wire::tag::OperationId);
	f.controller->Poll();
	const auto capture = U64(client.Operation(step), wire::tag::CaptureId);
	auto fields = client.Common();
	fields.push_back(wire::Field::U64(wire::tag::CaptureId, capture));
	const auto baseline = client.Call(wire::OpCode::InspectFrame, fields);
	Require(Error(baseline) == wire::ErrorCode::Ok, "Prepare inspection allocation regression");
	// Measure the vector allocation, including any standard-library alignment overhead.
	const auto document = baseline.Find(wire::tag::Document)->value;
	const auto size = lastAllocationSize;
	const auto firstId = U64(baseline, wire::tag::InspectionId);
	Require(client.Call(wire::OpCode::ReleaseInspection, {wire::Field::U64(wire::tag::InspectionId, firstId)})
					.Find(wire::tag::Released)
					->AsBool() == true,
			"Release baseline inspection");
	int failures = 0;
	bool exhausted = false;
	// The exact document-sized allocations include the serialized record,
	// outgoing Document field, and replay-cache copy. Fail each occurrence.
	for (int skip = 0; skip != 32; ++skip)
	{
		auto request = client.Request(wire::OpCode::InspectFrame, fields);
		wire::Message response;
		bool failed;
		{
			MatchingAllocationFailure failure(size, skip);
			response = f.service.Handle(client.connection, request);
			failed = allocationFailed;
		}
		if (!failed)
		{
			Require(Error(response) == wire::ErrorCode::Ok, "Inspection failed without injected allocation");
			client.Call(wire::OpCode::ReleaseInspection,
						{wire::Field::U64(wire::tag::InspectionId, U64(response, wire::tag::InspectionId))});
			exhausted = true;
			break;
		}
		++failures;
		Require(Error(response) == wire::ErrorCode::Capacity, "Allocation failure lacked structured capacity error");
		// A failed export must not publish any undisclosed reader, even when
		// the catalog commit preceded the outgoing field/cache allocation.
		for (std::uint64_t id = firstId + 1; id <= firstId + skip + 1; ++id)
			Require(client.Call(wire::OpCode::ReleaseInspection, {wire::Field::U64(wire::tag::InspectionId, id)})
							.Find(wire::tag::Released)
							->AsBool() == false,
					"Failed response leaked an unannounced inspection reader");
	}
	Require(exhausted && failures >= 2, "Document response and cache allocation faults were not both exercised");
#endif
}

void HistoricalInspectionAfterSessionDestroy()
{
	Fixture f;
	Client client(f.service), observer(f.service);
	client.Hello();
	observer.Hello();
	client.Discover();
	client.Acquire();
	const auto first = f.Issue();
	client.Submit(1, wire::CommandKind::Pause);
	f.controller->Poll();
	f.Clear(first);
	const auto step = U64(client.Submit(2, wire::CommandKind::Step), wire::tag::OperationId);
	f.controller->Poll();
	const auto capture = U64(client.Operation(step), wire::tag::CaptureId);
	auto fields = client.Common();
	fields.push_back(wire::Field::U64(wire::tag::CaptureId, capture));
	const auto inspected = client.Call(wire::OpCode::InspectFrame, fields);
	Require(Error(inspected) == wire::ErrorCode::Ok, "Retain independent CPU inspection before teardown");
	const auto document = Nested(inspected, wire::tag::Document);
	const auto members = Nested(document, 3);
	std::uint64_t revision = 0;
	for (const auto& member : members.fields)
	{
		const auto node = Nested(members, member.tag);
		if (Text(node, 1) == "view_settings")
			revision = U64(node, 3);
	}
	Require(revision != 0, "View revision reference missing");
	Require(f.runtime.DestroySession(f.session) == XR_SUCCESS, "Destroy inspected session");
	const auto read = client.Call(wire::OpCode::FeatureRevision, {wire::Field::U64(wire::tag::RevisionId, revision)});
	Require(Error(read) == wire::ErrorCode::Ok && Text(read, wire::tag::RevisionKind) == "view_settings_v1" &&
				read.Find(wire::tag::Document),
			"Immutable feature revision depended on a discoverable session");
	Require(Error(observer.Call(wire::OpCode::FeatureRevision, {wire::Field::U64(wire::tag::RevisionId, revision)})) ==
				wire::ErrorCode::NotFound,
			"Revision identity escaped its connection");
	const auto released =
		client.Call(wire::OpCode::ReleaseInspection,
					{wire::Field::U64(wire::tag::InspectionId, U64(inspected, wire::tag::InspectionId))});
	Require(released.Find(wire::tag::Released)->AsBool() == true &&
				Error(client.Call(wire::OpCode::FeatureRevision,
								  {wire::Field::U64(wire::tag::RevisionId, revision)})) == wire::ErrorCode::NotFound,
			"Released inspection kept revisions discoverable");
}

void CapabilityAndValidation()
{
	Fixture f(false);
	Client client(f.service);
	client.Hello();
	client.Discover();
	client.Acquire();
	const auto status = client.Call(wire::OpCode::Status, client.Common());
	const auto unsupported = static_cast<std::uint64_t>(wire::Capability::ComposedCapture) |
							 static_cast<std::uint64_t>(wire::Capability::LayerViewCapture) |
							 static_cast<std::uint64_t>(wire::Capability::GpuPreview) |
							 static_cast<std::uint64_t>(wire::Capability::ImageExport) |
							 static_cast<std::uint64_t>(wire::Capability::FullFrameState) |
							 static_cast<std::uint64_t>(wire::Capability::DepthCapture) |
							 static_cast<std::uint64_t>(wire::Capability::RotationalReprojection);
	Require((U64(status, wire::tag::Capabilities) & unsupported) == 0,
			"Unconfigured backend advertised unavailable images or omitted full state");
	const auto unknown = client.Submit(1, static_cast<wire::CommandKind>(0xffffffff));
	Require(Error(unknown) == wire::ErrorCode::UnsupportedCommand && f.controller->Snapshot().commands == 0,
			"Unknown command value reached core enum dispatch");
	const std::array target{
		wire::Field::U32(wire::tag::CaptureKind, static_cast<std::uint32_t>(wire::TargetKind::LayerView)),
		wire::Field::U32(65000, 7, true)};
	Require(Error(client.Submit(2, wire::CommandKind::Step,
								{wire::Field::Bytes(wire::tag::Target, wire::EncodeFields(target))})) ==
					wire::ErrorCode::UnsupportedField &&
				f.controller->Snapshot().commands == 0 && !f.session->Scheduler().Snapshot().paused,
			"Unknown critical nested target caused command side effects");
	client.Submit(3, wire::CommandKind::Pause);
	f.controller->Poll();
	const auto step = U64(client.Submit(4, wire::CommandKind::Step), wire::tag::OperationId);
	f.controller->Poll();
	const auto operation = client.Operation(step);
	Require(Text(operation, wire::tag::Phase) == "failed" && Text(operation, wire::tag::Progress) == "unsupported" &&
				f.session->Scheduler().Snapshot().permits == 0,
			"Unsupported capture granted an admission or fabricated an image");
}

void GenerationAndTeardown()
{
	Fixture f;
	Client client(f.service);
	client.Hello();
	client.Discover();
	client.Acquire();
	const auto oldSession = client.session, oldGeneration = client.generation;
	const auto queued = U64(client.Submit(1, wire::CommandKind::Pause), wire::tag::OperationId);
	f.Restart();
	f.controller->Poll();
	Require(!f.session->Scheduler().Snapshot().paused && !f.session->Display().Paused(),
			"Queued old-generation command mutated restarted session");
	Require(Error(client.Call(wire::OpCode::Status, client.Common())) == wire::ErrorCode::StaleGeneration,
			"Old session generation accepted");
	client.Discover();
	Require(client.session == oldSession && client.generation > oldGeneration,
			"Session restart changed opaque identity or reused generation");
	const auto stale = client.Operation(queued);
	Require(Error(stale) == wire::ErrorCode::Ok && Text(stale, wire::tag::Phase) == "failed",
			"Queued stale command lacked explicit failure");
	Require(f.runtime.DestroyInstance(f.instance) == XR_SUCCESS, "Destroy parent control graph");
	Require(Error(client.Call(wire::OpCode::Status, client.Common())) == wire::ErrorCode::NotFound &&
				Nested(client.Call(wire::OpCode::Sessions), wire::tag::Sessions).fields.empty(),
			"Parent teardown left discoverable session/control state");
	f.service.Close();
	Require(!f.service.Connect() &&
				Error(f.service.Handle(client.connection, client.Request(wire::OpCode::Sessions))) ==
					wire::ErrorCode::Closed,
			"Closed service accepted late connection or message");
}

void RecordingOwnershipAcrossGenerations()
{
	Fixture fixture;
	Client previous(fixture.service), current(fixture.service);
	previous.Hello();
	current.Hello();
	previous.Discover();
	current.Discover();
	Require(Error(previous.Call(wire::OpCode::StartRecording, previous.Common())) == wire::ErrorCode::Ok,
			"Start generation-scoped recording");
	Require(Error(current.Call(wire::OpCode::StartRecording, current.Common())) == wire::ErrorCode::Busy,
			"Another connection stole current recording");
	fixture.Restart();
	previous.Discover();
	current.Discover();
	Require(Error(current.Call(wire::OpCode::StartRecording, current.Common())) == wire::ErrorCode::Ok,
			"An ended generation retained recording ownership");
	Require(Error(previous.Call(wire::OpCode::StopRecording, previous.Common())) == wire::ErrorCode::Busy,
			"Old recording owner stopped the new owner's recording");
	fixture.service.Disconnect(previous.connection);
	Require(fixture.session->Commands().Recorder().Snapshot().active,
			"Old owner disconnect stopped a new generation's recording");
	Require(Error(current.Call(wire::OpCode::StopRecording, current.Common())) == wire::ErrorCode::Ok,
			"New owner could not stop recording");
}

void BeforeFirstGeneration()
{
	Fixture f(true, false);
	Client client(f.service);
	client.Hello();
	client.Discover();
	Require(client.generation == 0 && Error(client.Call(wire::OpCode::Status, client.Common())) == wire::ErrorCode::Ok,
			"Initial generation must remain observable");
	Require(Error(client.Call(wire::OpCode::Acquire, client.Common())) == wire::ErrorCode::Closed,
			"Remote acquisition used generation-zero internal compatibility behavior");
	f.Begin();
	client.Discover();
	client.Acquire();
}

void DestroyedLeaseCapacityRecovery()
{
	Fixture f;
	Client client(f.service);
	client.Hello();
	std::uint64_t previous = 0;
	for (unsigned iteration = 0; iteration < 66; ++iteration)
	{
		client.Discover();
		Require(client.session > previous, "Destroyed session control identity was reused");
		previous = client.session;
		client.Acquire();
		if (iteration == 65)
			break;
		Require(f.runtime.DestroySession(f.session) == XR_SUCCESS, "Destroy leased session in long-running client");
		f.session = {};
		Require(f.runtime.CreateSession(f.instance, f.session, f.clock) == XR_SUCCESS &&
					f.session->ConfigureCapturePool(f.pool) == XR_SUCCESS,
				"Recreate leased session");
		f.Begin();
		DebugControllerConfig config;
		config.startWorker = false;
		f.controller = f.runtime.Debugger(f.session, config);
	}
	Require(Error(client.Call(wire::OpCode::Renew, client.Leased())) == wire::ErrorCode::Ok,
			"Destroyed session leases exhausted a persistent connection");
}

void DiscoveryFailureRetainsReadersUntilSettlement()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	Fixture fixture(false);
	Client client(fixture.service);
	client.Hello();
	client.Discover();
	const auto previousId = client.session;
	const auto request = client.Request(wire::OpCode::Sessions);
	std::weak_ptr<Session> session = fixture.session.Shared();
	bool removed = false;
	bool readerHeld = false;
	bool clockDestroyed = false;
	bool reentered = false;
	std::weak_ptr<TestClock> observedClock = fixture.clock;
	fixture.clock->onDestroyed = [&]
	{
		clockDestroyed = true;
		const auto response = client.Call(wire::OpCode::Sessions);
		reentered = Error(response) == wire::ErrorCode::Ok && Nested(response, wire::tag::Sessions).fields.empty();
	};
	wire::Message response;
	{
		// The first reservation holds discovered readers. Fail the prepared
		// registration reservation after those readers have been acquired.
		MatchingAllocationFailure failure(sizeof(wire::ControlSession) * wire::ControlSessions::Capacity, 1,
										  [&]
										  {
											  removed = fixture.runtime.DestroySession(fixture.session) == XR_SUCCESS;
											  fixture.controller.reset();
											  fixture.session = {};
											  fixture.clock.reset();
											  readerHeld = !session.expired() && !clockDestroyed;
										  });
		response = fixture.service.Handle(client.connection, request);
	}
	if (auto remainingClock = observedClock.lock())
		remainingClock->onDestroyed = {};
	Require(allocationFailed && Error(response) == wire::ErrorCode::Capacity && removed && readerHeld &&
				clockDestroyed && reentered && session.expired(),
			"Failed discovery released its session dependency before request admission reopened");
	fixture.clock = std::make_shared<TestClock>();
	Require(fixture.runtime.CreateSession(fixture.instance, fixture.session, fixture.clock) == XR_SUCCESS,
			"Create a session after failed discovery");
	fixture.Begin();
	client.Discover();
	Require(client.session > previousId, "Discovery failure reused a retired public session identity");
#endif
}

void DiscoveryFailurePreservesIdentityRegistry()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	Fixture fixture(false);
	wire::ControlSessions directory(fixture.runtime);
	wire::ControlSessions::Snapshot first;
	directory.Discover(first);
	const auto id = first.Ids().front();
	const auto generation = fixture.session->Snapshot().generation;
	wire::ControlSessions::Snapshot partial;
	bool failed = false;
	{
		MatchingAllocationFailure failure(sizeof(wire::ControlSession) * wire::ControlSessions::Capacity, 1);
		try
		{
			directory.Discover(partial);
		}
		catch (const std::bad_alloc&)
		{
			failed = true;
		}
	}
	Require(failed && allocationFailed, "Discovery did not exercise registration allocation rollback");
	bool rejected = false;
	try
	{
		partial.Resolve(id, generation);
	}
	catch (const std::logic_error&)
	{
		rejected = true;
	}
	Require(rejected, "Partially discovered sessions were exposed to request routing");
	wire::ControlSessions::Snapshot recovered;
	directory.Discover(recovered);
	Require(recovered.Ids().size() == 1 && recovered.Ids().front() == id &&
				recovered.Resolve(id, generation).session == fixture.session.Shared(),
			"Discovery allocation rollback corrupted an existing session identity");
#endif
}

void ConcurrentDiscoveryPublishesInCollectionOrder()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	Fixture fixture(false);
	wire::ControlSessions directory(fixture.runtime);
	wire::ControlSessions::Snapshot first;
	directory.Discover(first);
	const auto originalId = first.Ids().front();
	wire::ControlSessions::Snapshot partial;
	wire::ControlSessions::Snapshot concurrent;
	Runtime::SessionRef added;
	std::future<void> discovering;
	std::promise<void> started;
	auto entered = started.get_future();
	bool overtook = false;
	bool failed = false;
	{
		// Stop after collecting the old session set but before publishing it.
		// Another discovery must not register a new session ahead of this one.
		MatchingAllocationFailure failure(
			sizeof(wire::ControlSession) * wire::ControlSessions::Capacity, 1,
			[&]
			{
				Require(fixture.runtime.DestroySession(fixture.session) == XR_SUCCESS &&
							fixture.runtime.CreateSession(fixture.instance, added, fixture.clock) == XR_SUCCESS,
						"Replace a session during directory discovery");
				discovering = std::async(std::launch::async,
										 [&]
										 {
											 started.set_value();
											 directory.Discover(concurrent);
										 });
				entered.get();
				overtook = discovering.wait_for(100ms) == std::future_status::ready;
			});
		try
		{
			directory.Discover(partial);
		}
		catch (const std::bad_alloc&)
		{
			failed = allocationFailed;
		}
	}
	if (discovering.valid())
		discovering.get();
	Require(failed && !overtook && concurrent.Sessions().size() == 1,
			"Concurrent discovery published a newer session set ahead of an unfinished collection");
	wire::ControlSessions::Snapshot recovered;
	directory.Discover(recovered);
	for (const auto& binding : concurrent.Sessions())
	{
		const auto& current = recovered.Resolve(binding.id, binding.session->Snapshot().generation);
		Require(current.session == binding.session, "Concurrent discovery replaced a live session identity");
		Require(binding.session == added.Shared() && binding.id > originalId,
				"Replacement discovery reused the removed session identity");
	}
#endif
}

class Output final : public FrameOutput
{
  public:
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	~Output() override
	{
		if (destroy)
			destroy();
	}
	std::shared_ptr<FrameReservation> reservation;
	std::function<void()> destroy;
};

struct ImageOwnerObservations
{
	std::size_t retains = 0;
	std::size_t destroyed = 0;
	bool rejectDescription = false;
	std::function<void()> onRetained;
	std::function<void()> onDestroyed;
};

class ObservedImageReader final : public wire::ImageReader
{
  public:
	ObservedImageReader(ImageOwnerObservations& observations, std::shared_ptr<const FrameOutput> output)
		: m_observations(observations), m_output(std::move(output))
	{
	}
	~ObservedImageReader() override
	{
		++m_observations.destroyed;
		if (m_observations.onDestroyed)
			m_observations.onDestroyed();
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	wire::ImageDescription Description() const override
	{
		if (std::exchange(m_observations.rejectDescription, false))
			throw std::bad_alloc{};
		wire::ImageDescription description;
		description.width = 17;
		description.height = 9;
		description.arrayLayers = 1;
		return description;
	}
	wire::ImageCapabilities Capabilities() const noexcept override
	{
		return {};
	}
	std::shared_ptr<wire::ImageDelivery> Share(const wire::ImageDeviceIdentity&,
											   const std::shared_ptr<wire::TransferHandle>&) const override
	{
		throw std::logic_error("CPU owner test does not provide GPU delivery");
	}
	std::shared_ptr<wire::ImageExport> Export(std::optional<std::uint32_t>) const override
	{
		throw std::logic_error("CPU owner test does not provide image export");
	}

  private:
	ImageOwnerObservations& m_observations;
	std::shared_ptr<const FrameOutput> m_output;
};

class ObservedImageProvider final : public wire::ImageInspectionProvider
{
  public:
	explicit ObservedImageProvider(ImageOwnerObservations& observations) : m_observations(observations) {}
	wire::ImageCapabilities Capabilities(XrSession) const noexcept override
	{
		return {};
	}
	std::shared_ptr<wire::ImageReader> Retain(std::shared_ptr<const FrameOutput> output) const override
	{
		++m_observations.retains;
		auto reader = std::make_shared<ObservedImageReader>(m_observations, std::move(output));
		if (m_observations.onRetained)
			m_observations.onRetained();
		return reader;
	}

  private:
	ImageOwnerObservations& m_observations;
};

void ImageWindowPublicationAndRelease()
{
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	bool exercisedPublicationRollback = false;
	for (std::size_t skip = 0; skip < 24 && !exercisedPublicationRollback; ++skip)
	{
		Fixture fixture(false, false);
		Require(fixture.session->ConfigureCapturePool(FramePool{8}, false,
			[](const auto&, const auto&, const auto&) { return SnapshotAdmission::Accepted; }) == XR_SUCCESS &&
			fixture.session->EnableImageHistory() == XR_SUCCESS, "Configure window history");
		fixture.Begin();
		ImageOwnerObservations observations;
		wire::ControlService service(fixture.runtime, 2, std::make_shared<ObservedImageProvider>(observations));
		Client client(service);
		client.Hello();
		client.Discover();
		auto metadata = std::make_shared<FrameRecord>();
		metadata->key = {{client.generation}, {1}};
		metadata->submission.emplace();
		metadata->submission->layerCount = 1;
		auto output = std::make_shared<Output>();
		fixture.session->ImageHistory().Append({metadata, output});
		const auto request = client.Request(wire::OpCode::BeginImageWindow, client.Common());
		wire::Message response;
		bool rejected = false;
		{
			MatchingAllocationFailure failure(sizeof(std::uint64_t), skip);
			try
			{
				response = service.Handle(client.connection, request);
			}
			catch (const std::bad_alloc&)
			{
				rejected = true;
			}
		}
		if (!rejected && Error(response) == wire::ErrorCode::Ok)
		{
			client.Call(wire::OpCode::ReleaseImageWindow,
				{wire::Field::U64(wire::tag::ImageWindowId, U64(response, wire::tag::ImageWindowId))});
			continue;
		}
		Require(rejected || Error(response) == wire::ErrorCode::Capacity, "Window allocation failure was not reported");
		const auto replacement = client.Call(wire::OpCode::BeginImageWindow, client.Common());
		Require(Error(replacement) == wire::ErrorCode::Ok, "Unpublished window leaked collection admission");
		const auto id = U64(replacement, wire::tag::ImageWindowId);
		exercisedPublicationRollback = id > 1;
		fixture.session->ImageHistory().Clear();
		bool reentered = false;
		output->destroy = [&]
		{
			reentered = Error(client.Call(wire::OpCode::Status, client.Common())) == wire::ErrorCode::Ok;
		};
		output.reset();
		Require(Error(client.Call(wire::OpCode::ReleaseImageWindow,
			{wire::Field::U64(wire::tag::ImageWindowId, id)})) == wire::ErrorCode::Ok && reentered,
			"Window release leaked its native owner or destroyed it inside the request gate");
	}
	Require(exercisedPublicationRollback, "No allocation failure exercised stored-window publication rollback");
#endif
}

void ImageOwnerPublicationAndDeferredRelease()
{
	ImageOwnerObservations observations;
	ObservedImageProvider provider(observations);
	wire::ConnectionImages images;
	const wire::ImageSource source{{0xfedcba9876543210ULL}, {{7}, {11}}, {23}, {CaptureKind::LayerView, 3, 2}};
	const auto output = std::make_shared<Output>();
	wire::Message response;
	observations.rejectDescription = true;
	bool rejected = false;
	try
	{
		wire::ConnectionImages::RetiredResource unpublished;
		wire::ConnectionImages::Publication publication(images, unpublished);
		images.Retain(response, source, output, provider, false, publication);
	}
	catch (const std::bad_alloc&)
	{
		rejected = true;
	}
	const auto rolledBack = U64(response, wire::tag::ImageReaderId);
	Require(rejected && observations.retains == 1 && observations.destroyed == 1,
			"Unannounced image publication retained a native-bearing reader");

	auto query = [&](wire::OpCode operation, std::uint64_t id, wire::ConnectionImages::RetiredResource& retired)
	{
		wire::ControlPacket request, outgoing;
		request.message.opcode = static_cast<std::uint16_t>(operation);
		request.message.requestId = {1};
		request.message.fields.push_back(wire::Field::U64(wire::tag::ImageReaderId, id));
		wire::ConnectionImages::RetiredResource unpublished;
		wire::ConnectionImages::Publication publication(images, unpublished);
		return images.Dispatch(request, outgoing, wire::ProtocolMinor, false, publication, retired);
	};
	auto absent = [&](std::uint64_t id)
	{
		wire::ConnectionImages::RetiredResource retired;
		try
		{
			query(wire::OpCode::ImageInfo, id, retired);
		}
		catch (const wire::ConnectionImages::RequestFailure& failure)
		{
			return failure.error == wire::ErrorCode::NotFound;
		}
		return false;
	};
	Require(absent(rolledBack), "Rolled-back resource remained discoverable");

	response = {};
	{
		wire::ConnectionImages::RetiredResource unpublished;
		wire::ConnectionImages::Publication publication(images, unpublished);
		images.Retain(response, source, output, provider, false, publication);
		publication.Commit();
	}
	const auto reader = U64(response, wire::tag::ImageReaderId);
	Require(reader > rolledBack && U64(response, wire::tag::Session) == source.session.value &&
				U64(Nested(response, wire::tag::Frame), wire::tag::FrameId) == 11 &&
				U64(response, wire::tag::CaptureId) == 23,
			"Publication changed original source identity or reused a retired resource ID");
	const auto target = Nested(response, wire::tag::Target);
	Require(target.Find(wire::tag::Layer)->AsU32() == 3 && target.Find(wire::tag::View)->AsU32() == 2,
			"Image owner substituted a selected layer/view identity");

	// An error describing an already-published source must leave its existing
	// reader intact; rollback owns only entries created by this transaction.
	observations.rejectDescription = true;
	try
	{
		wire::ConnectionImages::RetiredResource unpublished;
		wire::ConnectionImages::Publication publication(images, unpublished);
		wire::Message duplicate;
		images.Retain(duplicate, source, output, provider, false, publication);
		Require(false, "Duplicate metadata failure was not exercised");
	}
	catch (const std::bad_alloc&)
	{
	}
	Require(observations.retains == 2 && observations.destroyed == 1 && !absent(reader),
			"Failed duplicate publication retired an existing reader or repeated provider retention");

	bool executing = true;
	bool reentered = false;
	observations.onDestroyed = [&]
	{
		reentered = !executing && absent(reader);
	};
	{
		wire::ConnectionImages::RetiredResource retired;
		const auto release = query(wire::OpCode::ReleaseImage, reader, retired);
		Require(release.Find(wire::tag::Released)->AsBool() == true && observations.destroyed == 1,
				"Release destroyed its last native reader inside request execution");
		executing = false;
	}
	Require(observations.destroyed == 2 && reentered,
			"Deferred reader retirement could not reenter the image owner after gate reopening");
	observations.onDestroyed = {};
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	response = {};
	{
		wire::ConnectionImages::RetiredResource unpublished;
		wire::ConnectionImages::Publication publication(images, unpublished);
		images.Retain(response, source, output, provider, false, publication);
		publication.Commit();
	}
	const auto failingReader = U64(response, wire::tag::ImageReaderId);
	wire::ControlPacket releaseRequest, outgoing;
	releaseRequest.message.opcode = static_cast<std::uint16_t>(wire::OpCode::ReleaseImage);
	releaseRequest.message.requestId = {2};
	releaseRequest.message.fields.push_back(wire::Field::U64(wire::tag::ImageReaderId, failingReader));
	executing = true;
	reentered = false;
	observations.onDestroyed = [&]
	{
		reentered = !executing && absent(failingReader);
	};
	{
		wire::ConnectionImages::RetiredResource retired;
		wire::ConnectionImages::RetiredResource unpublished;
		wire::ConnectionImages::Publication publication(images, unpublished);
		bool allocationRejected = false;
		{
			// Response construction allocates an eight-byte identifier only after
			// removing the resource. Request fields were prepared above.
			MatchingAllocationFailure failure(sizeof(std::uint64_t), 0);
			try
			{
				images.Dispatch(releaseRequest, outgoing, wire::ProtocolMinor, false, publication, retired);
			}
			catch (const std::bad_alloc&)
			{
				allocationRejected = allocationFailed;
			}
		}
		Require(allocationRejected && observations.destroyed == 2 && absent(failingReader),
				"Release response allocation failure destroyed its detached reader inside the request gate");
		executing = false;
	}
	Require(observations.destroyed == 3 && reentered,
			"Failed release response stranded its reader or prevented deferred reentry");
	observations.onDestroyed = {};
#endif
}

class ReplayExposure final : public wire::PacketExposure
{
  public:
	~ReplayExposure() override
	{
		if (destroyed)
			destroyed();
	}
	void MarkTransferred() noexcept override
	{
		++transfers;
	}
	unsigned transfers = 0;
	std::function<void()> destroyed;
};

class ReplayHandle final : public wire::TransferHandle
{
  public:
	wire::TransferHandleKind Kind() const noexcept override
	{
		return wire::TransferHandleKind::OpaqueFd;
	}
	std::intptr_t Value() const noexcept override
	{
		return 17; // CPU-only owner identity; never passed to an OS transport.
	}
	std::shared_ptr<wire::TransferHandle> Duplicate() const override
	{
		return std::make_shared<ReplayHandle>();
	}
};

wire::ControlPacket ExchangePacket(std::uint64_t identity, bool response = false)
{
	wire::ControlPacket packet;
	packet.message.requestId = {identity};
	packet.message.opcode = static_cast<std::uint16_t>(wire::OpCode::Sessions);
	if (response)
	{
		packet.message.flags = 1;
		packet.message.fields.push_back(wire::Field::U32(wire::tag::Error, 0, true));
	}
	return packet;
}

void RequestReplayOwnership()
{
	using Admission = wire::ConnectionRequests::Exchange::Admission;
	unsigned closed = 0;
	wire::ConnectionRequests requests(
		[&]
		{
			++closed;
		});
	const auto first = ExchangePacket(0xfedcba9876543210ULL);
	auto response = ExchangePacket(first.message.requestId.value, true);
	auto exposure = std::make_shared<ReplayExposure>();
	auto handle = std::make_shared<ReplayHandle>();
	response.exposure = exposure;
	response.handles.push_back(handle);
	response.encoded = std::make_shared<const std::vector<std::byte>>(wire::Encode(response.message));
	Require(!requests.Negotiate(1), "Negotiation occurred without an admitted request");
	{
		wire::ConnectionRequests::RetiredReplay retired;
		auto exchange = requests.Begin(first);
		Require(exchange.Result() == Admission::Execute && requests.Negotiate(1) && !requests.Negotiate(2),
				"Request negotiation did not have a single owner");
		auto busy = requests.Begin(ExchangePacket(first.message.requestId.value + 1));
		Require(busy.Result() == Admission::Reject && busy.Error() == wire::ErrorCode::Busy &&
					busy.ResponseMinor() == 1,
				"Concurrent admission lost Busy or the negotiated response minor");
		exchange.Publish(response, retired);
	}
	{
		auto replay = requests.Begin(first);
		Require(replay.Result() == Admission::Replay && replay.Replay().handles.front() == handle &&
					replay.Replay().exposure == exposure && replay.Replay().encoded == response.encoded &&
					exposure->transfers == 0,
				"Replay changed exact native owners, rebuilt framing or fabricated handle exposure");
	}
	{
		auto retry = first;
		retry.handles.push_back(handle);
		auto rejected = requests.Begin(retry);
		Require(rejected.Error() == wire::ErrorCode::Conflict, "Replay accepted another consumer handle");
	}
	{
		auto changed = first;
		changed.message.fields.push_back(wire::Field::U32(60000, 1));
		auto rejected = requests.Begin(changed);
		Require(rejected.Error() == wire::ErrorCode::Conflict, "Changed envelope reused an accepted identity");
	}
	bool retiredAfterGate = false;
	exposure->destroyed = [&]
	{
		auto admitted = requests.Begin(ExchangePacket(first.message.requestId.value + 2));
		retiredAfterGate = admitted.Result() == Admission::Execute;
	};
	exposure.reset();
	response = {};
	{
		wire::ConnectionRequests::RetiredReplay retired;
		{
			auto exchange = requests.Begin(ExchangePacket(first.message.requestId.value + 1));
			exchange.Publish(ExchangePacket(first.message.requestId.value + 1, true), retired);
			Require(!retiredAfterGate, "Replay replacement destroyed a native exposure inside the request gate");
		}
		Require(!retiredAfterGate, "Replay retirement escaped its explicitly retained owner");
	}
	Require(retiredAfterGate && closed == 0, "Retired replay could not reenter admission after gate reopening");
	auto stale = requests.Begin(first);
	Require(stale.Result() == Admission::Reject && stale.Error() == wire::ErrorCode::Conflict,
			"Retired request became executable after the replay cache advanced");
}

void RequestCloseSettlementAndRollback()
{
	using Admission = wire::ConnectionRequests::Exchange::Admission;
	std::atomic<unsigned> closed{0};
	wire::ConnectionRequests* observed = nullptr;
	bool closedReentry = false;
	wire::ConnectionRequests requests(
		[&]
		{
			++closed;
			auto rejected = observed->Begin(ExchangePacket(99));
			closedReentry = rejected.Result() == Admission::Reject && rejected.Error() == wire::ErrorCode::Closed;
		});
	observed = &requests;
	const auto first = ExchangePacket(1);
	{
		wire::ConnectionRequests::RetiredReplay retired;
		auto exchange = requests.Begin(first);
		exchange.Publish(ExchangePacket(1, true), retired);
	}
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	{
		wire::ConnectionRequests::RetiredReplay retired;
		auto exchange = requests.Begin(ExchangePacket(2));
		auto response = ExchangePacket(2, true);
		response.message.fields.push_back(wire::Field::Text(wire::tag::Detail, std::string(319, 'x')));
		bool rejected = false;
		{
			MatchingAllocationFailure failure(319, 0);
			try
			{
				exchange.Publish(response, retired);
			}
			catch (const std::bad_alloc&)
			{
				rejected = allocationFailed;
			}
		}
		Require(rejected, "Replay-cache allocation failure was not exercised");
	}
	{
		auto replay = requests.Begin(first);
		Require(replay.Result() == Admission::Replay && replay.Replay().message.requestId.value == 1,
				"Failed replay-copy publication changed the previously accepted request");
	}
#endif
	{
		wire::ConnectionRequests::RetiredReplay retired;
		auto exchange = requests.Begin(ExchangePacket(2));
		Require(exchange.Result() == Admission::Execute, "Failed publication stranded active request admission");
		std::thread disconnect(
			[&]
			{
				requests.Close();
				requests.Close();
			});
		disconnect.join();
		Require(closed == 0, "Disconnect retired connection state while its request was still executing");
		auto rejected = requests.Begin(ExchangePacket(3));
		Require(rejected.Error() == wire::ErrorCode::Closed, "Disconnect admitted a new request before settlement");
		exchange.Publish(ExchangePacket(2, true), retired);
	}
	requests.Close();
	Require(closed == 1 && closedReentry, "Close notification was repeated or held the request mutex");

	// The observer itself may own native readers. Destroy it while the closed
	// admission state and mutex are still valid for last-reader reentry.
	bool observerRetiredSafely = false;
	auto output = std::make_shared<Output>();
	wire::ConnectionRequests* destroying = nullptr;
	output->destroy = [&]
	{
		auto rejected = destroying->Begin(ExchangePacket(1));
		observerRetiredSafely = rejected.Error() == wire::ErrorCode::Closed;
	};
	auto owner = std::make_unique<wire::ConnectionRequests>(
		[output]
		{
		});
	destroying = owner.get();
	output.reset();
	owner.reset();
	Require(observerRetiredSafely, "Close observer released its last reader after request-owner members died");
}

void ImagePublicationRollbackReentry()
{
	Fixture fixture;
	ImageOwnerObservations observations;
	auto provider = std::make_shared<ObservedImageProvider>(observations);
	wire::ControlService service(fixture.runtime, 2, provider);
	Client client(service);
	client.Hello();
	client.Discover();
	const auto prediction = fixture.Issue();
	const auto output = std::make_shared<Output>();
	const auto layers = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
															std::vector<LayerDescription>{QuadLayerDescription{}});
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, prediction.displayTime.value.count(),
					   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  nullptr};
	FramePrediction accepted;
	Require(fixture.session->EndFrame(accepted, &end, layers, output) == XR_SUCCESS,
			"Submit image for provider rollback reentry");
	Require(fixture.session->Display().Advance(prediction.displayTime).has_value(),
			"Select actual submitted CPU test output");
	bool reentered = false;
	observations.onDestroyed = [&]
	{
		reentered = Error(client.Call(wire::OpCode::Sessions)) == wire::ErrorCode::Ok;
	};
	observations.rejectDescription = true;
	const auto rejected = client.Call(wire::OpCode::RetainPreview, client.Common());
	Require(Error(rejected) == wire::ErrorCode::Capacity && observations.destroyed == 1 && reentered,
			"Publication rollback destroyed its last reader while the connection request gate was closed");
#ifndef SIMXR_TEST_TSAN_ALLOCATOR
	reentered = false;
	allocationFailed = false;
	observations.onRetained = []
	{
		// The provider has returned its reader; the table entry is the next allocation.
		failNextAllocation = true;
	};
	const auto entryRejected = client.Call(wire::OpCode::RetainPreview, client.Common());
	observations.onRetained = {};
	Require(allocationFailed && Error(entryRejected) == wire::ErrorCode::Capacity && observations.destroyed == 2 &&
				reentered,
			"Entry allocation failure destroyed its new reader before request admission reopened");
#endif
	observations.onDestroyed = {};
}

void AcknowledgeLastReaderReentry()
{
	Fixture f;
	Client client(f.service);
	client.Hello();
	client.Discover();
	client.Acquire();
	client.Submit(1, wire::CommandKind::Pause);
	f.controller->Poll();
	const auto step = U64(client.Submit(2, wire::CommandKind::Step), wire::tag::OperationId);
	f.controller->Poll();
	const auto prediction = f.Issue();
	auto output = std::make_shared<Output>();
	output->reservation = f.session->Captures().ProducerReservation({f.session->Snapshot().generation, prediction.id});
	Require(f.pool.Consume(output->reservation), "Consume fake GPU output admission");
	auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														std::vector<LayerDescription>{QuadLayerDescription{}});
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, prediction.displayTime.value.count(),
					   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  nullptr};
	FramePrediction accepted;
	Require(f.session->EndFrame(accepted, &end, metadata, output) == XR_SUCCESS, "Submit reentrant-reader frame");
	f.controller->Poll();
	const auto capture = U64(client.Operation(step), wire::tag::CaptureId);
	const auto firstDisplay = U64(client.Submit(3, wire::CommandKind::DisplayStep), wire::tag::OperationId);
	f.controller->Poll();
	const auto firstDisplayCapture = U64(client.Operation(firstDisplay), wire::tag::CaptureId);
	const auto display = U64(client.Submit(4, wire::CommandKind::DisplayStep), wire::tag::OperationId);
	f.controller->Poll();
	Require(Text(client.Operation(display), wire::tag::Progress) == "displayed", "Display fake output");
	const auto displayCapture = U64(client.Operation(display), wire::tag::CaptureId);
	client.Submit(5, wire::CommandKind::Release, {wire::Field::U64(wire::tag::CaptureId, capture)});
	f.controller->Poll();
	client.Submit(6, wire::CommandKind::Step);
	f.controller->Poll();
	f.Clear(f.Issue());
	client.Submit(7, wire::CommandKind::DisplayStep);
	f.controller->Poll();
	client.Submit(8, wire::CommandKind::Release, {wire::Field::U64(wire::tag::CaptureId, displayCapture)});
	f.controller->Poll();
	client.Submit(9, wire::CommandKind::Release, {wire::Field::U64(wire::tag::CaptureId, firstDisplayCapture)});
	f.controller->Poll();
	struct ReentryState
	{
		std::atomic<bool> destroyed{false}, reentered{false};
	};
	const auto state = std::make_shared<ReentryState>();
	const auto common = client.Common();
	const auto connection = client.connection;
	output->destroy = [&f, state, common, connection]
	{
		wire::Message status;
		status.opcode = static_cast<std::uint16_t>(wire::OpCode::Status);
		status.requestId = wire::RequestId{1000000};
		status.fields = common;
		const auto error = Error(f.service.Handle(connection, status));
		state->reentered.store(error == wire::ErrorCode::Ok || error == wire::ErrorCode::Busy);
		state->destroyed.store(true);
	};
	output.reset();
	Require(!state->destroyed.load(), "Operation ledger did not retain its actual display output reader");
	auto ack = client.Leased();
	ack.push_back(wire::Field::U64(wire::tag::CommandId, 9));
	Require(Error(client.Call(wire::OpCode::Acknowledge, ack)) == wire::ErrorCode::Ok && state->destroyed.load() &&
				state->reentered.load(),
			"Acknowledgement destroyed last output under connection lock or leaked reader");
}
} // namespace

int main()
{
	try
	{
		LiveHaptics();
		ContinuousFrameHistory();
		NegotiationAndReplay();
		NegotiationPublicationRollback();
		RecordingPagesShrinkBeforeEncoding();
		LegacyMinorNegotiation();
		OwnershipAndExactClear();
		LeaseOwnerRetirementAndRollback();
		HistoricalInspectionAfterSessionDestroy();
		InspectionResponseAllocationRollback();
		CapabilityAndValidation();
		GenerationAndTeardown();
		RecordingOwnershipAcrossGenerations();
		BeforeFirstGeneration();
		DestroyedLeaseCapacityRecovery();
		DiscoveryFailureRetainsReadersUntilSettlement();
		DiscoveryFailurePreservesIdentityRegistry();
		ConcurrentDiscoveryPublishesInCollectionOrder();
		AcknowledgeLastReaderReentry();
		ImageOwnerPublicationAndDeferredRelease();
		ImageWindowPublicationAndRelease();
		ImagePublicationRollbackReentry();
		RequestReplayOwnership();
		RequestCloseSettlementAndRollback();
		std::cout << "Control service negotiation, leases, exact snapshots, retry and teardown passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
