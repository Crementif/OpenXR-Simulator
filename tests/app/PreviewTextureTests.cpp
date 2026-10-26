#include "../TestSupport.h"
#include "../control/FakeImageInspection.h"
#include "app/render/PreviewImages.h"
#include "app/services/SpatialMath.h"
#include "control/LocalControlServer.h"
#include "graphics/SnapshotBudget.h"
#include "math/Math.h"
#include <cmath>

#include "app/platform/Window.h"
#include "app/render/PreviewTexture.h"
#include "app/render/UiRenderer.h"
#include "control/backends/vulkan/VulkanControlHandle.h"
#include "graphics/backends/vulkan/VulkanExternal.h"
#include <filesystem>
#include <fstream>
#include <thread>

#include <algorithm>
#include <array>
#include <iostream>

namespace simxr::tests
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
// Real external images exercise history copies and the same UI submission path
// used by the companion, including descriptor lifetime across a format change.
std::unique_ptr<app::PreviewTexture> HistorySource(app::UiRenderer& renderer, VkExtent2D extent, VkFormat format,
												   std::optional<XrFovf> fov = {})
{
	const auto access = renderer.DeviceAccess();
	auto device = std::make_shared<VulkanDevice>(
		VulkanBinding{access.instance, access.physical, access.device, access.queueFamily, 0}, access.owner);
	const std::array extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
								VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
	VulkanExternalDevice external(device, extensions, VK_API_VERSION_1_1);
	VulkanExternalImageSpec spec;
	spec.extent = {extent.width, extent.height, 1};
	spec.arrayLayers = 1;
	spec.format = format;
	auto image = external.CreateExportable(spec);
	auto ready = external.CreateExportableSemaphore();
	struct Commands
	{
		VkDevice device;
		VkCommandPool pool = VK_NULL_HANDLE;
		~Commands()
		{
			if (pool)
				vkDestroyCommandPool(device, pool, nullptr);
		}
	} commands{access.device};
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = access.queueFamily;
	CheckVulkan(vkCreateCommandPool(access.device, &pool, nullptr, &commands.pool), "create history source pool");
	VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocation.commandPool = commands.pool;
	allocation.commandBufferCount = 1;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer command;
	CheckVulkan(vkAllocateCommandBuffers(access.device, &allocation, &command), "allocate history source command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CheckVulkan(vkBeginCommandBuffer(command, &begin), "begin history source command");
	VulkanImageBarrier(command, *image->Image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
					   VK_ACCESS_TRANSFER_WRITE_BIT);
	const VkClearColorValue color{{0.2f, 0.4f, 0.6f, 1.f}};
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(command, image->Image()->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
	VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release.srcQueueFamilyIndex = access.queueFamily;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release.image = image->Image()->image;
	release.subresourceRange = range;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
						 0, nullptr, 1, &release);
	CheckVulkan(vkEndCommandBuffer(command), "end history source command");
	const auto semaphore = ready->Get();
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &semaphore;
	CheckVulkan(vkQueueSubmit(device->queue, 1, &submit, VK_NULL_HANDLE), "submit history source");
	CheckVulkan(vkQueueWaitIdle(device->queue), "finish history source");
	auto description = Description(access);
	description.width = extent.width;
	description.height = extent.height;
	if (fov)
		description.views = {{IdentityPose(), *fov}};
	description.arrayLayers = 1;
	description.format = format;
	description.usage = spec.usage;
	description.allocationSize = image->Descriptor().allocationSize;
	description.memoryTypeIndex = image->Descriptor().memoryTypeIndex;
	description.dedicated = image->Descriptor().dedicated;
	description.layout = VK_IMAGE_LAYOUT_GENERAL;
	description.producerQueueFamily = access.queueFamily;
	description.handleKind = static_cast<std::uint32_t>(wire::VulkanTransferKind);
	const std::array handles{wire::OwnVulkanControlHandle(image->ExportMemory()),
							 wire::OwnVulkanControlHandle(ready->ExportOnce())};
	auto texture = std::make_unique<app::PreviewTexture>(access, description);
	texture->Import(description, handles);
	Eventually(
		[&]
		{
			return texture->Poll() == CompletionStatus::Complete;
		},
		"Import actual history pixels");
	return texture;
}
void HistoryLifetime(app::UiRenderer& renderer)
{
	auto source = HistorySource(renderer, {64, 64}, VK_FORMAT_R8G8B8A8_UNORM,
								XrFovf{-std::atan(2.f), std::atan(2.f), std::atan(1.f), -std::atan(1.f)});
	auto changed = HistorySource(renderer, {32, 16}, VK_FORMAT_R8G8B8A8_SRGB, XrFovf{-.8f, .8f, .8f, -.8f});
	// Use actual allocation requirements to make exactly one reusable history slot.
	const auto access = renderer.DeviceAccess();
	auto device = std::make_shared<VulkanDevice>(
		VulkanBinding{access.instance, access.physical, access.device, access.queueFamily, 0}, access.owner);
	const auto allocation = VulkanImage::Create(device, VK_FORMAT_R8G8B8A8_UNORM, {64, 64}, 1,
												VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
								->allocationSize;
	app::PreviewHistory history(renderer, allocation * 2);
	auto begin = [&]
	{
		Eventually(
			[&]
			{
				return renderer.BeginFrame();
			},
			"Admit history UI frame");
		history.BeginFrame();
		source->BeginFrame();
		changed->BeginFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({400, 400});
		ImGui::Begin("History lifetime");
	};
	auto finish = [&](bool transfers = true)
	{
		ImGui::End();
		std::vector<VkSemaphore> waits;
		for (auto* texture : {source.get(), changed.get()})
			if (const auto wait = texture->ReadyWait())
				waits.push_back(wait);
		const auto previous = renderer.SubmittedFrames();
		bool abandoned = false;
		try
		{
			Require(renderer.Render(waits,
									[&](VkCommandBuffer command)
									{
										history.Record(command);
										if (!transfers)
											throw std::runtime_error("Abandon recorded history copy");
									}),
					"Submit history UI frame");
		}
		catch (const std::runtime_error& error)
		{
			if (transfers || std::string_view(error.what()) != "Abandon recorded history copy")
				throw;
			abandoned = true;
		}
		Require(abandoned != transfers, "History recording failure was not exercised");
		if (transfers)
		{
			history.Submitted(renderer.SubmittedFrames());
			for (auto* texture : {source.get(), changed.get()})
				texture->Submitted(renderer.SubmittedFrames());
		}
		else
			Require(renderer.SubmittedFrames() == previous, "Abandoned commands reached the GPU queue");
		renderer.WaitIdle();
		for (auto* texture : {source.get(), changed.get()})
			texture->ObserveCompletion(renderer.CompletedFrames());
	};
	const app::FrameIdentity first{1, 1, 1}, second{1, 1, 2};
	begin();
	history.Offer(*source, 0, first, 1);
	Require(history.Nearest(first, 1).has_value(), "History did not accept a real native image");
	// Drop an offered copy; next frame must accept the same identity again.
	finish(false);
	begin();
	Require(!history.Nearest(first, 1), "Abandoned history copy remained visible");
	history.Offer(*source, 0, first, 1);
	const auto original = history.Nearest(first, 1);
	Require(original.has_value(), "Abandoned history frame could not be retried");
	Require(std::abs(original->aspect - .5f) < .0001f, "History lost the recorded camera aspect");
	source->Draw(120, 0);
	Require(std::abs(ImGui::GetItemRectSize().y - 60) < .01f, "Native preview ignored camera proportions");
	source->Draw(120, 0, {0, 0, 1, 1}, false);
	Require(std::abs(ImGui::GetItemRectSize().y - 120) < .01f, "Pixel zoom corrected the source aspect");
	history.Draw(*original, 120);
	Require(std::abs(ImGui::GetItemRectSize().y - 60) < .01f, "Cached preview ignored recorded proportions");
	finish();
	std::this_thread::sleep_for(std::chrono::milliseconds(110));
	begin();
	Require(history.Shots().size() == 1, "History reuse fixture did not have one slot");
	history.DrawThumbnail(*original, {10, 10}, {70, 70}, {0, 0, 1, 1});
	history.Offer(*source, 0, second, 1);
	Require(history.Nearest(second, 1)->frame == first, "History overwrote a currently drawn thumbnail");
	finish();
	begin();
	history.Draw(*original, 120);
	history.Offer(*source, 0, second, 1);
	Require(history.Nearest(second, 1)->frame == first, "History overwrote the currently displayed frame");
	finish();
	begin();
	// A new epoch may reuse the exact same runtime frame identity.
	history.Offer(*source, 0, first, 2);
	const auto replacement = history.Nearest(first, 2);
	Require(replacement.has_value() && !history.Nearest(first, 1), "History deduplicated across epochs");
	const auto vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size;
	history.Draw(*original, 120);
	history.DrawThumbnail(*original, {10, 10}, {70, 70}, {0, 0, 1, 1});
	Require(ImGui::GetWindowDrawList()->VtxBuffer.Size == vertices, "Stale history identity drew a reused slot");
	history.Draw(*replacement, 120);
	finish();
	std::this_thread::sleep_for(std::chrono::milliseconds(110));
	begin();
	history.DrawThumbnail(*replacement, {10, 10}, {70, 70}, {0, 0, 1, 1});
	history.Draw(*replacement, 120);
	history.Offer(*changed, 0, second, 2);
	Require(history.Nearest(first, 2).has_value(), "Format change retired borrowed UI descriptors before submission");
	finish();
	begin();
	Require(!history.Nearest(first, 2), "Deferred format change did not replace history at the frame boundary");
	history.Offer(*changed, 0, second, 2);
	const auto resized = history.Nearest(second, 2);
	Require(resized && history.Aspect() == 1.f && resized->aspect == 1.f, "Deferred history format/size change failed");
	history.Draw(*resized, 120);
	finish();
}

void ScreenshotFiles(app::UiRenderer& renderer, app::Window& window)
{
	struct Directory
	{
		char path[48] = "/tmp/simxr-ui-screenshot-XXXXXX";
		Directory()
		{
			Require(::mkdtemp(path) != nullptr, "Create private screenshot directory");
		}
		~Directory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	} directory;
	auto draw = [&]
	{
		Eventually(
			[&]
			{
				return renderer.BeginFrame();
			},
			"Admit screenshot frame");
		ImGui::GetBackgroundDrawList()->AddRectFilled({20, 20}, {80, 80}, IM_COL32(23, 67, 131, 255));
		Require(renderer.Render(), "Render explicit screenshot");
	};
	bool invalidPath = false;
	try
	{
		renderer.RequestScreenshot(std::filesystem::path(std::string("truncated\0path", 14)));
	}
	catch (const std::invalid_argument&)
	{
		invalidPath = true;
	}
	Require(invalidPath, "Embedded NUL screenshot path was accepted");
	// These fail after a real submission; a later screenshot must still work.
	for (const auto& path : {std::filesystem::path(directory.path), std::filesystem::path("/dev/full")})
	{
		renderer.RequestScreenshot(path);
		const auto serial = renderer.SubmittedFrames();
		bool failed = false;
		try
		{
			draw();
		}
		catch (const std::ios_base::failure&)
		{
			failed = true;
		}
		Require(failed && renderer.SubmittedFrames() == serial + 1,
				"Screenshot open/write failure was hidden or lost its GPU submission");
	}
	window.Resize(640, 480);
	Eventually(
		[&]
		{
			window.PollEvents();
			return window.PixelSize() == std::pair{640, 480};
		},
		"Resize screenshot surface");
	const auto output = std::filesystem::path(directory.path) / std::filesystem::path(u8"捕获-é.bmp");
	renderer.RequestScreenshot(output);
	draw();
	std::ifstream file(output, std::ios::binary);
	file.exceptions(std::ios::badbit | std::ios::failbit);
	std::array<unsigned char, 54> header{};
	file.read(reinterpret_cast<char*>(header.data()), header.size());
	auto u32 = [&](std::size_t offset)
	{
		std::uint32_t value = 0;
		for (unsigned byte = 0; byte < 4; ++byte)
			value |= std::uint32_t(header[offset + byte]) << (8 * byte);
		return value;
	};
	const auto [width, height] = window.PixelSize();
	Require(header[0] == 'B' && header[1] == 'M' && u32(10) == 54 && u32(18) == std::uint32_t(width) &&
				u32(22) == 0u - std::uint32_t(height) && u32(2) == std::filesystem::file_size(output),
			"Unicode screenshot had an invalid BMP header or did not follow the resized surface");
	file.seekg(54 + (40 * width + 40) * 4);
	std::array<unsigned char, 4> pixel{};
	file.read(reinterpret_cast<char*>(pixel.data()), pixel.size());
	file.close();
	Require(std::abs(int(pixel[0]) - 131) <= 1 && std::abs(int(pixel[1]) - 67) <= 1 &&
				std::abs(int(pixel[2]) - 23) <= 1,
			"Screenshot did not preserve drawn RGB pixels in BMP byte order");
	renderer.WaitIdle();
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
	const auto previousBudget = SnapshotBudget::MiB();
	SnapshotBudget::Configure(192);
	Require(SnapshotBudget::Capacity() == 3, "Three-image cache pressure fixture changed");
	retained(107, 8);
	wire::Message unavailable;
	unavailable.fields = {wire::Field::Bool(wire::tag::ImageAvailable, false)};
	images.Retained(unavailable, true);
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
	SnapshotBudget::Configure(previousBudget);
	requests.Close();
}
void PreviewProportions()
{
	wire::ImageDescription image;
	image.width = 1280;
	image.height = 720;
	image.arrayLayers = 2;
	Require(std::abs(app::PreviewAspect(image) - 9.f / 16) < .0001f, "Image without camera lost pixel aspect");
	image.views = {{IdentityPose(), {-.7f, .9f, .8f, -.6f}}, {IdentityPose(), {-.5f, .5f, .9f, -.9f}}};
	for (std::size_t eye = 0; eye < image.views.size(); ++eye)
	{
		// A front-facing 16:9 screen must remain 16:9 despite nonuniform pixel density.
		const auto left = app::ProjectPoint(image.views[eye], {-8.f / 9, 0, -2});
		const auto right = app::ProjectPoint(image.views[eye], {8.f / 9, 0, -2});
		const auto top = app::ProjectPoint(image.views[eye], {0, .5f, -2});
		const auto bottom = app::ProjectPoint(image.views[eye], {0, -.5f, -2});
		const float displayed = (right.uv.x - left.uv.x) / (bottom.uv.y - top.uv.y) / app::PreviewAspect(image, eye);
		Require(std::abs(displayed - 16.f / 9) < .0001f, "Projected screen aspect changed with eye FOV");
	}
	Require(app::PreviewAspect(image) == app::PreviewAspect(image, 1), "Stereo fit clipped the taller eye");
	Require(std::abs(app::PreviewAspect(image, 0, false) - 9.f / 16) < .0001f, "Pixel inspection lost pixel aspect");
	const float aspect = app::PreviewAspect(image, 0);
	image.width = 720;
	image.height = 1280;
	Require(app::PreviewAspect(image, 0) == aspect, "Render resolution changed physical proportions");
	image.views[0].fov = {};
	Require(std::abs(app::PreviewAspect(image, 0) - 16.f / 9) < .0001f, "Degenerate camera lacked pixel fallback");
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
} // namespace simxr::tests
int main()
{
	using namespace simxr::tests;
	try
	{
		PreviewProportions();
		auto validation = std::make_shared<app::ValidationLog>();
		{
			app::Window window(true);
			app::UiRenderer renderer(window, true, validation, true);
			const auto access = renderer.DeviceAccess();
			if (!access.externalImages || !access.externalFence || !access.ioQueue)
			{
				std::cout << "SKIP preview texture: external sharing unavailable\n";
				return 77;
			}
			DisplayImageIdentity(renderer);
			BoundAndRollback(access);
			HistoryLifetime(renderer);
			ScreenshotFiles(renderer, window);
		}
		Require(validation->errors.load() == 0, "Preview texture lifetime produced Vulkan validation errors");
		std::cout << "PASS display-image identity, preview texture bounds, native rollback, unsent retirement and "
					 "history submission lifetime and screenshot I/O\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
