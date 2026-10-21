#include "../TestSupport.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "runtime/Runtime.h"

#include <cstring>
#include <iostream>

using namespace simxr;
namespace wire = control;
int main()
{
	try
	{
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
