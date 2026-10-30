#include "FakeImageInspection.h"
#include "control/ConnectionImages.h"
#include "graphics/SnapshotBudget.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace
{
using namespace simxr;
namespace wire = simxr::control;
namespace tag = wire::tag;

wire::ErrorCode Error(const wire::ControlPacket& packet)
{
	const auto* field = packet.message.Find(tag::Error);
	Require(field && field->AsU32(), "Missing live-preview response error");
	return static_cast<wire::ErrorCode>(*field->AsU32());
}

std::uint64_t Id(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	Require(field && field->AsU64(), "Missing live-preview uint64 metadata");
	return *field->AsU64();
}

wire::Message Nested(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	Require(field != nullptr, "Missing live-preview nested metadata");
	auto decoded = wire::DecodeFields(field->value);
	Require(decoded.error == wire::DecodeError::None, "Malformed live-preview nested metadata");
	wire::Message result;
	result.fields = std::move(decoded.fields);
	return result;
}

struct Client
{
	wire::ControlService& service;
	wire::ConnectionId connection;
	std::uint16_t minor;
	std::uint64_t next = 1, session = 0, generation = 0;
	wire::ControlPacket hello;

	Client(wire::ControlService& service, std::uint16_t minor = wire::ProtocolMinor)
		: service(service), connection(*service.Connect(true)), minor(minor)
	{
		hello = Call(wire::OpCode::Hello);
		Require(Error(hello) == wire::ErrorCode::Ok, "Negotiate live-preview connection");
		const auto discovered = Call(wire::OpCode::Sessions);
		const auto list = Nested(discovered.message, tag::Sessions);
		Require(list.fields.size() == 1, "Discover live-preview fixture session");
		const auto entry = Nested(list, list.fields.front().tag);
		session = Id(entry, tag::Session);
		generation = Id(entry, tag::Generation);
	}

	wire::ControlPacket Call(wire::OpCode opcode, std::vector<wire::Field> fields = {})
	{
		wire::ControlPacket request;
		request.message.minor = minor;
		request.message.opcode = static_cast<std::uint16_t>(opcode);
		request.message.requestId = {next++};
		request.message.fields = std::move(fields);
		return service.HandlePacket(connection, request);
	}

	std::vector<wire::Field> Common() const
	{
		return {wire::Field::U64(tag::Session, session), wire::Field::U64(tag::Generation, generation)};
	}

	wire::ControlPacket Preview(std::vector<wire::Field> extra = {})
	{
		auto fields = Common();
		fields.insert(fields.end(), extra.begin(), extra.end());
		return Call(wire::OpCode::RetainPreview, std::move(fields));
	}
};

class PendingOutput final : public FrameOutput
{
public:
	explicit PendingOutput(std::shared_ptr<FrameReservation> slot) : slot(std::move(slot)) {}

	CompletionStatus PollCompletion() const noexcept override
	{
		return status.load();
	}

	std::shared_ptr<FrameReservation> slot;
	std::atomic<CompletionStatus> status{CompletionStatus::Pending};
};

FramePrediction Submit(wire::test::Fixture& fixture, const std::shared_ptr<const FrameOutput>& output)
{
	FramePrediction prediction;
	Require((!fixture.session->Scheduler().Snapshot().paused ||
			 fixture.session->Scheduler().GrantPermits() == XR_SUCCESS) &&
				fixture.session->WaitFrame(prediction) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS,
			"Issue CPU live-preview frame without capture reservation");
	if (!output)
	{
		Require(fixture.session->EndFrame(prediction) == XR_SUCCESS, "Submit actual clear frame");
		return prediction;
	}
	auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														std::vector<LayerDescription>{QuadLayerDescription{}});
	const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
							 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
	Require(fixture.session->EndFrame(prediction, &end, std::move(metadata), output) == XR_SUCCESS,
			"Submit CPU live-preview image frame");
	return prediction;
}

std::shared_ptr<PendingOutput> NewOutput(wire::test::Fixture& fixture, CompletionStatus status)
{
	auto slot = fixture.pool.Reserve();
	Require(slot && fixture.pool.Consume(slot), "Reserve CPU live output budget");
	auto output = std::make_shared<PendingOutput>(std::move(slot));
	output->status = status;
	return output;
}

void Through(wire::test::Fixture& fixture, DisplayTick tick)
{
	if (fixture.session->Display().NextTick() > tick)
		tick = fixture.session->Display().NextTick();
	while (fixture.session->Display().NextTick() <= tick)
		Require(fixture.session->Display().Step().has_value(), "Advance actual selected virtual display");
}

void CheckIdentity(const wire::ControlPacket& packet, const Client& client, FrameKey frame)
{
	Require(Error(packet) == wire::ErrorCode::Ok && packet.message.Find(tag::ImageAvailable)->AsBool() == true &&
				Id(packet.message, tag::Session) == client.session &&
				Id(packet.message, tag::Generation) == frame.generation.value &&
				Id(packet.message, tag::CaptureId) == 0 &&
				Id(Nested(packet.message, tag::Frame), tag::FrameId) == frame.frame.value,
			"Live reader substituted capture/session/generation/frame identity");
	const auto target = Nested(packet.message, tag::Target);
	Require(target.Find(tag::CaptureKind)->AsU32() == static_cast<std::uint32_t>(wire::TargetKind::VirtualDisplay) &&
				target.Find(tag::Layer)->AsU32() == 0 && target.Find(tag::View)->AsU32() == 0,
			"Live reader lost virtual-display target identity");
}

void DistinctDisplayImagesFromOneFrame()
{
	wire::ConnectionImages images;
	auto state = std::make_shared<wire::test::State>();
	wire::test::Provider provider(state);
	const wire::ImageSource source{{7}, {{3}, {21}}, {}, {CaptureKind::VirtualDisplay}};
	auto retain = [&](const std::shared_ptr<const FrameOutput>& output, std::uint64_t imageTick) {
		wire::ConnectionImages::RetiredResource retired;
		wire::ConnectionImages::Publication publication(images, retired);
		wire::Message response;
		auto version = source;
		version.imageTick = DisplayTick{imageTick};
		images.Retain(response, version, output, provider, false, publication);
		publication.Commit();
		Require(Id(response, tag::ImageTick) == imageTick && Id(response, tag::Session) == source.session.value &&
					Id(response, tag::CaptureId) == 0 &&
					Id(Nested(response, tag::Frame), tag::FrameId) == source.frame.frame.value &&
					Id(Nested(response, tag::Frame), tag::Generation) == source.frame.generation.value,
				"Changed display image lost its original submitted-frame identity");
		return Id(response, tag::ImageReaderId);
	};
	auto release = [&](std::uint64_t reader) {
		wire::ConnectionImages::RetiredResource retired;
		wire::ConnectionImages::Publication publication(images, retired);
		wire::ControlPacket request, outgoing;
		request.message.opcode = static_cast<std::uint16_t>(wire::OpCode::ReleaseImage);
		request.message.fields.push_back(wire::Field::U64(tag::ImageReaderId, reader));
		const auto response = images.Dispatch(request, outgoing, wire::ProtocolMinor, false, publication, retired);
		publication.Commit();
		Require(response.Find(tag::Released)->AsBool() == true, "Release independently retained display image");
	};
	auto original = std::make_shared<wire::test::Output>(nullptr);
	auto rotated = std::make_shared<wire::test::Output>(nullptr);
	std::weak_ptr<const FrameOutput> originalLifetime = original, rotatedLifetime = rotated;
	const auto originalReader = retain(original, 5), rotatedReader = retain(rotated, 6);
	Require(originalReader != rotatedReader && state->retains == 2,
			"New display image reused the original frame's stale reader");
	Require(retain(rotated, 6) == rotatedReader && retain(original, 5) == originalReader && state->retains == 2,
			"Repeating an unchanged display image allocated another native reader");

	state->failReaderDescription = true;
	try
	{
		retain(std::make_shared<wire::test::Output>(nullptr), 20);
		throw std::runtime_error("Failed display-image publication was accepted");
	}
	catch (const std::bad_alloc&)
	{}
	Require(state->readersDestroyed == 1 && retain(rotated, 6) == rotatedReader,
			"Publication rollback replaced an existing image or retained its failed reader");
	for (std::size_t index = 2; index < SnapshotBudget::Capacity(); ++index)
		retain(std::make_shared<wire::test::Output>(nullptr), 100 + index);
	Require(retain(rotated, 6) == rotatedReader, "Full table rejected an existing display image");
	const auto retained = state->retains.load();
	try
	{
		retain(std::make_shared<wire::test::Output>(nullptr), 20);
		throw std::runtime_error("Display images from one frame bypassed the reader budget");
	}
	catch (const wire::ConnectionImages::RequestFailure& failure)
	{
		Require(failure.error == wire::ErrorCode::Capacity && state->retains == retained,
				"Display-image capacity was checked after native retention");
	}
	original.reset();
	rotated.reset();
	Require(!originalLifetime.expired() && !rotatedLifetime.expired(), "Readers lost their independent image owners");
	release(originalReader);
	Require(originalLifetime.expired() && !rotatedLifetime.expired(), "Image cache extended or confused GPU ownership");
	retain(std::make_shared<wire::test::Output>(nullptr), 20);
	release(rotatedReader);
	Require(rotatedLifetime.expired(), "Releasing the second image retained its output");
}

void ReaderReleasePreservesImageTick()
{
	wire::test::Fixture fixture;
	Client client(fixture.service);
	Through(fixture, fixture.session->Captures().Read(fixture.capture)->metadata->prediction.displayTick);
	const auto first = client.Preview();
	const auto firstReader = Id(first.message, tag::ImageReaderId), imageTick = Id(first.message, tag::ImageTick);
	client.Call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, firstReader)});
	Require(fixture.session->Display().Step().has_value(), "Repeat before reacquiring the selected image");
	const auto reacquired = client.Preview();
	const auto reader = Id(reacquired.message, tag::ImageReaderId);
	Require(reader != firstReader && Id(reacquired.message, tag::ImageTick) == imageTick &&
				Id(Nested(reacquired.message, tag::Display), tag::Tick) > imageTick,
			"Releasing a reader changed the selected image identity");
	const auto next = Submit(fixture, NewOutput(fixture, CompletionStatus::Complete));
	Through(fixture, next.displayTick);
	const auto changed = client.Preview();
	Require(Id(changed.message, tag::ImageTick) > imageTick, "Selecting a new image kept the old image tick");
	const auto previous = client.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, reader)});
	Require(Error(previous) == wire::ErrorCode::Ok && Id(previous.message, tag::ImageTick) == imageTick,
			"A new display selection changed a retained reader's image identity");
	const auto clear = Submit(fixture, {});
	Through(fixture, clear.displayTick);
	const auto cleared = client.Preview();
	Require(!cleared.message.Find(tag::ImageTick) && !Nested(cleared.message, tag::Display).Find(tag::ImageTick),
			"A selected clear retained an image identity");
}

void LongPollWaitsForNextImage()
{
	using namespace std::chrono;
	wire::test::Fixture fixture;
	Client client(fixture.service);
	Through(fixture, fixture.session->Captures().Read(fixture.capture)->metadata->prediction.displayTick);
	const auto release = [&](const wire::ControlPacket& preview) {
		client.Call(wire::OpCode::ReleaseImage,
					{wire::Field::U64(tag::ImageReaderId, Id(preview.message, tag::ImageReaderId))});
	};
	const auto timed = [&](std::uint64_t after) {
		const auto started = steady_clock::now();
		auto response = client.Preview({wire::Field::U64(tag::ImageTick, after)});
		return std::pair{std::move(response), steady_clock::now() - started};
	};
	const auto first = client.Preview();
	const auto imageTick = Id(first.message, tag::ImageTick);
	release(first);

	const auto [stale, staleWait] = timed(0);
	Require(Error(stale) == wire::ErrorCode::Ok && Id(stale.message, tag::ImageTick) == imageTick &&
				staleWait < milliseconds(50),
			"A consumer without the selected image waited for another");
	release(stale);
	const auto [unchanged, unchangedWait] = timed(imageTick);
	Require(Error(unchanged) == wire::ErrorCode::Ok && Id(unchanged.message, tag::ImageTick) == imageTick &&
				unchangedWait >= milliseconds(90),
			"Long poll returned before its bound without a new image");
	release(unchanged);

	// A selection made during the wait wakes it.
	const auto output = NewOutput(fixture, CompletionStatus::Pending);
	std::thread producer([&] {
		std::this_thread::sleep_for(milliseconds(10));
		const auto submitted = Submit(fixture, output);
		std::this_thread::sleep_for(milliseconds(10));
		output->status = CompletionStatus::Complete;
		Through(fixture, submitted.displayTick);
	});
	const auto [woken, wokenWait] = timed(imageTick);
	producer.join();
	Require(Error(woken) == wire::ErrorCode::Ok && Id(woken.message, tag::ImageTick) > imageTick &&
				wokenWait < milliseconds(90),
			"Long poll did not return the newly selected GPU-ready image");
	release(woken);
}

void SelectionLifetimeAndNoAdmission()
{
	wire::test::Fixture fixture;
	Client old(fixture.service, 2), client(fixture.service), other(fixture.service);
	const auto bit = static_cast<std::uint64_t>(wire::Capability::LivePreview);
	Require(!(Id(old.hello.message, tag::Capabilities) & bit) &&
				!(Id(old.Call(wire::OpCode::Status, old.Common()).message, tag::Capabilities) & bit) &&
				Error(old.Preview()) == wire::ErrorCode::UnsupportedCommand,
			"Minor2 exposed live preview command/capability");
	Require((Id(client.hello.message, tag::Capabilities) & bit) &&
				(Id(client.Call(wire::OpCode::Status, client.Common()).message, tag::Capabilities) & bit),
			"Minor3 omitted actual provider live-preview capability");
	auto initialCapture = fixture.session->Captures().Read(fixture.capture);
	const auto key = *initialCapture->frame;
	Require(!fixture.session->Display().Snapshot().latest && Error(client.Preview()) == wire::ErrorCode::Busy &&
				fixture.state->retains == 0,
			"Preview invented an initial selected image");
	const auto pipeline = fixture.session->Scheduler().Snapshot();
	const auto snapshots = fixture.session->Captures().Snapshot();
	const auto budget = fixture.pool.InUse();
	Through(fixture, initialCapture->metadata->prediction.displayTick);
	auto selected = client.Preview();
	CheckIdentity(selected, client, key);
	const auto reader = Id(selected.message, tag::ImageReaderId);
	Require(Id(client.Preview().message, tag::ImageReaderId) == reader && fixture.state->retains == 1,
			"Repeated selected source created another reader");
	fixture.session->Display().Step();
	const auto repeated = client.Preview();
	Require(Id(repeated.message, tag::ImageReaderId) == reader &&
				Nested(repeated.message, tag::Display).Find(tag::Kind)->AsText() == "repeat" &&
				Id(repeated.message, tag::ImageTick) == Id(selected.message, tag::ImageTick) &&
				Id(Nested(repeated.message, tag::Display), tag::ImageTick) == Id(selected.message, tag::ImageTick) &&
				Id(Nested(repeated.message, tag::Display), tag::Tick) > Id(selected.message, tag::ImageTick) &&
				fixture.state->retains == 1,
			"Repeated display tick did not reuse selected source reader");
	const auto after = fixture.session->Scheduler().Snapshot();
	const auto retained = fixture.session->Captures().Snapshot();
	Require(after.permits == pipeline.permits && after.nextAdmission == pipeline.nextAdmission && !after.waited &&
				!after.begun && retained.captures == snapshots.captures && retained.heldSlots == snapshots.heldSlots &&
				fixture.pool.InUse() == budget,
			"RetainPreview admitted XR work or allocated snapshot/GPU reservation capacity");
	Require(Error(other.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, reader)})) ==
				wire::ErrorCode::NotFound,
			"Live reader escaped connection ownership");
	initialCapture.reset();
	Require(fixture.session->Captures().Release(fixture.capture), "Release original exact capture");
	Require(Error(client.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, reader)})) ==
				wire::ErrorCode::Ok,
			"Capture release invalidated independent live image reader");
	Require(fixture.runtime.DestroySession(fixture.session) == XR_SUCCESS, "Destroy source session");
	const auto info = client.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, reader)});
	Require(Error(info) == wire::ErrorCode::Ok && Id(info.message, tag::Session) == client.session &&
				Id(Nested(info.message, tag::Frame), tag::FrameId) == key.frame.value &&
				Id(info.message, tag::CaptureId) == 0,
			"Live reader did not preserve original metadata after session teardown");
	Require(fixture.pool.InUse() == 1, "Source image budget did not follow its independent live reader");
	Require(Error(client.Preview()) == wire::ErrorCode::NotFound && fixture.state->reads == 0,
			"Destroyed session retained a new selection or preview implicitly read pixels");
	Require(Error(client.Call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, reader)})) ==
				wire::ErrorCode::Ok,
			"Release live reader after source session destruction");
	Require(fixture.pool.InUse() == 0, "Final live reader leaked the retired source image budget");
}

void CompleteSelectionAndClear()
{
	wire::test::Fixture fixture;
	Client client(fixture.service);
	Through(fixture, fixture.session->Captures().Read(fixture.capture)->metadata->prediction.displayTick);
	const auto original = client.Preview();
	const auto originalReader = Id(original.message, tag::ImageReaderId);
	const auto oldKey = *fixture.session->Captures().Read(fixture.capture)->frame;
	auto pending = NewOutput(fixture, CompletionStatus::Pending);
	const auto frame = Submit(fixture, pending);
	Through(fixture, frame.displayTick);
	auto repeated = client.Preview();
	CheckIdentity(repeated, client, oldKey);
	Require(Id(repeated.message, tag::ImageReaderId) == originalReader && fixture.state->retains == 1,
			"Preview selected a submitted but incomplete newer image");
	pending->status = CompletionStatus::Complete;
	fixture.session->Display().Step();
	const auto complete = client.Preview();
	CheckIdentity(complete, client, {oldKey.generation, frame.id});
	Require(Id(complete.message, tag::ImageReaderId) != originalReader && fixture.state->retains == 2,
			"Completed newer selected source reused old reader identity");
	const auto clearFrame = Submit(fixture, {});
	Through(fixture, clearFrame.displayTick);
	const auto cleared = client.Preview();
	Require(Error(cleared) == wire::ErrorCode::Ok && cleared.message.Find(tag::ImageAvailable)->AsBool() == false &&
				!cleared.message.Find(tag::ImageReaderId) && !cleared.message.Find(tag::ImageDescriptor) &&
				Nested(cleared.message, tag::Display).Find(tag::Kind)->AsText() == "clear" &&
				fixture.state->retains == 2,
			"Actual selected clear retained/fabricated a GPU reader");
	Require(Error(client.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, originalReader)})) ==
				wire::ErrorCode::Ok,
			"Selected clear mutated old immutable live reader");
	Require(fixture.state->reads == 0, "Live selection/clear performed implicit readback");
}

void SourceAge()
{
	wire::test::Fixture fixture(ExecutionMode::RealTime);
	Client client(fixture.service);
	const auto resume = [&] {
		Require(fixture.session->Scheduler().Resume(std::chrono::steady_clock::now(),
													fixture.session->Scheduler().Snapshot().nextAdmission) ==
					XR_SUCCESS,
				"Resume age fixture");
	};
	resume();
	auto prediction = Submit(fixture, NewOutput(fixture, CompletionStatus::Complete));
	Through(fixture, prediction.displayTick);
	const auto first = client.Preview().message.Find(tag::SourceAge)->AsI64();
	Require(first && *first >= 0, "Live prediction omitted mapped source age");
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	fixture.session->Display().Step();
	const auto repeated = client.Preview().message.Find(tag::SourceAge)->AsI64();
	Require(repeated && *repeated >= *first + 30000000, "Display repeat reset original game-frame age");
	fixture.session->Scheduler().Pause();
	Require(client.Preview().message.Find(tag::SourceAge)->AsI64() == -1, "Paused frame retained a live age");
	resume();
	Require(client.Preview().message.Find(tag::SourceAge)->AsI64() == -1, "Clock rebase relabelled an old prediction");
	prediction = Submit(fixture, NewOutput(fixture, CompletionStatus::Complete));
	Through(fixture, prediction.displayTick);
	const auto fresh = client.Preview().message.Find(tag::SourceAge)->AsI64();
	Require(fresh && *fresh >= 0, "New frame did not recover after clock rebase");
}

void ValidationAndRollback()
{
	wire::test::Fixture fixture;
	Client client(fixture.service);
	auto stale = client.Common();
	stale.back() = wire::Field::U64(tag::Generation, client.generation + 1);
	Require(Error(client.Call(wire::OpCode::RetainPreview, std::move(stale))) == wire::ErrorCode::StaleGeneration,
			"Stale live-preview generation accepted");
	Require(Error(client.Preview({wire::Field::U32(65000, 1, true)})) == wire::ErrorCode::UnsupportedField &&
				Error(client.Preview({wire::Field::U32(65000, 1)})) == wire::ErrorCode::Busy &&
				fixture.state->retains == 0,
			"Unknown critical live field admitted work or optional field was rejected");
	Through(fixture, fixture.session->Captures().Read(fixture.capture)->metadata->prediction.displayTick);
	fixture.state->failReaderDescription = true;
	const auto rejected = client.Preview();
	Require(Error(rejected) == wire::ErrorCode::Capacity && !rejected.message.Find(tag::ImageReaderId) &&
				fixture.state->readersDestroyed == 1 && fixture.state->retains == 1,
			"Live reader metadata allocation failure published an unannounced resource");
	const auto retried = client.Preview();
	Require(Error(retried) == wire::ErrorCode::Ok && fixture.state->retains == 2,
			"Failed live-reader publication poisoned selected-source retry");
	std::vector<std::uint64_t> readers{Id(retried.message, tag::ImageReaderId)};
	for (std::size_t index = 1; index < SnapshotBudget::Capacity(); ++index)
	{
		const auto frame = Submit(fixture, NewOutput(fixture, CompletionStatus::Complete));
		Through(fixture, frame.displayTick);
		const auto response = client.Preview();
		Require(Error(response) == wire::ErrorCode::Ok, "Fill bounded live image-reader table");
		readers.push_back(Id(response.message, tag::ImageReaderId));
	}
	Require(Id(client.Preview().message, tag::ImageReaderId) == readers.back(),
			"Full table rejected already retained selection");
	const auto next = Submit(fixture, NewOutput(fixture, CompletionStatus::Complete));
	Through(fixture, next.displayTick);
	Require(Error(client.Preview()) == wire::ErrorCode::Capacity &&
				fixture.state->retains == SnapshotBudget::Capacity() + 1,
			"Live reader table capacity was checked after native provider retention");
	client.Call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, readers.front())});
	Require(Error(client.Preview()) == wire::ErrorCode::Ok && fixture.state->retains == SnapshotBudget::Capacity() + 2,
			"Released live image-reader capacity did not recover");
	Require(fixture.session->Captures().Snapshot().captures == std::vector<CaptureId>{fixture.capture} &&
				fixture.state->reads == 0,
			"Bounded live preview allocated captures or implicitly read pixels");
}
} // namespace

int main()
{
	try
	{
		DistinctDisplayImagesFromOneFrame();
		ReaderReleasePreservesImageTick();
		LongPollWaitsForNextImage();
		SelectionLifetimeAndNoAdmission();
		CompleteSelectionAndClear();
		SourceAge();
		ValidationAndRollback();
		std::cout << "PASS live-preview selected-source CPU service contracts; no GPU qualification\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
