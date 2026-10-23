#include "Math.h"
#include "VulkanProbeInstance.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanReadbackBatch.h"
#include "presentation/DebugController.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace simxr;
using simxr::probe::Check;
constexpr std::uint64_t Timeout = 5000000000ULL;
constexpr VkImageUsageFlags SourceUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
										  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

void Require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}
[[noreturn]] void UnknownCompletion(const char* operation)
{
	std::fprintf(stderr, "FAIL: %s; GPU completion unknown, preserving process-owned resources\n", operation);
	std::fflush(nullptr);
	std::_Exit(1);
}

struct Device
{
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice native = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	std::uint32_t family = 0;
	std::uint32_t index = 0;
	~Device()
	{
		if (native)
			vkDestroyDevice(native, nullptr);
	}
	bool Create(VkInstance instance)
	{
		std::uint32_t count = 0;
		Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate compositor adapters");
		std::vector<VkPhysicalDevice> adapters(count);
		Check(vkEnumeratePhysicalDevices(instance, &count, adapters.data()), "enumerate compositor adapters");
		for (auto adapter : adapters)
		{
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(adapter, &properties);
			if (properties.apiVersion < VK_API_VERSION_1_1)
				continue;
			vkGetPhysicalDeviceQueueFamilyProperties(adapter, &count, nullptr);
			std::vector<VkQueueFamilyProperties> families(count);
			vkGetPhysicalDeviceQueueFamilyProperties(adapter, &count, families.data());
			for (std::uint32_t candidate = 0; candidate < count; ++candidate)
			{
				if (!(families[candidate].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !families[candidate].queueCount)
					continue;
				physical = adapter;
				family = candidate;
				index = families[candidate].queueCount > 1 ? 1 : 0;
				const std::array priorities{1.0f, 1.0f};
				VkDeviceQueueCreateInfo queues{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
				queues.queueFamilyIndex = family;
				queues.queueCount = index + 1;
				queues.pQueuePriorities = priorities.data();
				VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
				info.queueCreateInfoCount = 1;
				info.pQueueCreateInfos = &queues;
				Check(vkCreateDevice(physical, &info, nullptr, &native), "create feature-minimal compositor device");
				vkGetDeviceQueue(native, family, index, &queue);
				std::printf("adapter=%s queueFamily=%u queueIndex=%u optionalFeatures=none optionalExtensions=none\n",
							properties.deviceName, family, index);
				return true;
			}
		}
		return false;
	}
};

struct CommandResources
{
	VkDevice device;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkRenderPass pass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> framebuffers;
	std::vector<VkImageView> views;
	bool submitted = false;
	bool complete = false;
	explicit CommandResources(VkDevice owner) : device(owner) {}
	~CommandResources()
	{
		if (submitted && !complete)
			UnknownCompletion("probe command teardown");
		if (pool)
			vkDestroyCommandPool(device, pool, nullptr);
		for (auto framebuffer : framebuffers)
			vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (pass)
			vkDestroyRenderPass(device, pass, nullptr);
		for (auto view : views)
			vkDestroyImageView(device, view, nullptr);
		if (fence)
			vkDestroyFence(device, fence, nullptr);
	}
};
class Commands
{
  public:
	explicit Commands(Device& owner) : device(owner), resources(owner.native)
	{
		VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pool.queueFamilyIndex = device.family;
		Check(vkCreateCommandPool(device.native, &pool, nullptr, &resources.pool), "create probe command pool");
		VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocate.commandPool = resources.pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		Check(vkAllocateCommandBuffers(device.native, &allocate, &resources.command), "allocate probe commands");
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		Check(vkCreateFence(device.native, &fence, nullptr, &resources.fence), "create probe completion fence");
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkBeginCommandBuffer(resources.command, &begin), "begin probe commands");
	}
	void Finish()
	{
		Check(vkEndCommandBuffer(resources.command), "end probe commands");
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &resources.command;
		const auto result = vkQueueSubmit(device.queue, 1, &submit, resources.fence);
		if (result != VK_SUCCESS)
			UnknownCompletion("probe queue submission failed");
		resources.submitted = true;
		const auto waited = vkWaitForFences(device.native, 1, &resources.fence, VK_TRUE, Timeout);
		if (waited != VK_SUCCESS)
			UnknownCompletion("probe fence wait failed");
		resources.complete = true;
	}
	Device& device;
	CommandResources resources;
};

struct Source
{
	XrSwapchain identity;
	std::shared_ptr<VulkanImage> image;
	std::array<XrColor4f, 4> colors;
	LayerImageDescription Description(std::uint32_t slice = 0) const
	{
		return {identity, {{4, 4}, {8, 8}}, slice};
	}
};
using Palette = std::vector<std::pair<XrSwapchain, std::array<XrColor4f, 4>>>;

void Paint(Device& device, const Source& source)
{
	Commands work(device);
	auto& native = work.resources;
	const auto& image = *source.image;
	VulkanImageBarrier(native.command, image, image.ApplicationLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	const VkClearColorValue blue{{0, 0, 1, 1}};
	const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4};
	vkCmdClearColorImage(native.command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &all);
	VulkanImageBarrier(native.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.ApplicationLayout(),
					   VK_ACCESS_TRANSFER_WRITE_BIT,
					   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	VkAttachmentDescription attachment{};
	attachment.format = image.format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color;
	VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	pass.attachmentCount = 1;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1;
	pass.pSubpasses = &subpass;
	Check(vkCreateRenderPass(device.native, &pass, nullptr, &native.pass), "create GPU source-paint pass");
	native.framebuffers.reserve(4);
	native.views.reserve(4);
	for (std::uint32_t slice = 0; slice < 4; ++slice)
	{
		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view.image = image.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = image.format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slice, 1};
		VkImageView handle;
		Check(vkCreateImageView(device.native, &view, nullptr, &handle), "create source array-slice view");
		native.views.push_back(handle);
		VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebuffer.renderPass = native.pass;
		framebuffer.attachmentCount = 1;
		framebuffer.pAttachments = &native.views.back();
		framebuffer.width = framebuffer.height = 16;
		framebuffer.layers = 1;
		VkFramebuffer target;
		Check(vkCreateFramebuffer(device.native, &framebuffer, nullptr, &target),
			  "create GPU source-paint framebuffer");
		native.framebuffers.push_back(target);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = native.pass;
		begin.framebuffer = target;
		begin.renderArea.extent = {16, 16};
		vkCmdBeginRenderPass(native.command, &begin, VK_SUBPASS_CONTENTS_INLINE);
		const auto& rgba = source.colors[slice];
		VkClearAttachment clear{};
		clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		clear.clearValue.color = {{rgba.r, rgba.g, rgba.b, rgba.a}};
		const VkClearRect rectangle{{{4, 4}, {8, 8}}, 0, 1};
		vkCmdClearAttachments(native.command, 1, &clear, 1, &rectangle);
		vkCmdEndRenderPass(native.command);
	}
	work.Finish();
}

struct Readback
{
	VkDevice device;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	bool coherent = false;
	explicit Readback(VkDevice owner) : device(owner) {}
	~Readback()
	{
		if (buffer)
			vkDestroyBuffer(device, buffer, nullptr);
		if (memory)
			vkFreeMemory(device, memory, nullptr);
	}
};
std::vector<std::uint8_t> Pixels(Device& device, const VulkanFrame& frame)
{
	const auto& image = frame.Image();
	const VkDeviceSize size = static_cast<VkDeviceSize>(image.extent.width) * image.extent.height * 4 * 2;
	Readback storage(device.native);
	VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer.size = size;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	Check(vkCreateBuffer(device.native, &buffer, nullptr, &storage.buffer), "create explicit verification buffer");
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device.native, storage.buffer, &requirements);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(device.physical, &properties);
	std::optional<std::uint32_t> type;
	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
		if ((requirements.memoryTypeBits & (1u << index)) &&
			(properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
		{
			type = index;
			if (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
				break;
		}
	Require(type.has_value(), "No host-visible verification memory");
	storage.coherent = properties.memoryTypes[*type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = *type;
	Check(vkAllocateMemory(device.native, &allocate, nullptr, &storage.memory),
		  "allocate explicit verification memory");
	Check(vkBindBufferMemory(device.native, storage.buffer, storage.memory, 0), "bind verification buffer");
	Commands work(device);
	VulkanImageBarrier(work.resources.command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
	copy.imageExtent = {image.extent.width, image.extent.height, 1};
	vkCmdCopyImageToBuffer(work.resources.command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storage.buffer, 1,
						   &copy);
	VulkanImageBarrier(work.resources.command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
					   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
	VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = storage.buffer;
	host.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(work.resources.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
						 nullptr, 1, &host, 0, nullptr);
	work.Finish();
	void* mapped = nullptr;
	Check(vkMapMemory(device.native, storage.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map explicit verification pixels");
	if (!storage.coherent)
	{
		VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = storage.memory;
		range.size = VK_WHOLE_SIZE;
		Check(vkInvalidateMappedMemoryRanges(device.native, 1, &range), "invalidate noncoherent verification memory");
	}
	std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
	std::memcpy(result.data(), mapped, result.size());
	vkUnmapMemory(device.native, storage.memory);
	return result;
}

float Srgb(float value)
{
	value = std::clamp(value, 0.0f, 1.0f);
	return value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}
std::uint8_t Byte(float value)
{
	return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
}

XrColor4f Expected(const CompositionPlan& plan, const Palette& palette, std::uint32_t eye, XrVector2f uv)
{
	XrColor4f result{0, 0, 0, 1};
	for (const auto& draw : plan.draws)
	{
		if (draw.eye != eye || !EvaluateCompositionSample(draw, uv))
			continue;
		const auto source = std::find_if(palette.begin(), palette.end(),
										 [&](const auto& entry)
										 {
											 return entry.first == draw.source.source;
										 });
		Require(source != palette.end() && draw.source.arraySlice < 4, "Missing expected source color");
		result = CompositeReferenceColor(source->second[draw.source.arraySlice], result, draw.constants.control[1]);
	}
	return result;
}
void Verify(Device& device, const VulkanFrame& frame, const CompositionPlan& plan, const Palette& palette,
			const char* name)
{
	const auto pixels = Pixels(device, frame);
	for (std::uint32_t eye = 0; eye < 2; ++eye)
		for (std::uint32_t y = 0; y < plan.height; ++y)
			for (std::uint32_t x = 0; x < plan.width; ++x)
			{
				const auto rgba = Expected(plan, palette, eye, {(x + 0.5f) / plan.width, (y + 0.5f) / plan.height});
				const std::array expected{Byte(Srgb(rgba.r)), Byte(Srgb(rgba.g)), Byte(Srgb(rgba.b)), Byte(rgba.a)};
				const auto offset = ((static_cast<std::size_t>(eye) * plan.height + y) * plan.width + x) * 4;
				for (std::size_t channel = 0; channel < 4; ++channel)
					if (std::abs(static_cast<int>(pixels[offset + channel]) - expected[channel]) > 2)
					{
						std::fprintf(stderr, "%s eye=%u pixel=(%u,%u) channel=%zu actual=%u expected=%u\n", name, eye,
									 x, y, channel, pixels[offset + channel], expected[channel]);
						throw std::runtime_error("Production compositor pixel mismatch");
					}
			}
	std::printf("case=%s verify=explicit-readback pixels=%u eyes=2 tolerance=2bytes PASS\n", name,
				plan.width * plan.height);
}

XrFovf Fov(float left = -1, float right = 1, float up = 1, float down = -1)
{
	return {std::atan(left), std::atan(right), std::atan(up), std::atan(down)};
}
PoseSample Pose(XrVector3f position = {})
{
	return {{{0, 0, 0, 1}, position}, TrackedPoseFlags, {Duration{1000}}};
}
CompositionTarget Target(std::uint32_t width = 32, std::uint32_t height = 24)
{
	return {width, height, {{{Pose({-0.032f, 0, 0}), Fov()}, {Pose({0.032f, 0, 0}), Fov()}}}};
}
ProjectionLayerDescription Projection(const Source& source, std::size_t count = 2)
{
	ProjectionLayerDescription layer;
	for (std::size_t index = 0; index < count; ++index)
		layer.views.push_back(
			{IdentityPose(), Pose(), Fov(), source.Description(static_cast<std::uint32_t>(index)), {}});
	return layer;
}
QuadLayerDescription Quad(const Source& source, XrCompositionLayerFlags flags, XrEyeVisibility eyes)
{
	return {{XR_NULL_HANDLE, flags, Pose()}, eyes,			 IdentityPose(),
			Pose({0.13f, -0.07f, -2}),		 {1.73f, 1.31f}, source.Description()};
}
FrameLayers Frame(std::vector<LayerDescription> layers)
{
	return {{Duration{1000}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, std::move(layers)};
}

class Exercise
{
  public:
	Exercise(Device& native, VulkanBackend& graphics, bool analysis)
		: device(native), backend(graphics), verify(analysis)
	{
	}
	Palette Colors() const
	{
		Palette colors;
		for (const auto& source : sources)
			colors.emplace_back(source.identity, source.colors);
		return colors;
	}
	std::shared_ptr<VulkanFrame> Render(const FrameLayers& frame, const CompositionTarget& target,
										CompositionPlan& plan)
	{
		const auto extent = [&](XrSwapchain identity) -> std::optional<XrExtent2Di>
		{
			const auto source = Find(identity);
			if (!source)
				return {};
			return XrExtent2Di{static_cast<std::int32_t>(source->image->extent.width),
							   static_cast<std::int32_t>(source->image->extent.height)};
		};
		Require(PrepareComposition(frame, target, extent, plan) == XR_SUCCESS, "Prepare shared composition plan");
		std::shared_ptr<VulkanFrame> output;
		Require(backend.Prepare(
					plan,
					[&](XrSwapchain identity)
					{
						const auto source = Find(identity);
						return source ? source->image : nullptr;
					},
					output) == XR_SUCCESS,
				"Prepare production Vulkan output");
		Require(output && output->PollCompletion() == CompletionStatus::Pending,
				"Unsubmitted output reported completion");
		Require(backend.Submit(output) == XR_SUCCESS, "Submit production Vulkan composition");
		Drain();
		Require(output->PollCompletion() == CompletionStatus::Complete && output->Image().layers == 2 &&
					output->Image().extent.width == target.width && output->Image().extent.height == target.height,
				"Production output completion or dimensions invalid");
		return output;
	}
	void Drain()
	{
		if (backend.Drain(Timeout) != VK_SUCCESS)
			UnknownCompletion("production backend drain");
	}
	Source* Find(XrSwapchain identity)
	{
		const auto found = std::find_if(sources.begin(), sources.end(),
										[&](const auto& source)
										{
											return source.identity == identity;
										});
		return found == sources.end() ? nullptr : &*found;
	}
	void Add(std::uintptr_t identity, std::array<XrColor4f, 4> colors)
	{
		auto image = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 4, SourceUsage);
		auto initialized = backend.InitializeImage(image);
		if (initialized->Wait(Timeout) != VK_SUCCESS)
			UnknownCompletion("source initialization");
		Require(initialized->PollCompletion() == CompletionStatus::Complete, "Initialization ticket did not complete");
		sources.push_back({reinterpret_cast<XrSwapchain>(identity), std::move(image), colors});
		Paint(device, sources.back());
	}
	void ReservedPreparation()
	{
		backend.RetireCompleted();
		auto pool = backend.Reservations();
		Require(pool.Capacity() == 2 && pool.InUse() == 0, "Reservation baseline retained old frame budget");
		auto reservation = pool.Reserve();
		auto reservationReader = reservation;
		CompositionPlan plan;
		auto original = Render(Frame({Projection(sources[0])}), Target(), plan);
		Require(reservation && pool.InUse() == 2 && !pool.Reserve(),
				"Explicit reservation failed to protect native capacity");
		const auto resolve = [&](XrSwapchain identity)
		{
			const auto source = Find(identity);
			return source ? source->image : nullptr;
		};
		auto unchanged = original;
		Require(backend.Prepare(
					plan,
					[](XrSwapchain)
					{
						return std::shared_ptr<VulkanImage>{};
					},
					unchanged, reservation) == XR_ERROR_LAYER_INVALID &&
					unchanged == original && !reservation->Consumed() && pool.InUse() == 2,
				"Failed preparation consumed reservation or replaced output");
		FramePool foreign(1);
		auto foreignReservation = foreign.Reserve();
		Require(backend.Prepare(plan, resolve, unchanged, foreignReservation) == XR_ERROR_VALIDATION_FAILURE &&
					!foreignReservation->Consumed() && unchanged == original,
				"Foreign native reservation accepted");
		std::shared_ptr<VulkanFrame> reserved;
		Require(backend.Prepare(plan, resolve, reserved, reservation) == XR_SUCCESS && reservation->Consumed() &&
					pool.InUse() == 2,
				"Explicit preparation could not use reserved full-pool capacity");
		Require(backend.Prepare(plan, resolve, unchanged, reservation) == XR_ERROR_CALL_ORDER_INVALID &&
					unchanged == original,
				"Consumed reservation reused for a second output");
		Require(backend.Submit(reserved) == XR_SUCCESS, "Submit explicitly reserved output");
		Drain();
		if (verify)
			Verify(device, *reserved, plan, Colors(), "explicit-reservation-retry");
		unchanged.reset();
		original.reset();
		backend.RetireCompleted();
		auto completionReader = reserved->Completion();
		reserved.reset();
		reservation.reset();
		reservationReader.reset();
		backend.RetireCompleted();
		Require(pool.InUse() == 1 && completionReader->PollCompletion() == CompletionStatus::Complete,
				"Completion reader did not retain producer reservation");
		completionReader.reset();
		backend.RetireCompleted();
		Require(pool.InUse() == 0, "Final native reservation reader leaked budget");
		std::puts("case=explicit-reservation-retry foreign=rejected failedPrepare=reusable consumed=once "
				  "completionReader=retained PASS");
	}

	void PendingProducerReservation()
	{
		CompositionPlan plan;
		Require(PrepareComposition(
					Frame({Projection(sources[0])}), Target(),
					[&](XrSwapchain identity) -> std::optional<XrExtent2Di>
					{
						return Find(identity) ? std::optional{XrExtent2Di{16, 16}} : std::nullopt;
					},
					plan) == XR_SUCCESS,
				"Producer reservation composition plan");
		std::shared_ptr<VulkanFrame> output;
		Require(backend.Prepare(
					plan,
					[&](XrSwapchain identity)
					{
						const auto source = Find(identity);
						return source ? source->image : nullptr;
					},
					output) == XR_SUCCESS &&
					backend.Submit(output) == XR_SUCCESS,
				"Submit reservation producer");
		const auto observed = output->PollCompletion();
		Require(observed != CompletionStatus::Failed, "Reservation producer failed");
		auto pool = backend.Reservations();
		output.reset();
		// Even a producer that finishes quickly remains a native owner until
		// observed retirement. This does not require fabricating a GPU stall.
		Require(pool.InUse() == 1, "Submitted producer released budget with last CPU output reader");
		Drain();
		Require(pool.InUse() == 0, "Completed producer did not retire its reservation");
		std::printf(
			"case=producer-reservation outputReader=released completionObserved=%s budget=retainedUntilRetire PASS\n",
			observed == CompletionStatus::Pending ? "pending" : "complete");
	}

	void SessionWorker(bool capture = false)
	{
		auto instance = std::make_shared<Instance>(reinterpret_cast<XrInstance>(100), std::span<const char* const>{},
												   XR_MAKE_VERSION(1, 0, 27));
		auto sessionOwner = std::make_shared<Session>(reinterpret_cast<XrSession>(101), instance);
		auto& session = *sessionOwner;
		FrameSchedulerConfig timing;
		timing.period = Duration{5000000};
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		if (capture)
			Require(session.ConfigureCapturePool(backend.Reservations()) == XR_SUCCESS,
					"Configure production capture pool");
		Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"GPU worker session lifecycle");
		CaptureReservation reservation{};
		std::unique_ptr<DebugController> debugger;
		DebugLeaseId lease;
		std::uint64_t nextCommand = 1;
		auto command = [&](DebugCommand kind, DebugOperationId operation = {}, CaptureId id = {})
		{
			const auto submitted = debugger->Submit(lease, {{nextCommand++}, kind, 1, {}, operation, id});
			Require(submitted.result == DebugAdmission::Accepted, "Production debug command admission");
			return submitted.operation;
		};
		auto await = [&](DebugOperationId id)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			for (;;)
			{
				const auto operation = debugger->Read(id);
				Require(operation && operation->phase != DebugPhase::Failed &&
							operation->phase != DebugPhase::Interrupted && operation->phase != DebugPhase::Cancelled,
						"Production debug operation failed");
				if (operation->phase == DebugPhase::Complete)
					return *operation;
				Require(std::chrono::steady_clock::now() < deadline, "Production debug operation did not settle");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		};
		auto admit = [&](DebugOperationId operation)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			for (;;)
			{
				const auto status = debugger->Read(operation);
				Require(status && status->phase != DebugPhase::Failed, "Production capture reservation failed");
				if (!status->captures.empty() && session.Scheduler().Snapshot().permits)
					return status->captures.front();
				Require(std::chrono::steady_clock::now() < deadline, "Production capture did not reserve and grant");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		};
		DebugOperationId inspection;
		if (capture)
		{
			debugger = std::make_unique<DebugController>(sessionOwner);
			lease = debugger->Acquire()->id;
			Require(await(command(DebugCommand::Pause)).progress == DebugProgress::Paused, "Production debug pause");
			inspection = command(DebugCommand::Step);
			reservation = {SnapshotAdmission::Accepted, admit(inspection)};
		}
		FramePrediction prediction;
		Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
				"GPU worker issue/begin");
		XrSpace stage{};
		Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), stage) ==
					XR_SUCCESS,
				"GPU worker reference space");
		std::array<XrCompositionLayerProjectionView, 2> views{};
		for (std::uint32_t eye = 0; eye < 2; ++eye)
		{
			views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			views[eye].pose = IdentityPose();
			views[eye].fov = Fov();
			const auto image = sources[0].Description(eye);
			views[eye].subImage = {image.source, image.rectangle, image.arraySlice};
		}
		XrCompositionLayerProjection projection{
			XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, stage, 2, views.data()};
		const XrCompositionLayerBaseHeader* layers[] = {
			reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
		XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			  nullptr, prediction.displayTime.value.count(),
							XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,	   layers};
		LayerValidationContext validation;
		validation.validSpace = [&](XrSpace space)
		{
			return session.Spaces().Owns(space);
		};
		validation.swapchain = [&](XrSwapchain identity) -> std::optional<SwapchainDescription>
		{
			return Find(identity) ? std::optional{SwapchainDescription{16, 16, 4, true}} : std::nullopt;
		};
		std::shared_ptr<const FrameLayers> prepared;
		Require(BuildLayers(
					info, validation,
					[&](XrSpace space, RuntimeTime time, SpaceSample& pose)
					{
						return session.Spaces().Sample(space, time, pose);
					},
					prepared) == XR_SUCCESS,
				"GPU worker metadata preparation");
		CompositionPlan plan;
		Require(PrepareComposition(
					*prepared, Target(),
					[&](XrSwapchain identity) -> std::optional<XrExtent2Di>
					{
						return Find(identity) ? std::optional{XrExtent2Di{16, 16}} : std::nullopt;
					},
					plan) == XR_SUCCESS,
				"GPU worker composition plan");
		std::shared_ptr<VulkanFrame> output;
		Require(backend.Prepare(
					plan,
					[&](XrSwapchain identity)
					{
						const auto source = Find(identity);
						return source ? source->image : nullptr;
					},
					output,
					capture
						? session.Captures().ProducerReservation(FrameKey{session.Snapshot().generation, prediction.id})
						: nullptr) == XR_SUCCESS,
				"GPU worker private output preparation");
		Require(output->PollCompletion() == CompletionStatus::Pending, "Unsubmitted worker output completed");
		Require(session.EndFrame(prediction, &info, prepared, output,
								 [&]
								 {
									 return backend.Submit(output);
								 }) == XR_SUCCESS,
				"GPU worker transactional submission");
		const auto key = FrameKey{session.Snapshot().generation, prediction.id};
		std::optional<SnapshotRecord> retainedCapture;
		if (capture)
		{
			const auto completed = await(inspection);
			Require(completed.progress == DebugProgress::SnapshotReady && completed.capture == reservation.id,
					"Production controller did not observe actual GPU snapshot readiness");
			retainedCapture = session.Captures().Read(reservation.id);
			Require(retainedCapture && retainedCapture->phase == SnapshotPhase::Ready,
					"Production controller selected missing retained snapshot");
			Require(retainedCapture->frame == key && retainedCapture->output.get() == output.get() &&
						retainedCapture->metadata && retainedCapture->metadata->stage == FrameStage::Submitted &&
						retainedCapture->metadata->submission &&
						retainedCapture->metadata->submission->layers == prepared,
					"Capture did not retain exact GPU output and immutable frame metadata");
			views[0].subImage.imageArrayIndex = 3;
			projection.viewCount = 0;
			Require(std::get<ProjectionLayerDescription>(retainedCapture->metadata->submission->layers->layers.at(0))
							.views.at(0)
							.color.arraySlice == 0,
					"Capture metadata retained application layer memory");
			while (session.Display().NextTick() <= prediction.displayTick)
			{
				const auto displayed = await(command(DebugCommand::DisplayStep));
				Require(displayed.capture.has_value(), "Controller display snapshot missing");
				await(command(DebugCommand::Release, {}, *displayed.capture));
			}
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (;;)
		{
			const auto state = session.Display().Snapshot();
			if (state.latest && state.latest->source == key)
			{
				Require(state.latest->output.get() == output.get() &&
							output->PollCompletion() == CompletionStatus::Complete,
						"Display selected output without actual producer completion");
				break;
			}
			Require(!session.Display().Faulted() && std::chrono::steady_clock::now() < deadline,
					"Real-time worker did not select GPU output without more XR calls");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (verify)
			Verify(device, *output, plan, Colors(), "core-session-worker-real-fence");
		if (capture)
		{
			Require(await(command(DebugCommand::Release, {}, reservation.id)).progress == DebugProgress::Released &&
						!session.Captures().Read(reservation.id),
					"Logical controller release retained lookup");
			const auto oldColors = Colors();
			const auto savedSourceColors = sources[0].colors;
			sources[0].colors = {{{1, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}}};
			Paint(device, sources[0]);
			if (verify)
				Verify(device, *output, plan, oldColors, "capture-reader-after-release-source-reuse");
			sources[0].colors = savedSourceColors;
			Paint(device, sources[0]);
			auto pool = backend.Reservations();
			Require(pool.InUse() == 1, "Released capture lost retained GPU reservation");
			const auto emptyOperation = command(DebugCommand::Step);
			const auto empty = admit(emptyOperation);
			Require(pool.InUse() == 2, "Controller did not reserve zero-layer capture before admission");
			FramePrediction clear;
			Require(session.WaitFrame(clear) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
						session.EndFrame(clear) == XR_SUCCESS,
					"Submit zero-layer captured frame");
			{
				Require(await(emptyOperation).progress == DebugProgress::Clear, "Controller clear snapshot status");
				const auto cleared = session.Captures().Read(empty);
				Require(cleared && cleared->phase == SnapshotPhase::Clear && !cleared->output && pool.InUse() == 1,
						"Zero-layer capture did not return unused GPU slot");
			}
			await(command(DebugCommand::Release, {}, empty));
			while (session.Display().NextTick() <= clear.displayTick)
			{
				const auto displayed = await(command(DebugCommand::DisplayStep));
				Require(displayed.capture.has_value(), "Controller clear display snapshot missing");
				await(command(DebugCommand::Release, {}, *displayed.capture));
			}
			{
				const auto display = session.Display().Snapshot();
				Require(display.latest && display.latest->kind == PresentationKind::Clear && !display.latest->output,
						"Eligible clear retained selected GPU source");
			}
			Require(debugger->Acknowledge(lease, {nextCommand - 1}), "Acknowledge GPU display command readers");
			output.reset();
			Drain();
			Require(pool.InUse() == 1, "Last SnapshotRecord reader failed to retain GPU slot");
			retainedCapture.reset();
			backend.RetireCompleted();
			Require(pool.InUse() == 0, "Released SnapshotRecord reader leaked GPU slot");
			const auto queuedOperation = command(DebugCommand::Step);
			const auto queued = admit(queuedOperation);
			const auto before = session.Scheduler().Snapshot();
			await(command(DebugCommand::Cancel, queuedOperation));
			Require(debugger->Read(queuedOperation)->phase == DebugPhase::Cancelled && pool.InUse() == 0 &&
						!session.Captures().Read(queued) && !session.Scheduler().Snapshot().permits &&
						session.Scheduler().Snapshot().nextAdmission == before.nextAdmission,
					"Controller cancellation consumed admission or leaked capacity");
		}
		session.Close();
		if (debugger)
			debugger->Close(false);
		output.reset();
		Drain();
		session.Captures().Poll();
		Require(backend.Reservations().InUse() == 0, "Closed session retained capture GPU capacity");
		std::puts(capture ? "case=debug-controller-capture realFence=ready metadata=immutable reader=retained "
							"empty=clear unused=cancelled explicitDisplayStep=selected PASS"
						  : "case=core-session-worker-real-fence XRcallsAfterEndFrame=none producer=complete "
							"selection=independent PASS");
	}
	void Run()
	{
		Add(1, {{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 1, 1}}});
		const XrColor4f orange{0.8f, 0.4f, 0.2f, 0.25f};
		const XrColor4f premultiplied{0.2f, 0.1f, 0.05f, 0.25f};
		Add(2, {orange, orange, orange, orange});
		Add(3, {premultiplied, premultiplied, premultiplied, premultiplied});
		backend.RetireCompleted();
		CompositionPlan plan;
		{
			auto output = Render(Frame({Projection(sources[0])}), Target(), plan);
			if (verify)
				Verify(device, *output, plan, Colors(), "projection-crop-array");
			const auto left = Expected(plan, Colors(), 0, {0.5f, 0.5f});
			const auto right = Expected(plan, Colors(), 1, {0.5f, 0.5f});
			Require(left.r == 1 && left.g == 0 && right.r == 0 && right.g == 1, "Known stereo red/green anchors lost");
			std::puts("case=projection-crop-array producer=complete private=2eye-GPU PASS");
		}
		{
			auto straight = Quad(sources[1],
								 XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
									 XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
								 XR_EYE_VISIBILITY_BOTH);
			auto premult =
				Quad(sources[2], XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, XR_EYE_VISIBILITY_RIGHT);
			premult.size = {0.91f, 0.73f};
			auto output = Render(Frame({Projection(sources[0]), straight, premult}), Target(), plan);
			if (verify)
				Verify(device, *output, plan, Colors(), "ordered-linear-alpha-eyes");
			std::puts("case=ordered-linear-alpha-eyes producer=complete PASS");
		}
		{
			auto quad = Quad(sources[1], 0, XR_EYE_VISIBILITY_BOTH);
			quad.worldPose.pose.orientation = EulerAngles(0.25f, 0.08f, 0.1f);
			auto target = Target();
			target.eyes[0].fov = Fov(-0.8f, 1.1f, 0.9f, -0.6f);
			target.eyes[1].fov = Fov(-1.1f, 0.8f, 0.7f, -0.9f);
			auto output = Render(Frame({quad}), target, plan);
			if (verify)
				Verify(device, *output, plan, Colors(), "quad-only-rotated-asymmetric");
			std::puts("case=quad-only-rotated-asymmetric producer=complete PASS");
		}
		{
			auto projection = Projection(sources[0]);
			projection.views[0].worldPose.pose.orientation = EulerAngles(0.19f, 0.03f, 0);
			projection.views[1].worldPose.pose.orientation = EulerAngles(-0.12f, -0.07f, 0);
			projection.views[0].fov = Fov(-0.7f, 1.3f, 0.6f, -0.9f);
			projection.views[1].fov = Fov(-1.2f, 0.65f, 0.8f, -0.55f);
			auto output = Render(Frame({projection}), Target(), plan);
			if (verify)
				Verify(device, *output, plan, Colors(), "projection-rotated-asymmetric");
			std::puts("case=projection-rotated-asymmetric producer=complete PASS");
		}
		{
			auto inset = Projection(sources[0], 4);
			inset.views[2].fov = Fov(-0.37f, 0.31f, 0.29f, -0.33f);
			inset.views[3].fov = Fov(-0.29f, 0.41f, 0.35f, -0.27f);
			auto output = Render(Frame({inset}), Target(), plan);
			if (verify)
				Verify(device, *output, plan, Colors(), "four-view-outer-inset");
			const auto left = Expected(plan, Colors(), 0, {0.5f, 0.5f});
			const auto right = Expected(plan, Colors(), 1, {0.5f, 0.5f});
			Require(left.g == 1 && left.b == 1 && right.r == 1 && right.b == 1,
					"Known cyan/magenta inset anchors lost");
			std::puts("case=four-view-outer-inset producer=complete PASS");
		}
		ReservedPreparation();
		PendingProducerReservation();
		SessionWorker();
		SessionWorker(true);
		CompositionPlan oldPlan;
		auto retained = Render(Frame({Projection(sources[0])}), Target(), oldPlan);
		const auto oldColors = Colors();
		const auto oldImage = retained->Image().image;
		sources[0].colors = {{{1, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}}};
		Paint(device, sources[0]);
		auto resized = Render(Frame({Projection(sources[0])}), Target(40, 30), plan);
		Require(resized->Image().image != oldImage && retained->Image().extent.width == 32 &&
					resized->Image().extent.width == 40,
				"Resize aliased or mutated retained private output");
		if (verify)
		{
			Verify(device, *retained, oldPlan, oldColors, "retained-after-source-reuse-resize");
			Verify(device, *resized, plan, Colors(), "new-source-resized-output");
		}
		std::shared_ptr<VulkanFrame> rejected;
		const auto resolve = [&](XrSwapchain identity)
		{
			const auto source = Find(identity);
			return source ? source->image : nullptr;
		};
		Require(backend.Prepare(plan, resolve, rejected) == XR_ERROR_LIMIT_REACHED && !rejected,
				"Third retained output exceeded capacity two");
		resized.reset();
		backend.RetireCompleted();
		resized = Render(Frame({Projection(sources[0])}), Target(40, 30), plan);
		if (verify)
			Verify(device, *resized, plan, Colors(), "capacity-release-reuse");
		retained.reset();
		sources.clear();
		backend.RetireCompleted();
		Require(backend.Close() == XR_ERROR_LIMIT_REACHED, "Close accepted retained frame reader");
		resized.reset();
		backend.RetireCompleted();
		auto closureImage = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 1, SourceUsage);
		auto ticket = backend.InitializeImage(closureImage);
		if (ticket->Wait(Timeout) != VK_SUCCESS)
			UnknownCompletion("closure initialization");
		closureImage.reset();
		backend.RetireCompleted();
		Require(backend.Close() == XR_ERROR_LIMIT_REACHED, "Close accepted retained initialization ticket");
		ticket.reset();
		backend.RetireCompleted();
		closureImage = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 1, SourceUsage);
		Require(backend.Close() == XR_ERROR_LIMIT_REACHED, "Close accepted standalone source image reader");
		closureImage.reset();
		Require(backend.Close() == XR_SUCCESS, "Close did not drain released native owners");
		std::puts("case=retention-resize-capacity-close privateOutput=stable readers=required-released PASS");
	}
	Device& device;
	VulkanBackend& backend;
	bool verify;
	std::vector<Source> sources;
};

void RejectOversizedImage(const VulkanBinding& binding)
{
	VkImageFormatProperties supported{};
	Check(vkGetPhysicalDeviceImageFormatProperties(binding.physicalDevice, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TYPE_2D,
												   VK_IMAGE_TILING_OPTIMAL, SourceUsage, 0, &supported),
		  "query source image limits");
	Require(supported.maxExtent.width < std::numeric_limits<std::uint32_t>::max(),
			"Cannot construct an extent above reported image width limit");
	const auto errorsBefore = simxr::probe::s_validationErrors.load();
	{
		VulkanBackend backend(binding, 1);
		bool rejected = false;
		try
		{
			auto invalid =
				backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {supported.maxExtent.width + 1, 16}, 1, SourceUsage);
			(void)invalid;
		}
		catch (const VulkanError& error)
		{
			rejected = error.Result() == VK_ERROR_FORMAT_NOT_SUPPORTED;
		}
		Require(rejected, "Oversized native image was not rejected with VK_ERROR_FORMAT_NOT_SUPPORTED");
		Require(backend.Close() == XR_SUCCESS, "Rejected image leaked backend resource ownership");
	}
	Require(simxr::probe::s_validationErrors.load() == errorsBefore, "Oversized image reached invalid native creation");
	std::printf("case=oversized-image-preflight maxWidth=%u rejectedWidth=%u result=FORMAT_NOT_SUPPORTED "
				"ownership=clean PASS\n",
				supported.maxExtent.width, supported.maxExtent.width + 1);
}

bool ClosedDepthPipeline(const simxr::probe::Instance& instance)
{
	std::uint32_t count = 0;
	Check(vkEnumerateInstanceLayerProperties(&count, nullptr), "query depth closure validation");
	std::vector<VkLayerProperties> layers(count);
	Check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "query depth closure validation");
	if (!instance.messenger || std::none_of(layers.begin(), layers.end(),
											[](const auto& layer)
											{
												return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
											}))
	{
		std::puts("SKIP: depth pipeline closure requires Vulkan validation");
		return false;
	}
	Device device;
	if (!device.Create(instance.handle))
	{
		std::puts("SKIP: no Vulkan1.1 device for depth pipeline closure");
		return false;
	}
	constexpr auto usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	for (const auto candidate : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM})
	{
		VkImageFormatProperties properties{};
		if (vkGetPhysicalDeviceImageFormatProperties(device.physical, candidate, VK_IMAGE_TYPE_2D,
													 VK_IMAGE_TILING_OPTIMAL, usage, 0, &properties) != VK_SUCCESS)
			continue;
		for (const auto sampleCount : {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT})
			if (properties.sampleCounts & sampleCount)
			{
				format = candidate;
				samples = sampleCount;
				break;
			}
		if (format != VK_FORMAT_UNDEFINED)
			break;
	}
	if (format == VK_FORMAT_UNDEFINED)
	{
		std::puts("SKIP: sampled multisample depth unavailable for pipeline closure");
		return false;
	}
	const auto errorsBefore = simxr::probe::s_validationErrors.load();
	const VulkanBinding binding{instance.handle, device.physical, device.native, device.family, device.index};
	auto backend = std::make_unique<VulkanBackend>(binding, 1);
	auto image = backend->CreateImage(format, {8, 8}, 1, usage, 1, samples);
	const LayerDepthDescription depth{{reinterpret_cast<XrSwapchain>(902), {{0, 0}, {8, 8}}, 0}, 0, 1, 0.1f, 100};
	std::shared_ptr<VulkanFrame> frame;
	Require(backend->PrepareDepthCapture(
				depth, false,
				[&](XrSwapchain)
				{
					return image;
				},
				frame) == XR_SUCCESS,
			"Prepare multisample depth pipeline closure");
	frame.reset();
	image.reset();
	Require(backend->Close() == XR_SUCCESS, "Multisample depth backend did not close");
	vkDestroyDevice(std::exchange(device.native, VK_NULL_HANDLE), nullptr);
	if (simxr::probe::s_validationErrors.load() != errorsBefore)
	{
		std::fputs("FAIL: successful Close retained native depth pipeline resources at device teardown\n", stderr);
		std::fflush(nullptr);
		std::_Exit(1); // Do not run a stale native destructor against the destroyed device.
	}
	backend.reset();
	std::puts("case=depth-pipeline-close nativeObjects=retired-before-borrowed-device backend=still-alive PASS");
	return true;
}

void ReservedClosure(const VulkanBinding& binding)
{
	VulkanBackend backend(binding, 1);
	auto pool = backend.Reservations();
	auto reservation = pool.Reserve();
	auto reader = reservation;
	pool.CloseAdmission();
	Require(reservation && !pool.Reserve() && !reservation->Consumed(),
			"Pool closure left new reservation admission open");
	CompositionPlan clear{16, 16, {}};
	std::shared_ptr<VulkanFrame> output;
	Require(backend.Prepare(
				clear,
				[](XrSwapchain)
				{
					return std::shared_ptr<VulkanImage>{};
				},
				output, reservation) == XR_SUCCESS &&
				reservation->Consumed() && backend.Submit(output) == XR_SUCCESS,
			"Admission closure invalidated already reserved production work");
	if (backend.Drain(Timeout) != VK_SUCCESS)
		UnknownCompletion("reserved closure drain");
	output.reset();
	reservation.reset();
	backend.RetireCompleted();
	Require(pool.InUse() == 1 && backend.Close() == XR_SUCCESS,
			"A capacity-only reader prevented closure after every borrowed resource was released");
	Require(backend.Prepare(
				clear,
				[](XrSwapchain)
				{
					return std::shared_ptr<VulkanImage>{};
				},
				output, reader) == XR_ERROR_SESSION_LOST,
			"Closed backend accepted production from a retained capacity token");
	reader.reset();
	Require(pool.InUse() == 0 && backend.Close() == XR_SUCCESS && !pool.Reserve(),
			"Closed reservation budget did not release cleanly");
	std::puts("case=reservation-close admission=closed existingToken=valid lastReader=required PASS");
}

void CompletedProducerReclamation(const VulkanBinding& binding)
{
	VulkanBackend backend(binding, 1);
	auto pool = backend.Reservations();
	CompositionPlan plan{8, 8, {}};
	std::shared_ptr<VulkanFrame> output;
	Require(backend.Prepare(
				plan,
				[](XrSwapchain)
				{
					return std::shared_ptr<VulkanImage>{};
				},
				output) == XR_SUCCESS &&
				backend.Submit(output) == XR_SUCCESS,
			"Submit capacity-one GPU producer");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		const auto completion = output->PollCompletion();
		Require(completion != CompletionStatus::Failed, "Capacity-one producer failed");
		if (completion == CompletionStatus::Complete)
			break;
		if (std::chrono::steady_clock::now() >= deadline)
			UnknownCompletion("capacity-one GPU producer observation");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	output.reset();
	Require(pool.InUse() == 1, "Capacity-one producer did not retain its native reservation");
	// The control-side reservation itself observes retirement. No Drain,
	// RetireCompleted, Prepare or further application/XR call prunes the owner.
	auto reservation = std::async(std::launch::async,
								  [pool]
								  {
									  return pool.Reserve();
								  });
	Require(reservation.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
			"Control-side reservation blocked while reclaiming an already complete producer");
	auto replacement = reservation.get();
	Require(replacement && pool.Owns(replacement) && !replacement->Consumed() && pool.InUse() == 1,
			"Completed producer-only owner stranded capacity without another XR call");
	replacement.reset();
	Require(pool.InUse() == 0 && backend.Close() == XR_SUCCESS, "Reclaimed producer did not close cleanly");
	std::puts("case=capacity-one-reclamation realFence=complete userReaders=released nextReserve=accepted "
			  "XRcalls=none drainBeforeReserve=none pixelReadback=none PASS");
}

void ExplicitReadbackOwnership(Device& device, const VulkanBinding& binding, bool verify)
{
	VulkanBackend backend(binding, 1);
	{
		Source source{reinterpret_cast<XrSwapchain>(901),
					  backend.CreateImage(VK_FORMAT_R8G8B8A8_UNORM, {16, 16}, 4, SourceUsage),
					  {{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}}}};
		auto initialized = backend.InitializeImage(source.image);
		if (initialized->Wait(Timeout) != VK_SUCCESS)
			UnknownCompletion("explicit readback source initialization");
		Paint(device, source);
		const VulkanReadbackRegion first{source.image, {4, 4}, {8, 8}, 0};
		auto invalid = first;
		invalid.layer = 4;
		for (unsigned attempt = 0; attempt < 12; ++attempt)
		{
			bool rejected = false;
			try
			{
				VulkanReadbackBatch::Prepare(backend.Device(), std::array{first, invalid});
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}
			Require(rejected, "Out-of-range readback layer survived partial native acquisition");
		}
		std::vector<std::shared_ptr<VulkanReadbackBatch>> retained;
		for (unsigned slot = 0; slot < 4; ++slot)
			retained.push_back(VulkanReadbackBatch::Prepare(backend.Device(), std::array{first}));
		bool full = false;
		try
		{
			VulkanReadbackBatch::Prepare(backend.Device(), std::array{first});
		}
		catch (const VulkanError& error)
		{
			full = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
		}
		Require(full, "Explicit readback exceeded its four charged native owners");
		retained.clear();
		auto lastLayer = first;
		lastLayer.layer = 3;
		auto readback = VulkanReadbackBatch::Prepare(backend.Device(), std::array{first, lastLayer});
		bool gated = false;
		try
		{
			readback->ReadPixels();
		}
		catch (const std::logic_error&)
		{
			gated = true;
		}
		Require(gated && readback->NativeIdle() && readback->PollCompletion() == CompletionStatus::Pending,
				"Unsubmitted readback exposed pixels or claimed completion");
		Check(readback->Submit(), "submit explicit source-region readback");
		Require(readback->Submit() == VK_ERROR_INITIALIZATION_FAILED,
				"Explicit readback submitted the same work twice");
		if (readback->Wait(Timeout) != VK_SUCCESS)
			UnknownCompletion("explicit source-region readback");
		Require(readback->NativeIdle() && readback->PollCompletion() == CompletionStatus::Complete,
				"Actual readback fence did not publish completion");
		if (verify)
		{
			const auto pixels = readback->ReadPixels();
			const std::array<std::array<std::uint8_t, 4>, 2> expected{{{255, 0, 0, 255}, {255, 255, 255, 255}}};
			Require(pixels.size() == expected.size(), "Readback lost a requested region");
			for (std::size_t region = 0; region < pixels.size(); ++region)
			{
				Require(pixels[region].size() == 8 * 8 * 4, "Readback changed the requested rectangle size");
				for (std::size_t offset = 0; offset < pixels[region].size(); offset += 4)
					Require(
						std::equal(expected[region].begin(), expected[region].end(), pixels[region].begin() + offset),
						"Readback changed exact region/layer pixels");
			}
		}
	}
	Require(backend.Close() == XR_SUCCESS, "Explicit readback retained borrowed native resources after retirement");
	std::puts("case=explicit-readback-owner partialRollback=12 capacity=4 reused=true repeatedSubmit=rejected PASS");
}

int Run(bool verify)
{
	simxr::probe::Instance instance;
	instance.Create();
	Device device;
	if (!device.Create(instance.handle))
	{
		std::puts("SKIP: no Vulkan1.1 graphics device");
		return 77;
	}
	const VulkanBinding binding{instance.handle, device.physical, device.native, device.family, device.index};
	const bool depthClosure = ClosedDepthPipeline(instance);
	RejectOversizedImage(binding);
	ReservedClosure(binding);
	CompletedProducerReclamation(binding);
	ExplicitReadbackOwnership(device, binding, verify);
	VulkanBackend backend(binding, 2);
	try
	{
		Exercise exercise(device, backend, verify);
		exercise.Run();
	}
	catch (...)
	{
		if (backend.Drain(Timeout) != VK_SUCCESS)
			UnknownCompletion("exception cleanup drain");
		throw;
	}
	Require(backend.Close() == XR_SUCCESS, "Final backend Close failed");
	return depthClosure ? 0 : 77;
}
} // namespace

int main(int argc, char** argv)
{
	bool verify = false;
	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--verify-pixels") == 0)
			verify = true;
		else
		{
			std::fprintf(stderr, "Unknown argument: %s\n", argv[index]);
			return 1;
		}
	}
	try
	{
		const int result = Run(verify);
		Require(simxr::probe::s_validationErrors.load() == 0, "Validation errors after native teardown");
		if (!result)
			std::printf("Production Vulkan compositor PASS pixelReadback=%s validationErrors=0\n",
						verify ? "explicit-analysis" : "none");
		return result;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
