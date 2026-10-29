#include "../TestSupport.h"
#include "automation/RecordingArchive.h"
#include "automation/RecordingEvidence.h"
#include "automation/RecordingRange.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "runtime/Runtime.h"
#include "simulation/BodyModel.h"
#include "simulation/CommandRecorder.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace simxr::tests
{
namespace wire = control;
namespace archive = automation;

struct Directory
{
	std::filesystem::path path =
		std::filesystem::temp_directory_path() /
		("simxr-archive-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

	Directory()
	{
		Require(std::filesystem::create_directory(path), "Create private test directory");
	}

	~Directory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}
};

template<typename Operation>
static void Reject(Operation operation)
{
	try
	{
		operation();
	}
	catch (const std::exception&)
	{
		return;
	}
	throw std::runtime_error("Invalid archive operation succeeded");
}

static wire::Message Response(CommandRecorder& recorder, wire::OpCode opcode, std::size_t offset, std::size_t count)
{
	const auto snapshot = recorder.Snapshot(offset, count);
	wire::Message message;
	message.opcode = static_cast<std::uint16_t>(opcode);
	message.fields = {wire::Field::U32(wire::tag::Error, 0),
					  wire::Field::Bytes(wire::tag::Document, wire::SimulationRecording(snapshot).Encode()),
					  wire::Field::U64(wire::tag::Offset, offset),
					  wire::Field::U64(wire::tag::Length, snapshot.transactions.size()),
					  wire::Field::Bool(wire::tag::End, offset + snapshot.transactions.size() == snapshot.total)};
	return message;
}

static CommandOutcome Outcome(std::size_t index, bool body)
{
	CommandOutcome outcome;
	outcome.transaction.id = index;
	outcome.appliedFrame = FrameId{index};
	outcome.appliedRuntimeTime = RuntimeTime{Duration{static_cast<std::int64_t>(index + 1000)}};
	outcome.result = CommandResult::Applied;
	if (body)
	{
		BodyObservation sample;
		sample.jointSet = BodyJointSet::WithoutArmsBD;
		sample.located = true;
		sample.requestedTime = *outcome.appliedRuntimeTime;
		sample.allJointPosesTracked = false;
		for (const auto joint : BodyModel::JointMap(sample.jointSet))
			sample.joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
		outcome.transaction.commands = {SetBodyControls{{}, sample}};
	}
	else
		outcome.transaction.commands = {SetHeadPose{}};
	return outcome;
}

static std::uint64_t ReadAll(archive::RecordingArchiveReader& reader)
{
	std::uint64_t count = 0;
	while (const auto page = reader.Next())
	{
		Require(page->offset == count && page->recording.transactions.size() <= 16,
				"Archive page order or memory bound changed");
		for (const auto& outcome : page->recording.transactions)
		{
			Require(outcome.id == ++count &&
						outcome.runtimeTime->value.count() == static_cast<std::int64_t>(count + 1000),
					"Archive lost outcome identity or exact replay anchor");
			const auto expected = Outcome(count, count > 1);
			Require(wire::EncodeSimulationCommand(outcome.commands.front()).value ==
						wire::EncodeSimulationCommand(expected.transaction.commands.front()).value,
					"Archive changed body metadata or command payload");
		}
	}
	return count;
}

static void Streaming()
{
	Directory root;
	const auto path = root.path / "take";
	CommandRecorder recorder(16);
	recorder.Start({}, true);
	Require(recorder.Observe(Outcome(1, false)), "Record origin race fixture");
	archive::RecordingArchiveWriter writer(path, Response(recorder, wire::OpCode::StartRecording, 0, 0));
	auto first = Response(recorder, wire::OpCode::ReadRecording, 0, 1);
	Require(writer.Append(first) == 1, "First flush acknowledgement changed");
	Reject([&] { writer.Append(first); });
	recorder.Acknowledge(1);
	for (std::size_t batch = 0; batch < 80; ++batch)
	{
		for (std::size_t index = 0; index < 16; ++index)
			Require(recorder.Observe(Outcome(batch * 16 + index + 2, true)),
					"Archive failed to drain bounded recorder");
		const auto page = Response(recorder, wire::OpCode::ReadRecording, batch * 16 + 1, 16);
		const auto through = writer.Append(page);
		Require(through == (batch + 1) * 16 + 1, "Flushed offset changed across ring reuse");
		recorder.Acknowledge(through);
	}
	Reject([&] { writer.Finish(Response(recorder, wire::OpCode::StopRecording, 1281, 0)); });
	recorder.Stop();
	writer.Finish(Response(recorder, wire::OpCode::StopRecording, 1281, 0));
	Require(writer.State().complete && writer.State().origin.version == 4, "Finalized archive lost schema promotion");
	Reject([&] { writer.Append(first); });
	Reject([&] { archive::RecordingArchiveWriter duplicate(path, first); });
	archive::RecordingArchiveReader reader(path);
	Require(ReadAll(reader) == 1281 && reader.State().complete && !reader.State().truncated &&
				reader.State().origin.version == 4,
			"Complete disk archive did not replay its full window history");
	for (const auto removed : {1u, 7u})
	{
		const auto damaged = root.path / ("partial-" + std::to_string(removed));
		std::filesystem::create_directory(damaged);
		std::filesystem::copy_file(path / "input.simxr", damaged / "input.simxr");
		std::filesystem::resize_file(damaged / "input.simxr",
									 std::filesystem::file_size(damaged / "input.simxr") - removed);
		archive::RecordingArchiveReader partial(damaged);
		Require(ReadAll(partial) == 1281 && !partial.State().complete && partial.State().truncated,
				"Incomplete final append discarded intact pages or claimed completion");
	}
	const auto oversized = root.path / "oversized";
	std::filesystem::create_directory(oversized);
	std::filesystem::copy_file(path / "input.simxr", oversized / "input.simxr");
	{
		std::fstream file(oversized / "input.simxr", std::ios::binary | std::ios::in | std::ios::out);
		file.seekp(8);
		const std::array<char, 4> length{char(0xff), char(0xff), char(0xff), char(0x7f)};
		file.write(length.data(), length.size());
	}
	Reject([&] { archive::RecordingArchiveReader invalid(oversized); });
	const auto corrupt = root.path / "corrupt";
	std::filesystem::create_directory(corrupt);
	std::filesystem::copy_file(path / "input.simxr", corrupt / "input.simxr");
	{
		std::fstream file(corrupt / "input.simxr", std::ios::binary | std::ios::in | std::ios::out);
		file.seekp(50);
		file.put('\xff');
	}
	Reject([&] { archive::RecordingArchiveReader invalid(corrupt); });
	{
		std::ofstream file(path / "input.simxr", std::ios::binary | std::ios::app);
		file.put('x');
	}
	Reject([&] {
		archive::RecordingArchiveReader trailing(path);
		ReadAll(trailing);
	});
}

static void Interrupted()
{
	Directory root;
	CommandRecorder recorder(2);
	recorder.Start({}, true);
	const auto start = Response(recorder, wire::OpCode::StartRecording, 0, 0);
	{
		archive::RecordingArchiveWriter writer(root.path / "take", start);
		recorder.Observe(Outcome(1, false));
		writer.Append(Response(recorder, wire::OpCode::ReadRecording, 0, 1));
		recorder.Stop();
		recorder.Start({}, true);
		Reject([&] { writer.Append(Response(recorder, wire::OpCode::ReadRecording, 0, 0)); });
		Require(writer.State().next == 1 && !writer.State().complete, "Rejected page changed archive progress");
	}
	archive::RecordingArchiveReader reader(root.path / "take");
	Require(ReadAll(reader) == 1 && !reader.State().complete && !reader.State().truncated,
			"Clean-boundary interruption was misreported as completed");
}

static void ReleaseRanges()
{
	for (const bool metadata : {false, true})
	{
		Directory root;
		CommandRecorder recorder;
		CommandRecordingOrigin origin;
		origin.runtimeTime = RuntimeTime{Duration{100}};
		origin.configuration.tracking.head.pose.position.x = 4;
		recorder.Start(origin, true);
		archive::RecordingArchiveWriter writer(root.path / "take",
											   Response(recorder, wire::OpCode::StartRecording, 0, 0));
		CommandOutcome release;
		release.result = CommandResult::Released;
		release.appliedFrame = FrameId{1};
		release.appliedRuntimeTime = RuntimeTime{Duration{110}};
		release.transaction.commands = {ReleaseControl{ControlChannel::Head}};
		auto state = std::make_shared<CommandRecordingOrigin>();
		state->frame = release.appliedFrame;
		state->runtimeTime = release.appliedRuntimeTime;
		state->configuration.tracking.head.pose.position.x = 9;
		const auto body = std::get<SetBodyControls>(Outcome(1, true).transaction.commands.front()).observation;
		state->configuration.tracking.bodyObservation = body;
		if (metadata)
			release.releaseState = state;
		recorder.Observe(release);
		auto next = Outcome(2, false);
		next.transaction.commands = {SetControllerControls{1, {}}};
		recorder.Observe(next);
		const auto page = Response(recorder, wire::OpCode::ReadRecording, 0, 2);
		const auto decoded = archive::DecodeRecordingPage(page);
		Require(decoded.recording.transactions.front().releaseState.has_value() == metadata,
				"Release snapshot presence changed across the codec");
		Require(decoded.recording.version == 3,
				"Release-only body metadata unexpectedly changed the parent recording schema");
		writer.Append(page);
		recorder.Stop();
		writer.Finish(Response(recorder, wire::OpCode::StopRecording, 0, 0));
		if (metadata)
		{
			const auto range = archive::ReadRecordingRange(root.path / "take", FrameId{2}, FrameId{2});
			const auto restoredBody = std::ranges::find_if(range.initial, [](const auto& command) {
				const auto* value = std::get_if<SetBodyControls>(&command);
				return value && value->observation.has_value();
			});
			Require(restoredBody != range.initial.end() &&
						std::get<SetBodyControls>(*restoredBody).observation->requestedTime == body->requestedTime &&
						std::get<SetBodyControls>(*restoredBody).observation->jointSet == body->jointSet,
					"Trim lost body data present only in the release snapshot");
			Require(std::get<SetHeadPose>(range.initial.front()).value.pose.position.x == 9,
					"Trim restored the old controlled origin instead of its recorded fallback");
			Require(std::ranges::any_of(range.initial,
										[](const auto& command) {
											const auto* eyes = std::get_if<SetEyeViews>(&command);
											return eyes && !eyes->value;
										}),
					"Release snapshot omitted the absence of physical eye views");
		}
		else
		{
			Reject([&] { (void)archive::ReadRecordingRange(root.path / "take", FrameId{2}, FrameId{2}); });
			archive::RecordingArchiveReader reader(root.path / "take");
			while (reader.Next())
			{}
			Require(reader.State().complete, "Legacy archive stopped being readable");
		}
	}
	for (const bool wrongFrame : {false, true})
	{
		CommandRecorder recorder;
		recorder.Start({}, true);
		CommandOutcome release;
		release.result = CommandResult::Released;
		release.appliedFrame = FrameId{1};
		release.appliedRuntimeTime = RuntimeTime{Duration{110}};
		release.transaction.commands = {ReleaseControl{ControlChannel::Head}};
		auto state = std::make_shared<CommandRecordingOrigin>();
		state->frame = FrameId{wrongFrame ? 2u : 1u};
		state->runtimeTime = RuntimeTime{Duration{wrongFrame ? 110 : 111}};
		release.releaseState = state;
		recorder.Observe(release);
		Reject([&] { (void)archive::DecodeRecordingPage(Response(recorder, wire::OpCode::ReadRecording, 0, 1)); });
	}
}

static void GameEvidence()
{
	for (const bool gap : {false, true})
	{
		Directory root;
		Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
		wire::ControlService service(runtime);
		const auto connection = *service.Connect();
		std::uint64_t requestId = 0;
		const auto call = [&](wire::OpCode opcode, std::vector<wire::Field> fields) {
			wire::Message request;
			request.requestId = wire::RequestId{++requestId};
			request.opcode = static_cast<std::uint16_t>(opcode);
			request.fields = std::move(fields);
			return service.Handle(connection, request);
		};
		call(wire::OpCode::Hello, {});
		const auto game = runtime.Game().Register(SIMXR_GAME_CAN_RESET);
		Require(runtime.Game().Publish(game, "initial state"), "SDK origin setup");
		constexpr std::int64_t anchor = 9007199254740993LL;
		const auto read = [&](std::uint64_t offset, std::uint64_t count) {
			auto result = call(wire::OpCode::ReadGameObservations, {wire::Field::U64(wire::tag::Offset, offset),
																	wire::Field::U64(wire::tag::Length, count)});
			result.fields.insert(result.fields.end(),
								 {wire::Field::U64(wire::tag::Session, 1), wire::Field::U64(wire::tag::Generation, 1),
								  wire::Field::U32(wire::tag::ClockDomain, 1),
								  wire::Field::U64(wire::tag::ClockRevision, 7),
								  wire::Field::I64(wire::tag::NativeTime, anchor),
								  wire::Field::I64(wire::tag::RuntimeTimeBefore, anchor + 3),
								  wire::Field::I64(wire::tag::RuntimeTimeAfter, anchor + 9),
								  wire::Field::I64(wire::tag::ClockResolution, 1)});
			return result;
		};
		CommandRecorder recorder;
		recorder.Start({}, true);
		auto start = Response(recorder, wire::OpCode::StartRecording, 0, 0);
		start.fields.push_back(wire::Field::U64(wire::tag::Session, 1));
		start.fields.push_back(wire::Field::U64(wire::tag::Generation, 1));
		const auto origin = read(0, 0);
		auto wrongOrigin = origin;
		for (auto& field : wrongOrigin.fields)
			if (field.tag == wire::tag::Generation)
				field = wire::Field::U64(wire::tag::Generation, 2);
		Reject([&] { archive::RecordingArchiveWriter invalid(root.path / "wrong", start, wrongOrigin); });
		Require(!std::filesystem::exists(root.path / "wrong"), "Wrong SDK origin created an archive");
		auto partialClock = origin;
		std::erase_if(partialClock.fields, [](const auto& field) { return field.tag == wire::tag::NativeTime; });
		Reject([&] { (void)archive::DecodeRecordedGamePage(partialClock); });
		archive::RecordingArchiveWriter writer(root.path / "sdk", start, origin);
		for (unsigned index = 0; index < (gap ? 1100u : 2u); ++index)
			Require(runtime.Game().Publish(game, std::to_string(index)), "SDK journal setup");
		for (;;)
		{
			const auto message = read(writer.State().game->next, 16);
			const auto decoded = archive::DecodeRecordedGamePage(message);
			auto invalid = message;
			for (auto& field : invalid.fields)
				if (field.tag == wire::tag::Generation)
					field = wire::Field::U64(wire::tag::Generation, 2);
			Reject([&] { writer.AppendGame(invalid); });
			writer.AppendGame(message);
			if (decoded.journal.next == decoded.journal.total)
				break;
		}
		writer.AppendGame(read(writer.State().game->next, 0), "end");
		recorder.Stop();
		writer.Finish(Response(recorder, wire::OpCode::StopRecording, 0, 0));
		archive::RecordingArchiveReader reader(root.path / "sdk");
		Require(reader.State().game->origin.clock->nativeNanoseconds == anchor &&
					reader.State().game->origin.clock->runtimeAfter.value.count() == anchor + 9 &&
					reader.State().game->origin.journal.state.telemetry == "initial state",
				"SDK archive changed exact clock uncertainty or initial state");
		std::size_t observed = 0;
		while (const auto event = reader.NextEvent())
			if (const auto* page = std::get_if<archive::RecordedGamePage>(&*event))
				observed += page->journal.observations.size();
		Require(observed == (gap ? 1024u : 2u) && reader.State().game->gap == gap && reader.State().game->complete &&
					reader.State().complete == !gap,
				"SDK eviction gap became a complete playtest or lost recoverable evidence");
		const auto evidence = archive::ReadRecordingEvidence(root.path / "sdk");
		Require(evidence.available && evidence.complete == !gap && evidence.gap == gap &&
					evidence.entries.size() == (gap ? 16u : 2u) && evidence.more == gap &&
					evidence.initial.telemetry == "initial state",
				"Evidence inspection hid gaps or lost bounded paging");
		for (const auto& entry : evidence.entries)
			Require(!entry.first && !entry.last, "SDK evidence invented missing input boundaries");
		if (gap)
		{
			const auto next = archive::ReadRecordingEvidence(root.path / "sdk", evidence.next);
			Require(next.entries.size() == 16 && next.entries.front().observation.sequence == evidence.next,
					"Evidence page navigation duplicated or skipped records");
		}
		std::stop_source cancellation;
		cancellation.request_stop();
		Reject([&] { (void)archive::ReadRecordingEvidence(root.path / "sdk", 0, cancellation.get_token()); });
		const auto input = root.path / "sdk" / "input.simxr";
		std::filesystem::resize_file(input, std::filesystem::file_size(input) - 1);
		archive::RecordingArchiveReader truncated(root.path / "sdk");
		while (truncated.NextEvent())
		{}
		Require(truncated.State().game->complete && !truncated.State().complete && truncated.State().truncated,
				"SDK completion hid an interrupted input archive footer");
		service.Disconnect(connection);
	}
}
} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::Streaming();
		simxr::tests::Interrupted();
		simxr::tests::ReleaseRanges();
		simxr::tests::GameEvidence();
		std::cout << "Bounded recording archive and recovery checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
