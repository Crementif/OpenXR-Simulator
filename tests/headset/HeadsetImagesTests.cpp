#include "../control/FakeImageInspection.h"
#include "Pch.h"
#include "control/LocalControlServer.h"
#include "graphics/backends/vulkan/VulkanResources.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetImages.h"

#include <iostream>

namespace simxr::headset::tests
{
static unsigned s_alive = 0, s_peak = 0, s_submits = 0;
static bool s_complete = true;
static std::thread::id s_frameThread;

static void OnFrameThread()
{
	Require(std::this_thread::get_id() == s_frameThread, "GPU consumer called from transport worker");
}
} // namespace simxr::headset::tests

namespace simxr
{
VulkanDevice::VulkanDevice(const VulkanBinding& value, std::shared_ptr<const void> owner)
	: binding(value), m_owner(std::move(owner))
{}

VulkanDevice::~VulkanDevice() = default;

VulkanCommands::~VulkanCommands()
{
	Require(!pool && !fence && !descriptors && !timestamps, "Transport fixture created native command storage");
}
} // namespace simxr

namespace simxr::control
{
struct VulkanImageConsumer::State
{
	ImageDescription description;
	std::shared_ptr<TransferHandle> completion;
	bool submitted = false, idle = true;
};

VulkanImageConsumer::VulkanImageConsumer(std::shared_ptr<VulkanDevice>, std::span<const char* const>, std::uint32_t,
										 ImageCompletionKind, bool, bool)
	: m_state(std::make_unique<State>())
{
	headset::tests::OnFrameThread();
	headset::tests::s_peak = std::max(headset::tests::s_peak, ++headset::tests::s_alive);
}

VulkanImageConsumer::~VulkanImageConsumer()
{
	headset::tests::OnFrameThread();
	Require(m_state->idle, "Destroyed consumer before copy completion");
	--headset::tests::s_alive;
}

ImageDeviceIdentity VulkanImageConsumer::Identity() const
{
	return test::Description().identity;
}

std::shared_ptr<TransferHandle> VulkanImageConsumer::ConsumerFence()
{
	if (!m_state->completion)
		m_state->completion = OwnControlFd(::memfd_create("simxr-fake-image-completion", MFD_CLOEXEC));
	return m_state->completion;
}

void VulkanImageConsumer::Import(const ImageDescription& description,
								 std::span<const std::shared_ptr<TransferHandle>> handles)
{
	headset::tests::OnFrameThread();
	Require(handles.size() == 2, "Missing image handles");
	m_state->description = description;
}

void VulkanImageConsumer::Submit()
{
	headset::tests::OnFrameThread();
	m_state->submitted = true;
	m_state->idle = false;
	++headset::tests::s_submits;
}

CompletionStatus VulkanImageConsumer::Poll()
{
	headset::tests::OnFrameThread();
	if (m_state->submitted && headset::tests::s_complete)
		m_state->idle = true;
	return m_state->idle ? CompletionStatus::Complete : CompletionStatus::Pending;
}

bool VulkanImageConsumer::NativeIdle() const
{
	return m_state->idle;
}

void VulkanImageConsumer::CancelUnused()
{
	headset::tests::OnFrameThread();
	if (!m_state->submitted)
		m_state->idle = true;
}

void VulkanImageConsumer::AbandonUnsent()
{
	Require(!m_state->submitted, "Abandoned submitted image");
	m_state->completion.reset();
}
} // namespace simxr::control

namespace simxr::headset
{
HeadsetGraphics::~HeadsetGraphics() = default;

VkInstance HeadsetGraphics::Instance() const
{
	return VK_NULL_HANDLE;
}

VkPhysicalDevice HeadsetGraphics::PhysicalDevice() const
{
	return VK_NULL_HANDLE;
}

VkDevice HeadsetGraphics::Device() const
{
	return VK_NULL_HANDLE;
}

std::uint32_t HeadsetGraphics::QueueFamily() const
{
	return 0;
}

std::uint32_t HeadsetGraphics::ApiVersion() const
{
	return VK_API_VERSION_1_2;
}

const std::vector<const char*>& HeadsetGraphics::Extensions() const
{
	return m_extensions;
}

void HeadsetGraphics::PreserveNativeResources()
{
	m_preserved = true;
}

bool HeadsetGraphics::ResourcesPreserved() const
{
	return m_preserved;
}
} // namespace simxr::headset

namespace simxr::headset::tests
{
static void Until(HeadsetImages& images, const std::function<bool()>& done)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!done())
	{
		Require(std::chrono::steady_clock::now() < deadline, "Image stream deadline expired");
		images.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

static void Ownership()
{
	s_frameThread = std::this_thread::get_id();
	control::test::Fixture fixture;
	fixture.state->completionKind = ImageCompletionKind::Timeline;
	fixture.state->consumerPhase = CompletionStatus::Complete;
	fixture.state->views.resize(2);
	for (auto& view : fixture.state->views)
	{
		view.pose.orientation.w = 1;
		view.fov = {-.7f, .8f, .9f, -.6f};
	}
	const auto endpoint =
		(std::filesystem::temp_directory_path() / ("simxr-private-images-" + std::to_string(getpid()) + ".sock"))
			.string();
	control::LocalControlServer server(fixture.service, endpoint);
	HeadsetGraphics graphics;
	HeadsetImages images(graphics, {endpoint, 1, fixture.session->Snapshot().generation.value});
	const auto select = [&](CaptureId capture) {
		auto tick = fixture.session->Captures().Read(capture)->metadata->prediction.displayTick;
		if (fixture.session->Display().NextTick() > tick)
			tick = fixture.session->Display().NextTick();
		while (fixture.session->Display().NextTick() <= tick)
			Require(fixture.session->Display().Step().has_value(), "Select private-copy source");
	};
	select(fixture.capture);
	s_complete = false;
	Until(images, [&] { return s_submits == 1; });
	Require(!images.Current(), "Published pixels before copy completion");
	Require(images.Timing().copies == 0 && images.Timing().requests == 1, "Pending GPU copy was counted as completed");
	s_complete = true;
	Until(images, [&] { return images.Current() != nullptr; });
	Require(images.Timing().copies == 1 && images.Timing().maximumCopyObservation.count() >= 0,
			"Completed private copy was not observed");
	auto first = images.Current();
	Require(first->metadata.description.views[0].fov.angleLeft == -.7f && first->metadata.handles.empty(),
			"Private copy lost source view metadata or retained transfer handles");
	const auto next = [&] {
		auto old = images.Current();
		select(fixture.Submit());
		Until(images, [&] { return images.Current() != old; });
	};
	next();
	auto second = images.Current();
	next();
	auto third = images.Current();
	Require(s_alive == 3 && s_peak == 3, "Private image capacity is not bounded");
	const auto submissions = s_submits;
	select(fixture.Submit());
	for (unsigned index = 0; index < 10; ++index)
		images.Tick();
	Require(images.Current() == third && s_submits == submissions,
			"Overwrote an image held by a presenter or exceeded the copy pool");
	const auto timing = images.Timing();
	Require(timing.copies == 3 && timing.retained == 3 && timing.maximumRetained == 3 && timing.capacityWaits == 10,
			"Retained-reader backpressure was not measured independently of copies");
	first.reset();
	Until(images, [&] { return images.Current() != third; });
	Require(s_peak == 3, "Released presenter did not recover bounded capacity");
	second.reset();
	third.reset();
	const auto repeated = s_submits;
	for (unsigned index = 0; index < 20; ++index)
	{
		images.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Require(s_submits == repeated, "Repeated source performed another GPU copy");
	FramePrediction clear;
	Require(fixture.session->Scheduler().GrantPermits() == XR_SUCCESS &&
				fixture.session->WaitFrame(clear) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS &&
				fixture.session->EndFrame(clear) == XR_SUCCESS,
			"Submit private-image clear");
	do
	{
		Require(fixture.session->Display().Step().has_value(), "Select private-image clear");
	}
	while (fixture.session->Display().NextTick() <= clear.displayTick);
	Until(images, [&] { return !images.Current(); });
	Require(images.Close() && s_alive == 0 && !graphics.ResourcesPreserved(), "Clean stream did not retire");
	select(fixture.Submit());
	HeadsetImages borrowed(graphics, {endpoint, 1, fixture.session->Snapshot().generation.value});
	Until(borrowed, [&] { return borrowed.Current() != nullptr; });
	auto inFlight = borrowed.Current();
	Require(!borrowed.Close() && graphics.ResourcesPreserved() && s_alive == 1,
			"Shutdown destroyed graphics still borrowed by the presenter");
	inFlight.reset();
	Require(s_alive == 0, "Completed presenter did not release its private image");
}
} // namespace simxr::headset::tests

int main()
{
	try
	{
		simxr::headset::tests::Ownership();
		std::cout << "PASS private headset image ownership; mock GPU consumer\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
