#include "../TestSupport.h"
#include "app/services/DebuggerPanelController.h"
#include "app/services/ImageExportTask.h"
#include "app/services/InspectionWorkspace.h"
#include "app/services/TimelineHistory.h"
#include "graphics/SnapshotBudget.h"
#include "graphics/ColorPixels.h"
#include "app/services/Utf8Path.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#if defined(__linux__)
#include "../control/FakeImageInspection.h"
#include "control/LocalControlServer.h"
#endif

namespace
{
namespace app = simxr::app;
namespace wire = simxr::control;
namespace tag = wire::tag;
using namespace std::chrono_literals;
wire::Message Message(app::PanelFields fields)
{
	wire::Message result;
	result.fields = std::move(fields);
	return result;
}
wire::Field Frame(std::uint64_t id)
{
	return wire::Field::Bytes(tag::Frame, wire::EncodeFields(app::PanelFields{wire::Field::U64(tag::FrameId, id)}));
}
app::SessionView TimelineState(std::uint64_t epoch, std::uint64_t frame, double seconds)
{
	app::SessionView state;
	state.endpoint = "timeline-fixture";
	state.connected = state.sessionAvailable = state.sessionRunning = true;
	state.epoch = epoch;
	state.session = 11;
	state.generation = 5;
	state.latestFrame = frame;
	state.historySeconds = seconds;
	return state;
}
void TimelineRecordingIdentity()
{
	app::HistoryStore frames(3);
	app::TimelineHistory timeline;
	for (std::uint64_t frame = 10; frame <= 12; ++frame)
	{
		frames.Append({{11, 5, frame}, 1, 0, double(frame), 10});
		timeline.Observe(TimelineState(1, frame, double(frame)), frame != 12);
	}
	timeline.Resolve(frames);
	Require(timeline.Clips().size() == 1 && !timeline.Recording(), "Recording did not become one completed clip");
	const auto& clip = timeline.Clips().front();
	Require(clip.Range(frames) == std::pair<std::size_t, std::size_t>{0, 2},
			"Completed recording lost its exact inclusive source endpoints");
	frames.Append({{11, 5, 13}, 1, 0, 13, 10});
	Require(!clip.Range(frames) && frames.Find({11, 5, 12}, 1),
			"Partly evicted recording substituted a newer frame for its missing endpoint");
	for (std::uint64_t frame = 10; frame <= 12; ++frame)
		frames.Append({{11, 5, frame}, 2, 0, double(frame + 10), 10});
	timeline.Resolve(frames);
	Require(!clip.Range(frames) && clip.first.identity == app::FrameIdentity{11, 5, 10} &&
				clip.last.identity == app::FrameIdentity{11, 5, 12} && clip.first.epoch == 1 && clip.last.epoch == 1,
			"Reconnect retargeted a completed recording to reused frame identities");
}
void TimelineScriptReconnect()
{
	app::TimelineHistory timeline;
	simxr::automation::RigSnapshot script;
	script.phase = simxr::automation::RigPhase::Running;
	auto state = TimelineState(1, 10, 10);
	const auto observe = [&](std::uint64_t run)
	{
		timeline.Observe(state, false);
		timeline.ObserveScript(state, run, script, true);
	};
	observe(7);
	Require(timeline.Script() && timeline.Script()->first.Run() == app::RuntimeFrameRun{1, 11, 5},
			"Active script did not bind to its original runtime epoch");
	state.latestFrame = 11;
	state.historySeconds = 11;
	observe(7);
	state = TimelineState(2, 1, 12);
	observe(7);
	Require(!timeline.Script() && timeline.Clips().size() == 1 &&
				timeline.Clips().front().last.Run() == app::RuntimeFrameRun{1, 11, 5} &&
				timeline.Clips().front().last.identity.frame == 11,
			"Old script reopened on reconnect with matching endpoint, session and generation");
	script.phase = simxr::automation::RigPhase::Complete;
	observe(7);
	Require(!timeline.Script() && timeline.Clips().size() == 1, "Old script completion created a new-epoch clip");
	script.phase = simxr::automation::RigPhase::Running;
	observe(8);
	Require(timeline.Script() && timeline.ScriptRun() == 8 &&
				timeline.Script()->first.Run() == app::RuntimeFrameRun{2, 11, 5},
			"New script could not bind after the prior epoch retired");
}
void TimelineCompletedScripts()
{
	for (const auto phase : {simxr::automation::RigPhase::Complete, simxr::automation::RigPhase::Cancelled})
	{
		app::TimelineHistory timeline;
		auto state = TimelineState(1, 10, 10);
		simxr::automation::RigSnapshot script;
		script.phase = simxr::automation::RigPhase::Running;
		const auto observe = [&](std::uint64_t run)
		{
			timeline.Observe(state, false);
			timeline.ObserveScript(state, run, script, true);
		};
		timeline.ReplayTake(7, "original-take.luau", state);
		observe(7);
		const auto* active = timeline.FindScript(7);
		Require(timeline.Script() && active && active == &*timeline.Script() && active->scriptRun == 7 &&
					active->take == "original-take.luau" && !timeline.FindScript(0) && !timeline.FindScript(8),
				"Active replay lookup lost its script identity or saved take label");
		const auto id = active->id;
		state.latestFrame = 12;
		state.historySeconds = 12;
		script.phase = phase;
		observe(7);
		const auto* completed = timeline.FindScript(7);
		Require(!timeline.Script() && completed && completed->id == id && completed->first.identity.frame == 10 &&
					completed->last.identity.frame == 12,
				"Completed or cancelled script lookup lost its original span endpoints");
		state = TimelineState(2, 1, 13);
		script.phase = simxr::automation::RigPhase::Running;
		observe(8);
		const auto* original = timeline.FindScript(7);
		const auto* current = timeline.FindScript(8);
		Require(original && original->id == id && original->first.Run() == app::RuntimeFrameRun{1, 11, 5} &&
					original->last.Run() == app::RuntimeFrameRun{1, 11, 5} && original->last.identity.frame == 12 &&
					original->take == "original-take.luau" && current && current->id != id &&
					current->first.Run() == app::RuntimeFrameRun{2, 11, 5} && current->take.empty(),
				"New runtime scope retargeted the old script span or inherited its replay label");
	}
}
void TimelineImmediateReplayCompletion()
{
	const auto launch = TimelineState(1, 10, 10);
	for (const auto phase : {simxr::automation::RigPhase::Complete, simxr::automation::RigPhase::Cancelled})
	{
		app::TimelineHistory timeline;
		timeline.ReplayTake(7, "short-take.luau", launch);
		auto state = TimelineState(1, 12, 12);
		simxr::automation::RigSnapshot script;
		script.phase = phase;
		timeline.Observe(state, false);
		timeline.ObserveScript(state, 7, script, true);
		const auto* completed = timeline.FindScript(7);
		Require(!timeline.Script() && completed && completed->take == "short-take.luau" &&
					completed->first.Run() == app::RuntimeFrameRun{1, 11, 5} && completed->first.identity.frame == 10 &&
					completed->first.seconds == 10 && completed->last.identity.frame == 12 &&
					completed->last.seconds == 12,
				"Replay completed before first observation lost its launch-to-completion span");
		timeline.Observe(state, false);
		timeline.ObserveScript(state, 7, script, true);
		Require(timeline.Clips().size() == 1, "Repeated completion created duplicate replay spans");
	}
	app::TimelineHistory reconnect;
	reconnect.ReplayTake(7, "retired-take.luau", launch);
	const auto state = TimelineState(2, 12, 12);
	simxr::automation::RigSnapshot script;
	script.phase = simxr::automation::RigPhase::Running;
	reconnect.Observe(state, false);
	reconnect.ObserveScript(state, 7, script, true);
	Require(!reconnect.Script() && !reconnect.FindScript(7),
			"Replay first observed after reconnect bound to the new epoch instead of its launch target");
}
void TimelinePauseEndpoints()
{
	for (const bool alreadyAdvanced : {false, true})
	{
		app::TimelineHistory timeline;
		auto state = TimelineState(1, 10, 10);
		state.manual = true;
		timeline.Observe(state, false);
		state.latestFrame = 11;
		state.historySeconds = 11;
		timeline.Observe(state, false);
		state.manual = false;
		if (alreadyAdvanced)
		{
			state.latestFrame = 12;
			state.historySeconds = 12;
		}
		timeline.Observe(state, false);
		Require(!timeline.Pause() && timeline.Pauses().size() == 1, "Continue did not finish the active pause span");
		state.latestFrame = 12;
		state.historySeconds = 12;
		timeline.Observe(state, false);
		state.latestFrame = 13;
		state.historySeconds = 13;
		timeline.Observe(state, false);
		Require(timeline.Pauses().front().first.identity.frame == 10 &&
					timeline.Pauses().front().last.identity.frame == 12 && timeline.Pauses().front().last.seconds == 12,
				"Pause endpoint did not stop at the first post-Continue frame");
	}
	app::TimelineHistory timeline;
	auto state = TimelineState(1, 20, 20);
	state.manual = true;
	timeline.Observe(state, false);
	state.manual = false;
	timeline.Observe(state, false);
	state.generation = 6;
	state.latestFrame = 1;
	state.historySeconds = 21;
	timeline.Observe(state, false);
	Require(timeline.Pauses().size() == 1 && timeline.Pauses().front().last.Run() == app::RuntimeFrameRun{1, 11, 5} &&
				timeline.Pauses().front().last.identity.frame == 20 && timeline.Pauses().front().last.seconds == 20,
			"Pending pause endpoint extended into the next runtime run");
}
void TimelineSavedRecordingScope()
{
	app::TimelineHistory timeline;
	for (const auto epoch : {1U, 2U})
	{
		timeline.Observe(TimelineState(epoch, 10, epoch * 10), true);
		timeline.Observe(TimelineState(epoch, 11, epoch * 10 + 1), false);
	}
	Require(timeline.Clips().size() == 2, "Recording scope fixture lost one completed clip");
	timeline.SavedRecording("wrong-epoch.luau", {3, 11, 5});
	timeline.SavedRecording("wrong-session.luau", {2, 12, 5});
	timeline.SavedRecording("wrong-run.luau", {2, 11, 6});
	Require(timeline.Clips()[0].take.empty() && timeline.Clips()[1].take.empty(),
			"Saved recording attached to a clip from another runtime scope");
	timeline.SavedRecording("original.luau", {1, 11, 5});
	Require(timeline.Clips()[0].take == "original.luau" && timeline.Clips()[1].take.empty(),
			"Delayed recording save attached to the newest clip instead of its original run");
	timeline.SavedRecording("reconnected.luau", {2, 11, 5});
	Require(timeline.Clips()[0].take == "original.luau" && timeline.Clips()[1].take == "reconnected.luau",
			"Recording save failed to attach to its matching runtime scope");
}
void SessionRunningUpdates()
{
	app::RuntimePanelRequests requests;
	app::DebuggerPanelController debugger(requests);
	debugger.ChooseSession({11, 5, false});
	const auto scope = debugger.Scope();
	for (const bool running : {true, false, true})
	{
		debugger.Handle({app::PanelWork::Sessions}, Message({wire::Field::Bytes(tag::Sessions,
			wire::EncodeFields(app::PanelFields{wire::Field::Bytes(1, wire::EncodeFields(app::PanelFields{
				wire::Field::U64(tag::Session, 11), wire::Field::U64(tag::Generation, 5),
				wire::Field::Bool(tag::Running, running)}))}))}));
		Require(debugger.Selected().running == running && debugger.Scope() == scope,
			"Session discovery failed to refresh running state within the same scope");
	}
}
void MissingCaptureKeepsSession()
{
	app::RuntimePanelRequests requests;
	app::DebuggerPanelController debugger(requests);
	const auto select = [&]
	{
		debugger.ChooseSession({11, 5, true});
		debugger.Handle({app::PanelWork::Status}, Message({wire::Field::Bool(tag::Manual, true)}));
		debugger.Handle({app::PanelWork::Acquire}, Message({wire::Field::U64(tag::Lease, 17)}));
	};
	for (const auto kind : {app::PanelWork::Capture, app::PanelWork::Retain, app::PanelWork::Inspect})
	{
		select();
		const auto scope = debugger.Scope();
		debugger.Failed({kind, 7, 11, 5}, wire::ErrorCode::NotFound, "Snapshot is absent or released");
		Require(debugger.Scope() == scope && debugger.Selected().running && debugger.HasLease() && debugger.Manual() &&
				!debugger.Status().fields.empty(),
				"Evicted capture retired the running session or its debugger state");
		for (const auto error : {wire::ErrorCode::Closed, wire::ErrorCode::StaleGeneration})
		{
			select();
			debugger.Failed({kind, 7, 11, 5}, error, "Session retired");
			Require(!debugger.Selected().id && !debugger.HasLease() && !debugger.Manual() &&
					debugger.Status().fields.empty(),
					"Retired session survived a capture request failure");
		}
	}
	select();
	debugger.Failed({app::PanelWork::Status, 0, 11, 5}, wire::ErrorCode::NotFound, "Session not found");
	Require(!debugger.Selected().id && !debugger.HasLease() && debugger.Status().fields.empty(),
			"Missing session status was mistaken for an evicted capture");
}
void ScopedInspectionReaders()
{
	app::RuntimePanelRequests requests;
	app::InspectionWorkspace workspace(requests);
	const app::PanelScope original{1, 11, 5}, restarted{1, 11, 6}, reconnect{2, 11, 6};
	workspace.SetScope(original);
	Require(workspace.Add(7, 0) && workspace.Select(7), "Retain provisional capture identity");
	const auto originalLocal = workspace.Selected()->local;
	Require(workspace.Add(7, 101) && workspace.Captures().size() == 1 && workspace.Selected()->frame == 101 &&
				workspace.Selected()->local == originalLocal,
			"Completed capture did not update its provisional entry");
	workspace.Handle({app::PanelWork::Inspect, 7},
					 Message({wire::Field::U64(tag::InspectionId, 19),
							  wire::Field::Text(tag::Detail, "original immutable document")}));
	workspace.MarkUnavailable(7);
	Require(!workspace.Select(7) && workspace.Selected()->released && workspace.Selected()->inspection == 19 &&
			workspace.CaptureIds().empty() &&
			app::panel::Text(workspace.Selected()->document, tag::Detail) == "original immutable document",
			"Evicted capture remained fetchable or discarded its retained CPU document");
	workspace.Handle({app::PanelWork::Inspect, 7},
		Message({wire::Field::U64(tag::InspectionId, 20), wire::Field::Text(tag::Detail, "late response")}));
	Require(workspace.Selected()->inspection == 19 &&
			app::panel::Text(workspace.Selected()->document, tag::Detail) == "original immutable document",
			"Late inspection response replaced the document of an unavailable capture");
	workspace.Handle({app::PanelWork::Revision, 31}, Message({wire::Field::Text(tag::Detail, "original revision")}));
	workspace.SetScope(restarted);
	Require(workspace.Add(7, 202) && workspace.Select(7), "Reuse remote capture ID in a new generation");
	Require(workspace.Selected()->scope == restarted && workspace.Selected()->frame == 202 &&
			!workspace.Selected()->released,
			"Selection substituted an older generation");
	workspace.Handle({app::PanelWork::Capture, 7}, Message({wire::Field::Text(tag::Phase, "clear"), Frame(203)}));
	workspace.SelectLocal(originalLocal);
	Require(workspace.Selected()->frame == 101 && workspace.Selected()->scope == original &&
				app::panel::Text(workspace.Selected()->document, tag::Detail) == "original immutable document",
			"A later capture response mutated the original CPU document");
	workspace.SetScope(reconnect);
	Require(!workspace.Revision(31, reconnect.epoch), "A revision from another endpoint epoch survived as a live lookup");
	workspace.Handle({app::PanelWork::Revision, 31}, Message({wire::Field::Text(tag::Detail, "new revision")}));
	Require(workspace.Revision(31, reconnect.epoch) && !workspace.Revision(31, original.epoch),
		"A historical document resolved a reused revision ID from another endpoint");
	workspace.SelectLocal(originalLocal);
	Require(workspace.Selected() && workspace.Selected()->inspection == 19,
			"Reconnect lost retained historical metadata");
	workspace.ReleaseSelected();
	Require(!workspace.Selected()->inspection && workspace.Selected()->document.fields.empty() &&
				workspace.Selected()->released,
			"Releasing historical CPU inspection did not retire its document");

	app::InspectionWorkspace bounded(requests);
	bounded.SetScope(original);
	const auto capacity = simxr::SnapshotBudget::Capacity();
	for (std::uint64_t id = 1; id <= capacity; ++id)
		Require(bounded.Add(id, id), "Admit bounded catalog entry");
	Require(bounded.Add(capacity + 1, capacity + 1) && bounded.Captures().size() == capacity &&
		bounded.Captures().front().id == 2, "Catalog did not evict oldest unselected entry");
	bounded.MarkReleased(2);
	Require(bounded.Add(capacity + 2, capacity + 2) && bounded.Captures().size() == capacity, "Released catalog entry did not make room");
	bounded.Select(3);
	bounded.Handle({app::PanelWork::Inspect, 3},
				   Message({wire::Field::U64(tag::InspectionId, 77),
							wire::Field::Text(tag::Detail, "kept until catalog pressure")}));
	const auto retainedLocal = bounded.Selected()->local;
	bounded.SetScope(reconnect);
	bounded.SelectLocal(retainedLocal);
	Require(bounded.Captures().size() == capacity && bounded.Selected()->inspection == 77 &&
				app::panel::Text(bounded.Selected()->document, tag::Detail) == "kept until catalog pressure",
			"Reconnect prematurely discarded historical capture bytes");
	Require(bounded.Add(101, 101) && bounded.Select(101), "Select a new-run frame");
	for (std::uint64_t id = 102; id <= 101 + capacity; ++id)
		Require(bounded.Add(id, id), "Expired endpoint IDs permanently exhausted the capture catalog");
	Require(bounded.Captures().size() == capacity && bounded.Captures().front().scope == reconnect &&
				bounded.Captures().back().scope == reconnect,
			"Reconnect catalog recycling exceeded its bound");
	bounded.Select(101);
	const auto anotherSessionLocal = bounded.Selected()->local;
	bounded.SetScope({reconnect.epoch, 99, 1});
	bounded.SelectLocal(anotherSessionLocal);
	bounded.ReleaseSelected();
	Require(bounded.Selected()->released && bounded.Add(133, 133) && bounded.Captures().size() == capacity,
			"Historical selection from another session remained non-recyclable");

	app::InspectionWorkspace pinned(requests);
	pinned.SetScope(original);
	for (std::uint64_t id = 1; id <= capacity; ++id)
	{
		Require(pinned.Add(id, id), "Admit pinned capture");
		pinned.Handle({app::PanelWork::Capture, id},
			Message({wire::Field::Text(tag::Phase, "ready"), wire::Field::Bool(tag::Pinned, true), Frame(id)}));
	}
	Require(!pinned.Add(capacity + 1, capacity + 1), "Catalog evicted an active pinned capture");
	Require(pinned.Select(1), "Select pinned capture for release");
	pinned.Handle({app::PanelWork::Inspect, 1}, Message({wire::Field::U64(tag::InspectionId, 81)}));
	pinned.MarkReleased(1);
	Require(pinned.Selected()->released && !pinned.Selected()->inspection && pinned.Selected()->document.fields.empty(),
		"Capture release retained an inspection reader");
	pinned.Handle({app::PanelWork::Inspect, 1}, Message({wire::Field::U64(tag::InspectionId, 82)}));
	Require(!pinned.Selected()->inspection && pinned.Selected()->document.fields.empty(),
		"Late inspection response revived a released capture");
	pinned.Select(2);
	Require(pinned.Add(capacity + 1, capacity + 1) && pinned.Captures().front().id == 2,
		"Released pin permanently blocked catalog admission");
	pinned.SetScope(reconnect);
	pinned.SelectLocal(pinned.Captures().front().local);
	pinned.ReleaseSelected();
	pinned.SelectLocal(pinned.Captures().back().local);
	Require(pinned.Add(capacity + 2, capacity + 2) && pinned.Captures().front().id == 3,
		"Released historical pin blocked the new run");
}
std::filesystem::path Directory()
{
	const auto name =
		"simxr-panel-state-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const auto directory = std::filesystem::temp_directory_path() / name;
	Require(std::filesystem::create_directory(directory), "Create panel test directory");
	return directory;
}
std::string PathString(const std::filesystem::path& path)
{
	const auto utf8 = path.u8string();
	return std::string(utf8.begin(), utf8.end());
}
std::vector<std::byte> Read(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	Require(stream.good(), "Open explicit export artifact");
	const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
	std::vector<std::byte> result;
	for (const auto byte : bytes)
		result.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
	return result;
}
void FileWorkerAndChunkValidation()
{
	const auto directory = Directory();
	struct Cleanup
	{
		std::filesystem::path path;
		~Cleanup()
		{
			std::filesystem::remove_all(path);
		}
	} cleanup{directory};
	const std::vector<std::byte> expected{std::byte{0}, std::byte{0xff}, std::byte{0x31}, std::byte{0x80}};
	{
		app::ImageExportWriter writer;
		Require(writer.Save(PathString(directory / "exact.rgba"), expected), "Admit explicit CPU file job");
		std::this_thread::sleep_for(50ms);
		Require(writer.Busy() && !writer.Save(PathString(directory / "lost-result.rgba"), expected),
			"An unread file result was overwritten by a later export");
		std::optional<app::ImageExportWriteResult> result;
		Eventually(
			[&]
			{
				result = writer.TakeResult();
				return result.has_value();
			},
			"File worker did not publish completion");
		Require(result->status == app::ImageExportWriteResult::Status::Saved &&
					Read(directory / "exact.rgba") == expected,
				"Export worker changed RGBA bytes");
		const app::ImageExportMetadata metadata{7, 8, 9, 10, 1, 1, 1, 37, 2, 3, 4};
		Require(writer.Save(PathString(directory / "frame.ppm"), expected, metadata), "Admit viewable image");
		Eventually([&] { result = writer.TakeResult(); return result.has_value(); }, "PPM write did not settle");
		const auto ppm = Read(directory / "frame.ppm");
		const std::string header = "P6\n# session=7 generation=8 frame=9 capture=10\n"
			"# target=2 layer=3 view=4 slices=1 slice_height=1 format=37 alpha=omitted\n1 1\n255\n";
		Require(result->status == app::ImageExportWriteResult::Status::Saved && ppm.size() == header.size() + 3 &&
			std::equal(header.begin(), header.end(), reinterpret_cast<const char*>(ppm.data())) &&
			std::equal(expected.begin(), expected.begin() + 3, ppm.begin() + header.size()),
			"Viewable export lost exact RGB, dimensions or frame/target identity");
		struct ColorCase
		{
			uint32_t format;
			std::vector<std::byte> bytes;
			std::array<std::byte, 3> rgb;
		};
		const std::array colorCases{
			ColorCase{44, {std::byte{0x31}, std::byte{255}, std::byte{0}, std::byte{128}},
				{std::byte{0}, std::byte{255}, std::byte{0x31}}},
			ColorCase{97, {std::byte{0}, std::byte{0xb8}, std::byte{0}, std::byte{0x38},
				std::byte{0}, std::byte{0x40}, std::byte{0}, std::byte{0x34}},
				{std::byte{0}, std::byte{188}, std::byte{255}}},
			ColorCase{109, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xbf},
				std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x3f},
				std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x40},
				std::byte{0}, std::byte{0}, std::byte{0x80}, std::byte{0x3e}},
				{std::byte{0}, std::byte{188}, std::byte{255}}},
			ColorCase{64, {std::byte{0}, std::byte{0}, std::byte{0xf8}, std::byte{0x7f}},
				{std::byte{0}, std::byte{188}, std::byte{255}}}};
		for (const auto& color : colorCases)
		{
			auto image = metadata;
			image.format = color.format;
			const auto destination = directory / (std::to_string(color.format) + ".ppm");
			Require(simxr::ColorPixelBytes(color.format) == color.bytes.size(), "Export pixel stride mismatch");
			if (color.format == 97 || color.format == 109)
				Require(simxr::DecodeColorPixel(color.format, color.bytes) == std::array{-0.5f, 0.5f, 2.0f, 0.25f},
					"Float decode clamped HDR values or lost alpha");
			Require(writer.Save(PathString(destination), color.bytes, image), "Admit HDR/BGRA PPM");
			Eventually([&] { result = writer.TakeResult(); return result.has_value(); }, "HDR/BGRA write did not settle");
			const auto output = Read(destination);
			Require(result->status == app::ImageExportWriteResult::Status::Saved && output.size() > 3 &&
				std::equal(color.rgb.begin(), color.rgb.end(), output.end() - 3), "HDR/BGRA display conversion mismatch");
		}
		if (std::filesystem::exists("/dev/full"))
		{
			const auto full = directory / "full.rgba";
			std::filesystem::create_directory(full);
			Require(writer.Save(PathString(full), expected), "Admit failing atomic publication");
			Eventually([&] { result = writer.TakeResult(); return result.has_value(); }, "Write failure did not settle");
			Require(result->status == app::ImageExportWriteResult::Status::WriteFailed, "Atomic publication failure reported success");
		}

		bool rejected = false;
		try
		{
			auto wrong = metadata;
			wrong.width = 2;
			writer.Save(PathString(directory / "bad.ppm"), expected, wrong);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected && !std::filesystem::exists(directory / "bad.ppm"),
			"Mismatched image dimensions wrote a misleading artifact");
		const auto unicode = directory / app::Utf8Path("目-étude.rgba");
		Require(writer.Save(PathString(unicode), expected), "Admit UTF-8 destination");
		Eventually(
			[&]
			{
				result = writer.TakeResult();
				return result.has_value();
			},
			"Unicode file job did not settle");
		Require(result->status == app::ImageExportWriteResult::Status::Saved && result->path == PathString(unicode) &&
					Read(unicode) == expected,
				"Explicit Unicode destination or result path lost UTF-8 fidelity");
		Require(writer.Save(PathString(directory / "missing" / "bad.rgba"), expected), "Admit failing file job");
		Eventually(
			[&]
			{
				result = writer.TakeResult();
				return result.has_value();
			},
			"File worker did not publish failure");
		Require(result->status == app::ImageExportWriteResult::Status::OpenFailed && !writer.Busy(),
				"File open failure became a success or stranded admission");
	}
	{
		app::ImageExportWriter writer;
		Require(writer.Save(PathString(directory / "on-close.rgba"), expected), "Admit pending shutdown job");
	}
	Require(Read(directory / "on-close.rgba") == expected, "Writer shutdown abandoned an accepted byte buffer");

	app::RuntimePanelRequests requests;
	app::ImageExportTask task(requests);
	const auto start = Message({wire::Field::U64(tag::ExportId, 9), wire::Field::U64(tag::TotalBytes, 4)});
	const auto rejected = [&](wire::Message chunk)
	{
		task.Handle({app::PanelWork::Export}, start);
		bool threw = false;
		try
		{
			task.Handle({app::PanelWork::ExportRead, 9}, chunk);
		}
		catch (const std::runtime_error&)
		{
			threw = true;
		}
		Require(threw && !task.Busy(), "Malformed export chunk retained remote export admission");
	};
	rejected(Message({wire::Field::U64(tag::Offset, 1), wire::Field::U64(tag::Length, 4),
					  wire::Field::Bytes(tag::Bytes, expected), wire::Field::Bool(tag::End, true)}));
	rejected(Message({wire::Field::U64(tag::Offset, 0), wire::Field::U64(tag::Length, 4),
					  wire::Field::Bytes(tag::Bytes, expected), wire::Field::Bool(tag::End, false)}));
	rejected(Message({wire::Field::U64(tag::Offset, 0), wire::Field::U64(tag::Length, 0),
					  wire::Field::Bytes(tag::Bytes, {}), wire::Field::Bool(tag::End, false)}));
}
#if defined(__linux__)
void ActualConnectionAndDebugger()
{
	const auto directory = Directory();
	struct Cleanup
	{
		std::filesystem::path path;
		~Cleanup()
		{
			std::filesystem::remove_all(path);
		}
	} cleanup{directory};
	wire::test::Fixture fixture;
	wire::LocalControlServer server(fixture.service, (directory / "endpoint.sock").string());
	app::RuntimePanelRequests requests;
	app::DebuggerPanelController debugger(requests);
	requests.Connect(server.Path());
	Eventually(
		[&]
		{
			requests.RefreshEpoch();
			return requests.Snapshot().state == app::RuntimeConnectionState::Connected;
		},
		"Panel request owner did not negotiate asynchronously");
	const auto firstEpoch = requests.Epoch();
	const auto pump = [&]
	{
		requests.FlushReleases();
		debugger.Poll();
		for (auto& reply : requests.Poll())
		{
			Require(reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error) == 0,
					"Actual panel request returned an unexpected failure");
			debugger.Handle(reply.task, reply.response.packet.message);
		}
	};
	Eventually(
		[&]
		{
			pump();
			return debugger.Selected().id != 0;
		},
		"Debugger owner did not discover actual core session");
	{
		class PendingOutput final : public simxr::FrameOutput
		{
		  public:
			std::atomic<bool> complete{false};
			std::shared_ptr<simxr::FrameReservation> slot;
			simxr::CompletionStatus PollCompletion() const noexcept override
			{
				return complete ? simxr::CompletionStatus::Complete : simxr::CompletionStatus::Pending;
			}
		};
		app::InspectionWorkspace workspace(requests);
		workspace.SetScope(debugger.Scope());
		const auto reserved = fixture.session->ReserveCapture();
		Require(reserved.result == simxr::SnapshotAdmission::Accepted &&
				fixture.session->Scheduler().GrantPermits() == XR_SUCCESS, "Reserve pending CPU inspection");
		simxr::FramePrediction prediction;
		Require(fixture.session->WaitFrame(prediction) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS,
				"Begin pending CPU inspection frame");
		Require(workspace.Add(reserved.id.value, prediction.id.value) && workspace.Select(reserved.id.value),
				"Select pending CPU inspection");
		workspace.InspectSelected();
		Require(!requests.Has(app::PanelWork::Inspect), "Inspection was requested before capture metadata arrived");
		unsigned inspections = 0;
		const auto poll = [&]
		{
			for (auto& reply : requests.Poll())
			{
				const auto& message = reply.response.packet.message;
				Require(reply.response.error.empty() && app::panel::U32(message, tag::Error) == 0,
						"Pending CPU inspection issued an unavailable read");
				inspections += reply.task.kind == app::PanelWork::Inspect;
				if (!workspace.Handle(reply.task, message))
					debugger.Handle(reply.task, message);
			}
		};
		const auto refresh = [&]
		{
			auto fields = debugger.SessionFields();
			fields.push_back(wire::Field::U64(tag::CaptureId, reserved.id.value));
			Require(requests.Send(wire::OpCode::Capture, app::PanelWork::Capture, std::move(fields), reserved.id.value),
					"Request pending capture metadata");
			Eventually([&] { poll(); return !requests.Has(app::PanelWork::Capture); }, "Capture metadata did not arrive");
		};
		refresh();
		workspace.InspectSelected();
		Require(!requests.Has(app::PanelWork::Inspect) && !inspections && workspace.Selected()->phase == "begun",
				"A begun frame was mistaken for submitted CPU metadata");
		auto output = std::make_shared<PendingOutput>();
		output->slot = fixture.session->Captures().ProducerReservation(
			{fixture.session->Snapshot().generation, prediction.id});
		Require(fixture.pool.Consume(output->slot), "Consume pending inspection output reservation");
		auto metadata = std::make_shared<const simxr::FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
			std::vector<simxr::LayerDescription>{simxr::QuadLayerDescription{}});
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, prediction.displayTime.value.count(),
			XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, nullptr};
		Require(fixture.session->EndFrame(prediction, &end, std::move(metadata), output) == XR_SUCCESS,
				"Submit pending CPU inspection frame");
		refresh();
		Eventually([&] { poll(); return workspace.Selected()->document.Find(tag::Document) != nullptr; },
			"Submitted capture did not automatically load CPU details");
		Require(inspections == 1 && workspace.Selected()->phase == "gpu_pending" && !output->complete,
				"CPU details waited for GPU completion or were requested more than once");
		workspace.InspectSelected();
		workspace.InspectSelected();
		Eventually([&] { poll(); return !requests.Has(app::PanelWork::Inspect); },
			"Manual CPU refresh did not finish");
		Require(inspections == 2, "Repeated refresh queued duplicate CPU inspections");
		workspace.ReleaseSelected();
		output->complete = true;
		fixture.session->Captures().Poll();
		std::cout << "PASS deferred CPU inspection follows submission before GPU readiness and deduplicates refresh\n";
	}
	for (const bool malformed : {false, true})
	{
		debugger.Command(wire::CommandKind::Pause);
		bool rejected = false;
		Eventually([&]
		{
			requests.FlushReleases();
			debugger.Poll();
			for (auto& reply : requests.Poll())
			{
				if (reply.task.kind != app::PanelWork::Acquire)
				{
					debugger.Handle(reply.task, reply.response.packet.message);
					continue;
				}
				const auto lease = app::panel::U64(reply.response.packet.message, tag::Lease);
				Require(lease != 0, "Acquire did not return a test lease");
				requests.Release(wire::OpCode::Relinquish, tag::Lease, lease,
					reply.task.session, reply.task.generation);
				if (malformed)
				{
					try
					{
						debugger.Handle(reply.task, Message({}));
					}
					catch (const std::runtime_error&)
					{
						rejected = true;
					}
				}
				else
				{
					debugger.Failed(reply.task, wire::ErrorCode::Busy, "Another client owns debugging");
					rejected = true;
				}
			}
			return rejected;
		}, "Debugger did not consume the rejected lease reply");
		Eventually([&]
		{
			pump();
			return !debugger.Relinquishing();
		}, "Test lease was not relinquished");
		debugger.Poll();
		Require(!debugger.Pending() && !requests.Has(app::PanelWork::Acquire) && !debugger.HasLease(),
			"Rejected lease request kept retrying its deferred command");
	}
	debugger.Command(wire::CommandKind::Pause);
	Eventually(
		[&]
		{
			pump();
			return !debugger.Operations().empty() && debugger.Operations().back().phase == "complete";
		},
		"Debugger owner did not settle actual core pause");
	Require(debugger.HasLease() && debugger.Operations().size() == 1 && debugger.Operations().front().command == 1,
			"Lease acquisition duplicated or changed command admission");
	const auto pausedCaptures = debugger.TakeCaptures();
	Require(!pausedCaptures.empty() && std::all_of(pausedCaptures.begin(), pausedCaptures.end(),
		[](const auto& capture)
		{
			return capture.kind == wire::CommandKind::Pause;
		}), "Pause capture lost the operation that produced it");
	// Strict admission remains pending because the application makes no further
	// WaitFrame call. Replace one already-polled reply with malformed metadata.
	debugger.Command(wire::CommandKind::StrictAdmission);
	std::uint64_t rejectedTicket = 0, recoveredTicket = 0;
	Eventually(
		[&]
		{
			debugger.Poll();
			for (auto& reply : requests.Poll())
			{
				Require(reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error) == 0,
						"Strict-admission status failed");
				if (!rejectedTicket && reply.task.kind == app::PanelWork::Operation &&
					debugger.Operations().back().kind == wire::CommandKind::StrictAdmission &&
					reply.task.id == debugger.Operations().back().id)
				{
					const auto previous = debugger.Operations().back();
					Require(previous.polling, "Operation exchange was not admitted before its reply");
					bool rejected = false;
					try
					{
						debugger.Handle(reply.task, Message({wire::Field::Text(tag::Phase, "invalid-phase")}));
					}
					catch (const std::runtime_error&)
					{
						rejected = true;
					}
					const auto& preserved = debugger.Operations().back();
					Require(rejected && !preserved.polling && preserved.phase == previous.phase &&
								preserved.progress == previous.progress && preserved.id == previous.id &&
								preserved.command == previous.command,
							"Malformed reply stranded polling or partially replaced operation data");
					rejectedTicket = reply.response.ticket;
				}
				else
					debugger.Handle(reply.task, reply.response.packet.message);
			}
			return rejectedTicket != 0;
		},
		"Strict admission did not produce an operation reply");
	Eventually(
		[&]
		{
			debugger.Poll();
			for (auto& reply : requests.Poll())
			{
				Require(reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error) == 0,
						"Recovery poll failed");
				if (reply.task.kind == app::PanelWork::Operation && reply.task.id == debugger.Operations().back().id)
					recoveredTicket = reply.response.ticket;
				debugger.Handle(reply.task, reply.response.packet.message);
			}
			return recoveredTicket > rejectedTicket && debugger.Operations().back().phase == "pending";
		},
		"Malformed operation metadata prevented a fresh healthy poll");
	debugger.CancelPending();
	Eventually(
		[&]
		{
			pump();
			return debugger.Operations().size() == 3 && debugger.Operations().back().phase == "complete";
		},
		"Cancel did not recover the pending strict admission");
	debugger.Command(wire::CommandKind::Resume);
	Eventually(
		[&]
		{
			pump();
			return debugger.Operations().size() == 4 && debugger.Operations().back().phase == "complete";
		},
		"Debugger owner did not resume the actual session");
	Require(debugger.Operations().back().command == 4, "Next command lost monotonic identity");
	app::ImageExportTask exporter(requests);
	std::uint64_t reader = 0;
	auto retain = debugger.SessionFields();
	retain.push_back(wire::Field::U64(tag::CaptureId, fixture.capture.value, true));
	Require(requests.Send(wire::OpCode::RetainImage, app::PanelWork::Retain, std::move(retain)) != 0,
			"Admit exact source reader for export");
	Eventually(
		[&]
		{
			for (auto& reply : requests.Poll())
			{
				Require(reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error) == 0,
						"Retain source reader failed");
				if (reply.task.kind == app::PanelWork::Retain)
					reader = app::panel::U64(reply.response.packet.message, tag::ImageReaderId);
				else
					debugger.Handle(reply.task, reply.response.packet.message);
			}
			return reader != 0;
		},
		"Exact source reader was not retained");
	fixture.state->exportPhase = simxr::CompletionStatus::Complete;
	const auto destination = directory / "endpoint.rgba";
	Require(exporter.Request(reader, destination.string()), "Admit explicit export exchange");
	Require(!exporter.Request(reader, destination.string()), "A second export bypassed single-job admission");
	Eventually(
		[&]
		{
			requests.FlushReleases();
			for (auto& reply : requests.Poll())
			{
				Require(reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error) == 0,
						"Explicit export exchange failed");
				if (!exporter.Handle(reply.task, reply.response.packet.message))
					debugger.Handle(reply.task, reply.response.packet.message);
			}
			exporter.Poll();
			return !exporter.Busy() && exporter.Succeeded();
		},
		"Export owner did not finish exact endpoint bytes");
	Require(Read(destination) == wire::test::Pixels() && fixture.state->reads == 1 && fixture.state->shares == 0,
			"Explicit export substituted bytes or entered the GPU sharing path");
	Require(!debugger.Status().fields.empty(), "Reconnect status-reset regression requires an observed status");
	requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
	Require(requests.Has(app::PanelWork::Release) && requests.Pending() && !debugger.Relinquishing(),
			"Queued image release was mistaken for a debugger lease release");
	requests.FlushReleases();
	Require(requests.Has(app::PanelWork::Release) && requests.Pending() && !debugger.Relinquishing(),
			"In-flight image release was mistaken for a debugger lease release");
	requests.Send(wire::OpCode::Sessions, app::PanelWork::Sessions);
	const auto retiredSession = debugger.Selected();
	while (requests.Send(wire::OpCode::Sessions, app::PanelWork::Sessions))
	{
	}
	debugger.ChooseSession({});
	Require(!debugger.HasLease() && debugger.Relinquishing() && requests.Pending(),
			"Full transport queue lost the deferred debugger release");
	bool relinquished = false, imageReleased = false;
	Eventually(
		[&]
		{
			requests.FlushReleases();
			for (const auto& reply : requests.Poll())
				if (reply.response.packet.message.opcode == static_cast<std::uint16_t>(wire::OpCode::Relinquish))
				{
					Require(reply.task.kind == app::PanelWork::Relinquish && reply.task.session == retiredSession.id &&
							reply.task.generation == retiredSession.generation &&
							reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error, 1) == 0,
							"Deferred release lost the original session or failed to release its lease");
					relinquished = true;
				}
				else if (reply.response.packet.message.opcode == static_cast<std::uint16_t>(wire::OpCode::ReleaseImage))
				{
					Require(reply.task.kind == app::PanelWork::Release && reply.response.error.empty() &&
							app::panel::U32(reply.response.packet.message, tag::Error, 1) == 0,
							"Image release lost its resource-only work category or failed");
					imageReleased = true;
				}
			return relinquished && imageReleased && !debugger.Relinquishing();
		},
		"Debugger release did not recover after request capacity became available");
	debugger.ChooseSession(retiredSession);
	while (requests.Send(wire::OpCode::Sessions, app::PanelWork::Sessions))
	{
	}
	debugger.Command(wire::CommandKind::Pause);
	Eventually(
		[&]
		{
			pump();
			return debugger.HasLease() && !debugger.Operations().empty() &&
				debugger.Operations().back().phase == "complete";
		},
		"Deferred command did not retry lease acquisition after queue pressure");
	debugger.TakeCaptures();
	debugger.Command(wire::CommandKind::Step);
	Eventually(
		[&]
		{
			pump();
			return debugger.Operations().back().kind == wire::CommandKind::Step &&
				(debugger.Operations().back().phase == "pending" || debugger.Operations().back().phase == "complete");
		},
		"Debugger step did not admit a fixture frame");
	if (debugger.Operations().back().phase == "pending")
		fixture.Submit();
	Eventually(
		[&]
		{
			pump();
			return debugger.Operations().back().phase == "complete";
		},
		"Debugger step did not settle the submitted fixture frame");
	const auto steppedCaptures = debugger.TakeCaptures();
	Require(!steppedCaptures.empty() && std::all_of(steppedCaptures.begin(), steppedCaptures.end(),
		[](const auto& capture)
		{
			return capture.kind == wire::CommandKind::Step && capture.frame != 0;
		}), "Step capture lost its command attribution or completed frame identity");
	requests.Connect(server.Path());
	Eventually(
		[&]
		{
			requests.RefreshEpoch();
			return requests.Epoch() != firstEpoch &&
				   requests.Snapshot().state == app::RuntimeConnectionState::Connected;
		},
		"Reconnect did not replace transport epoch");
	debugger.ResetEpoch();
	Require(!requests.Pending() && !debugger.Selected().id && !debugger.HasLease() && debugger.Operations().empty() &&
				debugger.Status().fields.empty(),
			"Reconnect preserved old request identities or debugger lease");
	requests.Close();
}
#endif
} // namespace
int main()
{
	try
	{
		TimelineRecordingIdentity();
		TimelineScriptReconnect();
		TimelineCompletedScripts();
		TimelineImmediateReplayCompletion();
		TimelinePauseEndpoints();
		TimelineSavedRecordingScope();
		SessionRunningUpdates();
		MissingCaptureKeepsSession();
		ScopedInspectionReaders();
		FileWorkerAndChunkValidation();
#if defined(__linux__)
		ActualConnectionAndDebugger();
#endif
		std::cout << "Runtime panel owner tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
