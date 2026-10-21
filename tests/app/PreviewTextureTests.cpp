#include "graphics/SnapshotBudget.h"
#include "../TestSupport.h"
#include "../control/FakeImageInspection.h"
#include "app/PreviewImages.h"
#include "control/LocalControlServer.h"

#include "app/PreviewTexture.h"
#include "app/UiRenderer.h"
#include "app/Window.h"
#include "graphics/backends/vulkan/VulkanExternal.h"
#include <filesystem>

#include <algorithm>
#include <array>
#include <iostream>

namespace
{
using namespace simxr;
namespace app = simxr::app;
namespace wire = simxr::control;

wire::ImageDescription Description(const app::VulkanDeviceAccess& access)
{
	auto device = std::make_shared<VulkanDevice>(
		VulkanBinding{access.instance, access.physical, access.device, access.queueFamily, 1}, access.owner);
	const std::array extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
								VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
	VulkanExternalDevice external(device, extensions, VK_API_VERSION_1_1);
	wire::ImageDescription description;
	std::transform(external.Identity().deviceUuid.begin(), external.Identity().deviceUuid.end(),
				   description.identity.device.begin(),
				   [](auto byte)
				   {
					   return static_cast<std::byte>(byte);
				   });
	std::transform(external.Identity().driverUuid.begin(), external.Identity().driverUuid.end(),
				   description.identity.driver.begin(),
				   [](auto byte)
				   {
					   return static_cast<std::byte>(byte);
				   });
	return description;
}
void DisplayImageIdentity(app::UiRenderer& renderer)
{
	struct Directory
	{
		char path[40] = "/tmp/simxr-image-key-XXXXXX";
		Directory()
		{
			Require(::mkdtemp(path) != nullptr, "Create private image-identity endpoint directory");
		}
		~Directory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	} directory;
	wire::test::Fixture fixture;
	wire::LocalControlServer server(fixture.service, std::string(directory.path) + "/control.sock");
	app::RuntimePanelRequests requests;
	requests.Connect(server.Path());
	Eventually(
		[&]
		{
			return requests.Snapshot().state == app::RuntimeConnectionState::Connected;
		},
		"Connect isolated image-identity endpoint");
	requests.RefreshEpoch();
	app::PreviewImages images(renderer, requests);
	images.SetScope({requests.Epoch(), 1, 1});
	const auto description = Description(renderer.DeviceAccess());
	auto retained = [&](std::uint64_t reader, std::optional<std::uint64_t> imageTick)
	{
		wire::Message message;
		message.fields = {wire::Field::Bool(wire::tag::ImageAvailable, true),
						  wire::Field::U64(wire::tag::Session, 1),
						  wire::Field::U64(wire::tag::Generation, 1),
						  wire::Field::U64(wire::tag::CaptureId, 0),
						  wire::Field::U64(wire::tag::ImageReaderId, reader),
						  wire::EncodeImageDescription(description),
						  wire::Field::Bytes(wire::tag::Frame,
											 wire::EncodeFields(std::array{wire::Field::U64(wire::tag::Generation, 1),
																		   wire::Field::U64(wire::tag::FrameId, 7)}))};
		if (imageTick)
			message.fields.push_back(wire::Field::U64(wire::tag::ImageTick, *imageTick));
		images.Retained(message, true);
	};
	// Readers are deliberately absent from the fixture: exercise acquisition
	// identity and native fence retirement, without pretending to import pixels.
	retained(101, 5);
	retained(102, 5);
	Require(images.Images().size() == 1 && images.Images().front().imageTick == 5,
			"Recycled reader ID duplicated an unchanged display image");
	retained(103, 6);
	retained(104, 6);
	const auto versions = images.Images();
	Require(versions.size() == 2 && versions[0].frame == 7 && versions[1].frame == 7 && versions[0].imageTick == 5 &&
				versions[1].imageTick == 6,
			"Changed display image was discarded because its application frame did not change");
	retained(105, {});
	retained(106, {});
	Require(images.Images().size() == 3 && !images.Images().back().imageTick && images.ReadyCount() == 0,
			"Legacy preview identity was duplicated or pending images were marked ready");
	unsigned rejected = 0;
	Eventually(
		[&]
		{
			for (auto& reply : requests.Poll())
				if (reply.task.kind == app::PanelWork::Share)
				{
					Require(reply.response.packet.message.Find(wire::tag::Error)->AsU32() ==
								static_cast<std::uint32_t>(wire::ErrorCode::NotFound),
							"Fixture unexpectedly supplied native pixels");
					images.ShareFailed(reply.task.id);
					++rejected;
				}
			images.Poll();
			return rejected == 3 && images.Images().empty();
		},
		"Rejected image offers failed to retire their native fences");
	requests.Close();
}
void BoundAndRollback(const app::VulkanDeviceAccess& access)
{
	const auto description = Description(access);
	auto wrongAdapter = description;
	wrongAdapter.identity.device[0] ^= std::byte{1};
	bool rejected = false;
	try
	{
		app::PreviewTexture wrong(access, wrongAdapter);
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	Require(rejected, "Mismatched device identity created a preview texture");
	std::vector<std::unique_ptr<app::PreviewTexture>> textures(SnapshotBudget::Capacity());
	for (auto& texture : textures)
		texture = std::make_unique<app::PreviewTexture>(access, description);
	rejected = false;
	try
	{
		app::PreviewTexture overflow(access, description);
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	Require(rejected, "Preview textures exceeded their bounded retirement storage");
	for (auto& texture : textures)
	{
		Require(!texture->Ready() && texture->Poll() == CompletionStatus::Pending && !texture->ReadyWait(),
				"Unimported preview was published as a sampled image");
		auto offered = texture->ConsumerFence();
		Require(offered && offered->Value() >= 0, "Consumer fence did not retain an actual native FD");
		texture->AbandonUnsent();
		Require(texture->CanRetire(0), "Unsent consumer reservation failed to retire");
		texture.reset();
	}
	// Constructor rejection and normal retirement must return both slot budgets.
	for (auto& texture : textures)
		texture = std::make_unique<app::PreviewTexture>(access, description);
}
} // namespace
int main()
{
	try
	{
		auto validation = std::make_shared<app::ValidationLog>();
		{
			app::Window window(true);
			app::UiRenderer renderer(window, true, validation);
			const auto access = renderer.DeviceAccess();
			if (!access.externalImages || !access.externalFence || !access.ioQueue)
			{
				std::cout << "SKIP preview texture: external sharing unavailable\n";
				return 77;
			}
			DisplayImageIdentity(renderer);
			BoundAndRollback(access);
		}
		Require(validation->errors.load() == 0, "Preview texture lifetime produced Vulkan validation errors");
		std::cout << "PASS display-image identity, preview texture bounds, native rollback and unsent retirement\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
