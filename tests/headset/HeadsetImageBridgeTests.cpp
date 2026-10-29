#include "../control/FakeImageInspection.h"
#include "Pch.h"
#include "control/LocalControlServer.h"
#include "headset/HeadsetImageBridge.h"

#include <iostream>

namespace simxr::tests
{
using namespace headset;

static void Until(const std::function<bool()>& predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!predicate())
	{
		Require(std::chrono::steady_clock::now() < deadline, "Image bridge deadline expired");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

static void Freshness()
{
	using namespace std::chrono;
	HeadsetImagePacket packet;
	packet.requestedAt = steady_clock::time_point{} + seconds(1);
	Require(!FreshHeadsetImage(packet, milliseconds(10), packet.requestedAt), "Unknown source age is fresh");
	packet.sourceAge = milliseconds(5);
	Require(HeadsetImageAge(packet, packet.requestedAt + milliseconds(3)) == milliseconds(8),
			"Source age must include elapsed delivery time");
	Require(FreshHeadsetImage(packet, milliseconds(10), packet.requestedAt + milliseconds(34)),
			"Fresh source rejected before four display periods");
	Require(!FreshHeadsetImage(packet, milliseconds(10), packet.requestedAt + milliseconds(35)),
			"Transport/copy delay was omitted from source age");
	packet.sourceAge = milliseconds(50);
	Require(!FreshHeadsetImage(packet, milliseconds(10), packet.requestedAt), "Old reprojected frame became fresh");
	packet.sourceAge = nanoseconds::max();
	Require(!HeadsetImageAge(packet, packet.requestedAt + nanoseconds(1)), "Overflowed source age must remain unknown");
	Require(!FreshHeadsetImage(packet, nanoseconds::max(), packet.requestedAt), "Source-age arithmetic overflowed");
	packet.sourceAge = nanoseconds::zero();
	Require(!FreshHeadsetImage(packet, milliseconds(10), packet.requestedAt - milliseconds(1)),
			"Future request time was accepted");
}

static void DeliveryLifecycle()
{
	control::test::Fixture fixture;
	const auto endpoint =
		(std::filesystem::temp_directory_path() / ("simxr-headset-images-" + std::to_string(getpid()) + ".sock"))
			.string();
	control::LocalControlServer server(fixture.service, endpoint);
	HeadsetImageSource source{endpoint, 1, fixture.session->Snapshot().generation.value};
	HeadsetImageBridge bridge(source);
	auto completion = control::OwnControlFd(::memfd_create("simxr-fake-consumer", MFD_CLOEXEC));
	const auto request = [&](std::uint64_t after = 0) {
		Require(bridge.Error().empty(), "Unexpected live image failure");
		return bridge.Request(control::test::Description().identity, completion, after, ImageCompletionKind::Fence);
	};
	const auto receive = [&] {
		std::optional<HeadsetImagePacket> packet;
		Until([&] {
			Require(bridge.Error().empty(), bridge.Error().c_str());
			packet = bridge.Take();
			return packet.has_value();
		});
		return std::move(*packet);
	};
	Require(request(), "Initial request rejected");
	Require(receive().kind == HeadsetImageKind::Unavailable && fixture.state->shares == 0,
			"Unselected display exposed an image");
	const auto select = [&](CaptureId capture) {
		auto tick = fixture.session->Captures().Read(capture)->metadata->prediction.displayTick;
		if (fixture.session->Display().NextTick() > tick)
			tick = fixture.session->Display().NextTick();
		while (fixture.session->Display().NextTick() <= tick)
			Require(fixture.session->Display().Step().has_value(), "Select completed display image");
	};
	select(fixture.capture);
	Require(request(), "Selected image request rejected");
	Require(!request(), "Second outstanding image request accepted");
	auto first = receive();
	Require(first.session == source.session && first.frame.generation.value == source.generation &&
				first.frame.frame == fixture.session->Captures().Read(fixture.capture)->metadata->prediction.id,
			"Delivered image lost its original application frame identity");
	Require(first.kind == HeadsetImageKind::Image && first.delivery && first.imageTick && first.handles.size() == 2,
			"Native delivery metadata/handles missing");
	Require(!bridge.Take() && !request() && !bridge.Acknowledge(first.delivery + 1),
			"Outstanding delivery lost its exclusive ownership");
	Require(fixture.state->deliveriesDestroyed == 0 && fixture.state->shares == 1,
			"Delivery retired before completion acknowledgement");
	fixture.state->consumerPhase = CompletionStatus::Complete;
	Require(bridge.Acknowledge(first.delivery) && !bridge.Acknowledge(first.delivery),
			"Delivery acknowledgement identity was not consumed once");
	Until([&] { return request(first.imageTick); });
	const auto repeated = receive();
	Require(repeated.session == first.session && repeated.frame == first.frame,
			"Unchanged image reply changed its source identity");
	Require(repeated.kind == HeadsetImageKind::Unchanged && repeated.imageTick == first.imageTick &&
				repeated.handles.empty() && fixture.state->shares == 1,
			"Repeated display image created a second GPU delivery");
	const auto next = fixture.Submit();
	select(next);
	Require(request(first.imageTick), "Next image request rejected");
	auto second = receive();
	Require(second.kind == HeadsetImageKind::Image && second.imageTick != first.imageTick && fixture.state->shares == 2,
			"New display image reused stale pixels");
	Require(bridge.Acknowledge(second.delivery), "Acknowledge second delivery");
	FramePrediction prediction;
	Require(fixture.session->Scheduler().GrantPermits() == XR_SUCCESS &&
				fixture.session->WaitFrame(prediction) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS &&
				fixture.session->EndFrame(prediction) == XR_SUCCESS,
			"Submit clear frame");
	do
	{
		Require(fixture.session->Display().Step().has_value(), "Select clear frame");
	}
	while (fixture.session->Display().NextTick() <= prediction.displayTick);
	Until([&] { return request(second.imageTick); });
	Require(receive().kind == HeadsetImageKind::Clear && fixture.state->shares == 2 && fixture.state->reads == 0,
			"Clear output exposed stale image or read pixels");
	bridge.Stop();
	Require(!request(), "Stopped bridge accepted work");

	HeadsetImageBridge incompatible(source);
	select(next);
	const auto newest = fixture.Submit();
	select(newest);
	Require(incompatible.Request(control::test::Description().identity, completion), "Queue incompatible primitive");
	Until([&] { return !incompatible.Error().empty(); });
	Require(fixture.state->shares == 2, "Incompatible completion primitive exposed native handles");

	HeadsetImageBridge pending(source);
	fixture.state->consumerPhase = CompletionStatus::Pending;
	Require(pending.Request(control::test::Description().identity, completion, 0, ImageCompletionKind::Fence),
			"Queue pending consumer");
	std::optional<HeadsetImagePacket> outstanding;
	Until([&] {
		Require(pending.Error().empty(), pending.Error().c_str());
		outstanding = pending.Take();
		return outstanding.has_value();
	});
	Require(outstanding->kind == HeadsetImageKind::Image, "Missing outstanding shutdown image");
	pending.Stop();
	Require(!pending.Acknowledge(outstanding->delivery) && pending.Error().empty() &&
				fixture.state->consumerPhase == CompletionStatus::Pending,
			"Shutdown claimed consumer completion or accepted a late acknowledgement");
	fixture.state->consumerPhase = CompletionStatus::Complete;

	++source.generation;
	HeadsetImageBridge stale(source);
	Require(stale.Request(control::test::Description().identity, completion), "Queue stale target check");
	Until([&] { return !stale.Error().empty(); });
	Require(!stale.Take() && fixture.state->shares == 3, "Stale generation exposed an image");
}
} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::Freshness();
		simxr::tests::DeliveryLifecycle();
		std::cout << "PASS headset image transport lifecycle; CPU fixture, no GPU qualification\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
