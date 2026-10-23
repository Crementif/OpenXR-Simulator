#include "../TestSupport.h"
#include "app/DebuggerPanelController.h"
#include "app/ImageExportTask.h"
#include "app/InspectionWorkspace.h"
#include "graphics/SnapshotBudget.h"
#include "app/Utf8Path.h"

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
	workspace.Handle({app::PanelWork::Revision, 31}, Message({wire::Field::Text(tag::Detail, "original revision")}));
	workspace.SetScope(restarted);
	Require(workspace.Add(7, 202) && workspace.Select(7), "Reuse remote capture ID in a new generation");
	Require(workspace.Selected()->scope == restarted && workspace.Selected()->frame == 202,
			"Selection substituted an older generation");
	workspace.Handle({app::PanelWork::Capture, 7}, Message({wire::Field::Text(tag::Phase, "clear"), Frame(203)}));
	workspace.SelectLocal(originalLocal);
	Require(workspace.Selected()->frame == 101 && workspace.Selected()->scope == original &&
				app::panel::Text(workspace.Selected()->document, tag::Detail) == "original immutable document",
			"A later capture response mutated the original CPU document");
	workspace.SetScope(reconnect);
	Require(!workspace.Revision(31), "A revision from another endpoint epoch survived as a live lookup");
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
	// Another session in this endpoint can still release its connection-owned
	// inspection, but its capture must only be retired locally.
	bounded.Select(101);
	const auto anotherSessionLocal = bounded.Selected()->local;
	bounded.SetScope({reconnect.epoch, 99, 1});
	bounded.SelectLocal(anotherSessionLocal);
	bounded.ReleaseSelected();
	Require(bounded.Selected()->released && bounded.Add(133, 133) && bounded.Captures().size() == capacity,
			"Historical selection from another session remained non-recyclable");
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
	debugger.TakeCaptures();
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
	// Exercise the new export owner through its actual asynchronous transport,
	// not by treating the CPU fixture's memfd as a native GPU image.
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
			return !exporter.Busy() && exporter.Detail().starts_with("Saved exact RGBA bytes");
		},
		"Export owner did not finish exact endpoint bytes");
	Require(Read(destination) == wire::test::Pixels() && fixture.state->reads == 1 && fixture.state->shares == 0,
			"Explicit export substituted bytes or entered the GPU sharing path");
	Require(!debugger.Status().fields.empty(), "Reconnect status-reset regression requires an observed status");
	requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
	requests.Send(wire::OpCode::Sessions, app::PanelWork::Sessions);
	const auto retiredSession = debugger.Selected();
	while (requests.Send(wire::OpCode::Sessions, app::PanelWork::Sessions))
	{
	}
	debugger.ChooseSession({});
	Require(!debugger.HasLease() && debugger.Relinquishing() && requests.Pending(),
			"Full transport queue lost the deferred debugger release");
	bool relinquished = false;
	Eventually(
		[&]
		{
			requests.FlushReleases();
			for (const auto& reply : requests.Poll())
				if (reply.response.packet.message.opcode == static_cast<std::uint16_t>(wire::OpCode::Relinquish))
				{
					Require(reply.task.session == retiredSession.id && reply.task.generation == retiredSession.generation &&
							reply.response.error.empty() && app::panel::U32(reply.response.packet.message, tag::Error, 1) == 0,
							"Deferred release lost the original session or failed to release its lease");
					relinquished = true;
				}
			return relinquished && !debugger.Relinquishing();
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
		SessionRunningUpdates();
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
