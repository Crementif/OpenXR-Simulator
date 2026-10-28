#include "../TestSupport.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/InspectionDocument.h"
#include "runtime/Runtime.h"

#include <cstring>
#include <iostream>
#include <limits>

using namespace simxr;
namespace wire = control;
static void CheckObservationJournal()
{
	GameBridge bridge(4);
	const auto game = bridge.Register(SIMXR_GAME_CAN_RESET);
	Require(game && bridge.Observations().observations.front().kind == GameObservationKind::Registered,
			"Journal omitted game registration");
	for (unsigned index = 0; index < 8; ++index)
		Require(bridge.Publish(game, std::to_string(index)), "Journal publication failed");
	const auto stale = bridge.Observations(0, 2);
	Require(stale.gap && !stale.failed && stale.floor == 5 && stale.offset == 5 && stale.next == 7 &&
				stale.total == 9 && stale.observations.size() == 2,
			"Evicted SDK evidence was not reported as a bounded gap");
	Require(stale.observations[0].payload == "4" && stale.observations[0].revision == 5 &&
				stale.observations[1].payload == "5",
			"Journal coalesced successive telemetry values");
	const auto tail = bridge.Observations(stale.next);
	Require(!tail.gap && tail.next == 9 && tail.observations.size() == 2,
			"Journal cursor duplicated or skipped retained observations");
	for (const auto& event : tail.observations)
		Require(event.time && event.time->nanoseconds > 0 && event.time->resolutionNanoseconds > 0 &&
					event.game == game && event.kind == GameObservationKind::Telemetry,
				"SDK observation lacks native clock identity");
	Require(tail.observations[0].time->domain == tail.observations[1].time->domain &&
				tail.observations[0].time->nanoseconds <= tail.observations[1].time->nanoseconds,
			"Serialized observation timestamps went backward");
	const auto cursor = bridge.Observations({}, 0).total;
	Require(!bridge.Publish(game + 1, "wrong game") && bridge.Observations({}, 0).total == cursor,
			"Rejected SDK publication changed journal identity");
	const auto operation = bridge.Submit(game, 1, 1, SIMXR_GAME_RESET, "checkpoint");
	SimxrGameCommand command{};
	Require(operation && bridge.Poll(game, command) && bridge.Complete(game, command.id, false, "reset failed"),
			"SDK failure setup");
	bridge.Unregister(game);
	const auto events = bridge.Observations(cursor);
	Require(events.observations.size() == 1 && events.observations[0].kind == GameObservationKind::Unregistered,
			"Journal omitted disconnect or exposed private command results");
	const auto next = bridge.Register(SIMXR_GAME_CAN_STEP);
	Require(next != game, "Game identity reused");
	bridge.Close();
	const auto closed = bridge.Observations(events.next);
	Require(closed.observations.size() == 2 && closed.observations[0].game == next &&
				closed.observations[1].kind == GameObservationKind::Closed && stale.observations[0].payload == "4",
			"Registration reset journal cursors or mutated a retained page");
	const auto total = bridge.Observations({}, 0).total;
	bridge.Close();
	bridge.Unregister(0);
	Require(bridge.Observations({}, 0).total == total, "Repeated close invented lifecycle events");
	for (const auto offset : {total + 1, std::numeric_limits<std::uint64_t>::max()})
	{
		bool rejected = false;
		try
		{
			(void)bridge.Observations(offset);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Future journal cursor accepted");
	}
}
int main()
{
	try
	{
		CheckObservationJournal();
		Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "Optional game bridge");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Runtime::InstanceRef instance;
		Runtime::SessionRef session;
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS,
				"Create");
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS, "Begin");
		SimxrGameBridgeApi api{};
		api.size = sizeof(api);
		Require(!runtime.Game().FillApi(99, api) && runtime.Game().FillApi(1, api), "C API version negotiation");
		const auto game = api.registerGame(api.context, SIMXR_GAME_CAN_RESET | SIMXR_GAME_CAN_COMMAND);
		Require(game && !api.registerGame(api.context, 1), "Exclusive game registration");
		wire::ControlService service(runtime);
		auto connection = *service.Connect();
		std::uint64_t request = 0;
		const auto call = [&](wire::OpCode opcode, std::vector<wire::Field> fields)
		{
			wire::Message message;
			message.opcode = static_cast<std::uint16_t>(opcode);
			message.requestId = wire::RequestId{++request};
			message.fields = std::move(fields);
			return service.Handle(connection, message);
		};
		const auto hello = call(wire::OpCode::Hello, {});
		Require((*hello.Find(wire::tag::Capabilities)->AsU64() &
				 static_cast<std::uint64_t>(wire::Capability::GameBridge)) != 0,
				"Registered capabilities not detected");
		const auto common = [&]
		{
			return std::vector<wire::Field>{
				wire::Field::U64(wire::tag::Session, 1),
				wire::Field::U64(wire::tag::Generation, session->Snapshot().generation.value)};
		};
		const auto submit = [&](std::uint64_t id, std::uint32_t kind, const char* payload)
		{
			auto fields = common();
			fields.push_back(wire::Field::U64(wire::tag::GameId, game));
			fields.push_back(wire::Field::U64(wire::tag::CommandId, id));
			fields.push_back(wire::Field::U32(wire::tag::GameKind, kind));
			fields.push_back(wire::Field::Text(wire::tag::Detail, payload));
			return call(wire::OpCode::SubmitGame, std::move(fields));
		};
		const auto independent = call(wire::OpCode::GameStatus, {});
		Require(independent.Find(wire::tag::GameId)->AsU64() == game,
				"Game bridge unexpectedly required an XR session");
		const auto response = submit(1, SIMXR_GAME_RESET, "checkpoint:start");
		Require(response.Find(wire::tag::Phase)->AsText() == "queued",
				"Game submission pretended to execute game code");
		const auto operation = *response.Find(wire::tag::OperationId)->AsU64();
		Require(!api.complete(api.context, game, operation, 1, nullptr, 0), "Game completed an undelivered command");
		SimxrGameCommand command{};
		Require(api.poll(api.context, game, &command) && command.id == operation && command.kind == SIMXR_GAME_RESET &&
					std::string_view{command.payload, command.payloadLength} == "checkpoint:start",
				"Safe update poll lost command");
		Require(!api.poll(api.context, game, &command), "Game delivered same command twice");
		Require(api.complete(api.context, game, operation, 1, "reset applied", 13), "Actual game acknowledgement lost");
		Require(submit(1, SIMXR_GAME_RESET, "checkpoint:start").Find(wire::tag::OperationId)->AsU64() == operation,
				"Idempotent game retry changed identity");
		Require(submit(1, SIMXR_GAME_RESET, "changed").Find(wire::tag::Error)->AsU32() ==
					static_cast<std::uint32_t>(wire::ErrorCode::Conflict),
				"Changed game retry accepted");
		Require(submit(2, SIMXR_GAME_STEP, "").Find(wire::tag::Error)->AsU32() != 0,
				"Unsupported game step advertised");
		Require(api.publish(api.context, game, "level:ready", 11), "Game telemetry publication");
		Require(!api.publish(api.context, game, "\xc0\x80", 2), "Invalid UTF-8 accepted");
		const auto observed = call(wire::OpCode::ReadGameObservations,
								   {wire::Field::U64(wire::tag::Offset, 0), wire::Field::U64(wire::tag::Length, 16)});
		Require(observed.Find(wire::tag::Error)->AsU32() == 0 && observed.Find(wire::tag::Document) &&
					wire::DecodeFields(observed.Find(wire::tag::Document)->value).error == wire::DecodeError::None,
				"Production protocol failed to serialize SDK observations");
		Require(call(wire::OpCode::ReadGameObservations, {wire::Field::U64(wire::tag::Length, 17)})
						.Find(wire::tag::Error)
						->AsU32() != 0,
				"Unbounded SDK page accepted");
		const auto legacy = *service.Connect();
		wire::Message oldRequest;
		oldRequest.minor = 14;
		oldRequest.opcode = static_cast<std::uint16_t>(wire::OpCode::Hello);
		oldRequest.requestId = wire::RequestId{1};
		const auto oldHello = service.Handle(legacy, oldRequest);
		Require(!(*oldHello.Find(wire::tag::Capabilities)->AsU64() &
				  static_cast<std::uint64_t>(wire::Capability::GameObservations)),
				"Old protocol advertised SDK journal");
		oldRequest.opcode = static_cast<std::uint16_t>(wire::OpCode::ReadGameObservations);
		oldRequest.requestId = wire::RequestId{2};
		Require(service.Handle(legacy, oldRequest).Find(wire::tag::Error)->AsU32() != 0,
				"SDK journal bypassed protocol negotiation");
		service.Disconnect(legacy);

		const auto pending = runtime.Game().Submit(game, 99, 1, SIMXR_GAME_COMMAND, "pending");
		Require(pending.has_value(), "Pending game request");
		runtime.Game().Disconnect(99);
		Require(runtime.Game().Read(game, 99, pending->id)->phase == GameCommandPhase::Cancelled,
				"Unissued game request not cancelled");
		const auto delivered = runtime.Game().Submit(game, 100, 1, SIMXR_GAME_COMMAND, "in flight");
		Require(delivered && api.poll(api.context, game, &command), "Delivered game request");
		runtime.Game().Disconnect(100);
		Require(runtime.Game().Read(game, 100, delivered->id)->phase == GameCommandPhase::Delivered,
				"Disconnect invented game completion");
		Require(api.complete(api.context, game, delivered->id, 0, "game rejected", 13),
				"Actual game failure after client disconnect");
		for (std::uint64_t id = 2; id < 140; ++id)
		{
			const auto submitted = runtime.Game().Submit(game, 100, id, SIMXR_GAME_COMMAND, "retire");
			Require(submitted && api.poll(api.context, game, &command) &&
						api.complete(api.context, game, command.id, 1, nullptr, 0),
					"Bounded ledger did not recycle terminal history");
		}
		Require(!runtime.Game().Submit(game, 100, 1, SIMXR_GAME_COMMAND, "in flight"),
				"Retired command identity executed again");
		api.unregisterGame(api.context, game);
		Require(!runtime.Game().Snapshot().capabilities && !api.poll(api.context, game, &command),
				"Unregistered game remains available");
		Require(api.shutdown(api.context) && !api.registerGame(api.context, 1),
				"Explicit bridge shutdown allowed new work");
		std::cout << "PASS optional game C ABI, actual safe-point acknowledgement, protocol and bounded retention\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
