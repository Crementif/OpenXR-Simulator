#include "../TestSupport.h"
#include "app/SimulationEdits.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "simulation/CommandScheduler.h"

#include <iostream>
#include <limits>

namespace
{
using namespace simxr;
namespace wire = simxr::control;
namespace app = simxr::app;

wire::Message Observation(const CommandScheduler& scheduler)
{
	wire::Message result;
	result.fields.push_back(
		wire::Field::Bytes(wire::tag::Document, wire::SimulationState(scheduler.Snapshot()).Encode()));
	// Manual display ticks need not match admitted application frame identities.
	result.fields.push_back(wire::Field::Bytes(
		wire::tag::Pipeline, wire::EncodeFields(std::array{wire::Field::U64(wire::tag::NextAdmission, 900)})));
	return result;
}
std::shared_ptr<const AppliedCommandSnapshot> Apply(CommandScheduler& scheduler, std::uint64_t frame)
{
	return scheduler.Apply(FrameId{frame}, SimulationTime{Duration{100}}, RuntimeTime{Duration{200}}, {});
}
template <typename Action> void Rejected(Action action)
{
	bool rejected = false;
	try
	{
		action();
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected, "Invalid or unavailable simulation state scheduled a guessed frame");
}
void ObservedFrameScheduling()
{
	CommandScheduler scheduler;
	scheduler.Start();
	Apply(scheduler, 7);
	PoseSample pose;
	pose.pose.orientation.w = 1;
	pose.pose.position.x = 3;
	app::ManualTransaction edit;
	edit.commands = {TakeControl{ControlChannel::Head}, SetHeadPose{pose}};
	auto packet = app::EncodeSimulationEdit(edit, 21, 4, 1, Observation(scheduler));
	auto transaction = wire::DecodeSimulationTransaction(packet.message, 55);
	Require(transaction.target.frame == FrameId{8}, "Editor used a display tick as the command frame selector");
	Require(transaction.source == CommandSource{CommandSourceKind::Manual, 55} && transaction.id == 1,
			"Manual source or transaction identity changed during encoding");
	Require(scheduler.Submit(std::move(transaction)).result == CommandResult::Queued, "Encoded edit was not admitted");
	const auto publication = Apply(scheduler, 8);
	Require(publication->configuration.tracking.head.pose.position.x == 3 && publication->outcomes.size() == 1 &&
				publication->outcomes.front().result == CommandResult::Applied,
			"Edit did not apply at the next command publication");

	edit.frame = FrameId{12};
	edit.latePolicy = CommandLatePolicy::Reject;
	packet = app::EncodeSimulationEdit(edit, 21, 4, 2, {});
	transaction = wire::DecodeSimulationTransaction(packet.message, 55);
	Require(transaction.target.frame == FrameId{12} && transaction.latePolicy == CommandLatePolicy::Reject,
			"Explicit edit target and late policy were overwritten by observation");
}
void FirstPublicationAndStateFailures()
{
	CommandScheduler scheduler;
	scheduler.Start();
	app::ManualTransaction edit;
	edit.commands = {TakeControl{ControlChannel::Head}};
	auto packet = app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler));
	Require(wire::DecodeSimulationTransaction(packet.message, 1).target.frame == FrameId{1},
			"A fresh scheduler must accept the first command publication");
	Rejected(
		[&]
		{
			app::EncodeSimulationEdit(edit, 1, 1, 1, {});
		});
	constexpr auto largeFrame = (std::uint64_t{1} << 53) + 7;
	Apply(scheduler, largeFrame);
	packet = app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler));
	Require(wire::DecodeSimulationTransaction(packet.message, 1).target.frame == FrameId{largeFrame + 1},
			"Automatic target lost integer precision beyond the floating-point exact range");
	Apply(scheduler, std::numeric_limits<std::uint64_t>::max());
	Rejected(
		[&]
		{
			app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler));
		});
	scheduler.Close();
	Rejected(
		[&]
		{
			app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler));
		});
	wire::Message malformed;
	malformed.fields.push_back(wire::Field::Bytes(wire::tag::Document, std::array{std::byte{0xff}}));
	Rejected(
		[&]
		{
			app::EncodeSimulationEdit(edit, 1, 1, 1, malformed);
		});
	wire::Message wrongType;
	wrongType.fields.push_back(wire::Field::Bytes(
		wire::tag::Document, wire::InspectionValue::Object({{"closed", wire::InspectionValue::Boolean(false)},
															{"frame_id", wire::InspectionValue::Signed(5)}})
								 .Encode()));
	Rejected(
		[&]
		{
			app::EncodeSimulationEdit(edit, 1, 1, 1, wrongType);
		});

	packet = app::EncodeSimulationEdit(app::ReleaseManualInputs{}, 21, 4, 0, {});
	Require(packet.message.opcode == static_cast<std::uint16_t>(wire::OpCode::ReleaseControl) &&
				!packet.message.Find(wire::tag::ApplyFrame) && !packet.message.Find(wire::tag::TransactionId),
			"Immediate source retirement incorrectly depends on observed frames");
	Rejected(
		[&]
		{
			app::EncodeSimulationEdit(app::ReleaseManualInputs{}, 0, 4, 0, {});
		});
}
} // namespace
int main()
{
	try
	{
		ObservedFrameScheduling();
		FirstPublicationAndStateFailures();
		std::cout << "PASS simulation edits\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
