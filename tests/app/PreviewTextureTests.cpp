#include "graphics/SnapshotBudget.h"
#include "../TestSupport.h"
#include "app/PreviewTexture.h"
#include "app/UiRenderer.h"
#include "app/Window.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

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
			BoundAndRollback(access);
		}
		Require(validation->errors.load() == 0, "Preview texture lifetime produced Vulkan validation errors");
		std::cout << "PASS preview texture bounds, native rollback and unsent retirement\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
