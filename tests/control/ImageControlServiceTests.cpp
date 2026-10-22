#include "graphics/SnapshotBudget.h"
#include "FakeImageInspection.h"

#include <array>
#include <iostream>

namespace
{
using namespace simxr;
namespace wire = simxr::control;
namespace tag = wire::tag;
wire::ErrorCode Error(const wire::ControlPacket& packet)
{
	Require(packet.message.Find(tag::Error) && packet.message.Find(tag::Error)->AsU32(), "Missing structured error");
	return static_cast<wire::ErrorCode>(*packet.message.Find(tag::Error)->AsU32());
}
std::uint64_t Id(const wire::ControlPacket& packet, std::uint16_t field)
{
	Require(packet.message.Find(field) && packet.message.Find(field)->AsU64(), "Missing image identifier");
	return *packet.message.Find(field)->AsU64();
}
struct Client
{
	wire::ControlService& service;
	wire::ConnectionId connection;
	std::uint64_t sequence = 1, session = 0, generation = 0;
	std::uint16_t minor = wire::ProtocolMinor;
	Client(wire::ControlService& service, bool native = true, std::uint16_t minor = wire::ProtocolMinor)
		: service(service), connection(*service.Connect(native)), minor(minor)
	{
		Require(Error(Call(wire::OpCode::Hello)) == wire::ErrorCode::Ok, "Image client Hello");
		const auto sessions = Call(wire::OpCode::Sessions);
		auto list = wire::DecodeFields(sessions.message.Find(tag::Sessions)->value);
		Require(list.fields.size() == 1, "Discover CPU image session");
		auto record = wire::DecodeFields(list.fields.front().value);
		for (const auto& field : record.fields)
		{
			if (field.tag == tag::Session)
				session = *field.AsU64();
			if (field.tag == tag::Generation)
				generation = *field.AsU64();
		}
	}
	wire::ControlPacket Request(wire::OpCode op, std::vector<wire::Field> fields = {})
	{
		wire::ControlPacket result;
		result.message.minor = minor;
		result.message.opcode = static_cast<std::uint16_t>(op);
		result.message.requestId = {sequence++};
		result.message.fields = std::move(fields);
		return result;
	}
	wire::ControlPacket Send(const wire::ControlPacket& request)
	{
		return service.HandlePacket(connection, request);
	}
	wire::ControlPacket Call(wire::OpCode op, std::vector<wire::Field> fields = {})
	{
		return Send(Request(op, std::move(fields)));
	}
	wire::ControlPacket Retain(CaptureId capture)
	{
		return Call(wire::OpCode::RetainImage,
					{wire::Field::U64(tag::Session, session), wire::Field::U64(tag::Generation, generation),
					 wire::Field::U64(tag::CaptureId, capture.value)});
	}
	wire::ControlPacket Share(std::uint64_t image)
	{
		auto description = wire::test::Description();
		auto request =
			Request(wire::OpCode::ShareImage, {wire::Field::U64(tag::ImageReaderId, image),
											   wire::Field::Bytes(tag::DeviceUuid, description.identity.device),
											   wire::Field::Bytes(tag::DriverUuid, description.identity.driver),
											   wire::Field::U32(tag::HandleCount, 1)});
		request.handles.push_back(wire::OwnControlFd(::memfd_create("simxr-cpu-consumer-fixture", MFD_CLOEXEC)));
		return request;
	}
};
void ImageLifetimesAndReplay()
{
	wire::test::Fixture fixture;
	Client client(fixture.service), other(fixture.service), legacy(fixture.service, true, 1),
		noFd(fixture.service, false);
	Require(Error(legacy.Retain(fixture.capture)) == wire::ErrorCode::UnsupportedCommand,
			"Old minor accessed image delivery");
	const auto retained = client.Retain(fixture.capture);
	Require(Error(retained) == wire::ErrorCode::Ok && fixture.state->retains == 1, "Retain ready exact image");
	const auto image = Id(retained, tag::ImageReaderId);
	Require(Id(client.Retain(fixture.capture), tag::ImageReaderId) == image && fixture.state->retains == 1,
			"Same source did not reuse immutable reader");
	Require(Error(other.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, image)})) ==
				wire::ErrorCode::NotFound,
			"Image ID escaped connection ownership");
	const auto noFdImage = Id(noFd.Retain(fixture.capture), tag::ImageReaderId);
	Require(Error(noFd.Send(noFd.Share(noFdImage))) == wire::ErrorCode::UnsupportedCommand,
			"Non-FD connection shared native handles");
	auto unknown = client.Request(wire::OpCode::ImageInfo,
								  {wire::Field::U64(tag::ImageReaderId, image), wire::Field::U32(60000, 1, true)});
	Require(Error(client.Send(unknown)) == wire::ErrorCode::UnsupportedField, "Unknown critical image field accepted");
	auto mismatched = client.Share(image);
	mismatched.message.fields.push_back(wire::Field::U32(tag::CompletionKind, 1, true));
	Require(Error(client.Send(mismatched)) == wire::ErrorCode::Invalid && fixture.state->shares == 0,
			"Mismatched completion kind reached native handle import");
	auto request = client.Share(image);
	auto offered = client.Send(request);
	Require(Error(offered) == wire::ErrorCode::Ok && offered.handles.size() == 2 && offered.exposure &&
				offered.encoded && fixture.state->shares == 1 && fixture.state->reads == 0 &&
				fixture.state->exposures == 0,
			"Share read pixels or marked native exposure before transport");
	const auto delivery = Id(offered, tag::DeliveryId);
	Require(Error(client.Send(request)) == wire::ErrorCode::Conflict, "Replay with new native descriptor was accepted");
	request.handles.clear();
	auto replay = client.Send(request);
	Require(Error(replay) == wire::ErrorCode::Ok && Id(replay, tag::DeliveryId) == delivery &&
				replay.handles.size() == 2 && fixture.state->shares == 1,
			"FD-free replay created another consumer");
	offered.exposure->MarkTransferred();
	Require(fixture.state->exposures == 1, "Exposure marker did not reach provider");
	std::array<std::byte, 96> bytes;
	Require(::pread(static_cast<int>(offered.handles[0]->Value()), bytes.data(), bytes.size(), 0) == 96 &&
				std::equal(bytes.begin(), bytes.end(), wire::test::Pixels().begin()),
			"Delivery descriptor did not retain immutable fixture bytes");
	const auto started = client.Call(wire::OpCode::StartExport,
									 {wire::Field::U64(tag::ImageReaderId, image), wire::Field::U32(tag::Layer, 1)});
	Require(Error(started) == wire::ErrorCode::Ok && fixture.state->reads == 0,
			"Export creation read pixels prematurely");
	const auto exportId = Id(started, tag::ExportId);
	auto read = [&](std::uint64_t offset, std::uint64_t length)
	{
		return client.Call(wire::OpCode::ReadExport,
						   {wire::Field::U64(tag::ExportId, exportId), wire::Field::U64(tag::Offset, offset),
							wire::Field::U64(tag::Length, length)});
	};
	Require(Error(read(0, 16)) == wire::ErrorCode::Busy && fixture.state->reads == 0, "Pending export exposed bytes");
	Require(fixture.runtime.DestroySession(fixture.session) == XR_SUCCESS, "Destroy source public session");
	Require(Error(client.Call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, image)})) ==
				wire::ErrorCode::Ok,
			"Release image after public session destruction");
	Require(Error(client.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, image)})) ==
					wire::ErrorCode::NotFound &&
				Error(client.Call(wire::OpCode::DeliveryInfo, {wire::Field::U64(tag::DeliveryId, delivery)})) ==
					wire::ErrorCode::Ok,
			"Independent delivery was tied to released image/source session");
	fixture.state->exportPhase = CompletionStatus::Complete;
	Require(Error(read(0, 0)) == wire::ErrorCode::Invalid && Error(read(49, 1)) == wire::ErrorCode::Invalid &&
				Error(read(0, wire::MaxPayload)) == wire::ErrorCode::Invalid && fixture.state->reads == 0,
			"Invalid byte range reached provider");
	const auto copied = read(5, 100);
	Require(
		Error(copied) == wire::ErrorCode::Ok && Id(copied, tag::Length) == 43 &&
			copied.message.Find(tag::End)->AsBool() == true && fixture.state->reads == 1 &&
			copied.message.Find(tag::Bytes)->value ==
				[]
				{
					const auto pixels = wire::test::Pixels();
					return std::vector<std::byte>(pixels.begin() + 53, pixels.end());
				}(),
		"Explicit layer export chunk changed immutable source bytes");
	const auto eof = read(48, 1);
	Require(Error(eof) == wire::ErrorCode::Ok && Id(eof, tag::Length) == 0 &&
				eof.message.Find(tag::End)->AsBool() == true,
			"End-of-export bounded empty chunk");
	Require(Error(client.Call(wire::OpCode::ReleaseDelivery, {wire::Field::U64(tag::DeliveryId, delivery)})) ==
					wire::ErrorCode::Ok &&
				Error(client.Call(wire::OpCode::ExportInfo, {wire::Field::U64(tag::ExportId, exportId)})) ==
					wire::ErrorCode::Ok,
			"Delivery release invalidated independent export");
	Require(Error(client.Call(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, exportId)})) ==
				wire::ErrorCode::Ok,
			"Release explicit export");
}
void BoundedImageReadersAndReentry()
{
	wire::test::Fixture fixture;
	Client client(fixture.service);
	std::vector<std::uint64_t> readers;
	for (std::size_t index = 0; index < simxr::SnapshotBudget::Capacity(); ++index)
	{
		const auto capture = index ? fixture.Submit() : fixture.capture;
		const auto retained = client.Retain(capture);
		Require(Error(retained) == wire::ErrorCode::Ok, "Fill image reader table");
		readers.push_back(Id(retained, tag::ImageReaderId));
		Require(fixture.session->Captures().Release(capture), "Release logical fixture snapshot");
		fixture.session->Display().Step();
		fixture.session->Display().Step();
	}
	const auto ninth = fixture.Submit();
	Require(Error(client.Retain(ninth)) == wire::ErrorCode::Capacity && fixture.state->retains == simxr::SnapshotBudget::Capacity(),
			"Image reader capacity checked after provider Retain");
	bool reentered = false;
	fixture.state->onReaderDestroyed = [&]
	{
		reentered =
			Error(client.Call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, readers.front())})) ==
			wire::ErrorCode::NotFound;
	};
	Require(Error(client.Call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, readers.front())})) ==
					wire::ErrorCode::Ok &&
				reentered,
			"Last reader destruction retained service mutex or active request gate");
	fixture.state->onReaderDestroyed = {};
	Require(Error(client.Retain(ninth)) == wire::ErrorCode::Ok, "Released image reader capacity did not recover");
}
void PublicationFailureRollback()
{
	wire::test::Fixture fixture;
	Client client(fixture.service);
	fixture.state->failReaderDescription = true;
	const auto rejected = client.Retain(fixture.capture);
	Require(Error(rejected) == wire::ErrorCode::Capacity && !rejected.message.Find(tag::ImageReaderId) &&
				fixture.state->readersDestroyed == 1,
			"Failed reader publication retained an unannounced resource");
	const auto image = Id(client.Retain(fixture.capture), tag::ImageReaderId);
	fixture.state->failDeliveryDescription = true;
	const auto delivery = client.Send(client.Share(image));
	Require(Error(delivery) == wire::ErrorCode::Capacity && delivery.handles.empty() && !delivery.exposure &&
				!delivery.message.Find(tag::DeliveryId) && fixture.state->deliveriesDestroyed == 1 &&
				fixture.state->exposures == 0,
			"Failed delivery publication leaked native handles, exposure or table entry");
	fixture.state->failExportDescription = true;
	const auto exported = client.Call(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, image)});
	Require(Error(exported) == wire::ErrorCode::Capacity && !exported.message.Find(tag::ExportId) &&
				fixture.state->exportsDestroyed == 1 && fixture.state->reads == 0,
			"Failed export publication retained unannounced ticket");
	for (std::size_t index = 0; index < simxr::SnapshotBudget::Capacity(); ++index)
	{
		Require(Error(client.Send(client.Share(image))) == wire::ErrorCode::Ok,
				"Delivery publication failure consumed bounded capacity");
		Require(Error(client.Call(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, image)})) ==
					wire::ErrorCode::Ok,
				"Export publication failure consumed bounded capacity");
	}
}
void BoundedTables()
{
	wire::test::Fixture fixture;
	Client client(fixture.service);
	const auto image = Id(client.Retain(fixture.capture), tag::ImageReaderId);
	std::vector<std::uint64_t> deliveries, exports;
	for (std::size_t index = 0; index < simxr::SnapshotBudget::Capacity(); ++index)
	{
		auto response = client.Send(client.Share(image));
		Require(Error(response) == wire::ErrorCode::Ok, "Fill bounded delivery table");
		deliveries.push_back(Id(response, tag::DeliveryId));
		auto exported = client.Call(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, image)});
		Require(Error(exported) == wire::ErrorCode::Ok, "Fill independent export table");
		exports.push_back(Id(exported, tag::ExportId));
	}
	Require(Error(client.Send(client.Share(image))) == wire::ErrorCode::Capacity && fixture.state->shares == simxr::SnapshotBudget::Capacity() &&
				Error(client.Call(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, image)})) ==
					wire::ErrorCode::Capacity &&
				fixture.state->exports == simxr::SnapshotBudget::Capacity(),
			"Table capacity checked after provider side effects");
	client.Call(wire::OpCode::ReleaseDelivery, {wire::Field::U64(tag::DeliveryId, deliveries.front())});
	client.Call(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, exports.front())});
	Require(Error(client.Send(client.Share(image))) == wire::ErrorCode::Ok &&
				Error(client.Call(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, image)})) ==
					wire::ErrorCode::Ok,
			"Released independent table capacity did not recover");
	Require(fixture.state->reads == 0, "Metadata/share/capacity checks invoked Read");
}
} // namespace
int main()
{
	try
	{
		ImageLifetimesAndReplay();
		BoundedImageReadersAndReentry();
		PublicationFailureRollback();
		BoundedTables();
		std::cout << "PASS CPU image service contracts; no GPU qualification\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
