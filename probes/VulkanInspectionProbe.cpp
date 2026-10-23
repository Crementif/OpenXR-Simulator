#include "Pch.h"
#include "graphics/ColorPixels.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "Math.h"
#include "VulkanImageDeliveryProbe.h"
#include "VulkanProbeInstance.h"
#include "VulkanReprojectionProbe.h"
#include "VulkanRetentionProbe.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include "presentation/DebugController.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace simxr;
using simxr::probe::Check;
constexpr std::uint64_t Timeout = 5000000000ULL;
constexpr VkImageUsageFlags SourceUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
										  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr std::array SharingExtensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
									   VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};

void Require(bool value, const char* operation)
{
	if (!value)
		throw std::runtime_error(operation);
}

template <class Predicate> void Await(Predicate predicate, const char* operation)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate())
	{
		Require(std::chrono::steady_clock::now() < deadline, operation);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void AwaitComplete(const GpuCompletion& completion)
{
	Await(
		[&]
		{
			const auto status = completion.PollCompletion();
			Require(status != CompletionStatus::Failed, "Production inspection GPU completion failed");
			return status == CompletionStatus::Complete;
		},
		"Production inspection GPU completion timed out");
}

void VerifyOwnedDeviceAcquisition(const VulkanExternalIdentity& identity,
								  const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	const auto before = validation->liveOwnedDevices.load();
	auto mismatch = identity;
	mismatch.deviceUuid[0] ^= 0xff;
	bool rejected = false;
	try
	{
		VulkanInspectionDevice invalid(mismatch, validation);
	}
	catch (const VulkanError& error)
	{
		rejected = error.Result() == VK_ERROR_INCOMPATIBLE_DRIVER;
	}
	Require(rejected && validation->liveOwnedDevices.load() == before,
			"Failed UUID device acquisition retained a native instance or validation owner");
	std::shared_ptr<VulkanDevice> retained;
	{
		VulkanInspectionDevice acquisition(identity, validation);
		retained = acquisition.Device();
		Require(retained && acquisition.EnabledExtensions().size() >= 2 &&
					validation->liveOwnedDevices.load() == before + 1,
				"Owned inspection acquisition did not publish its actual device lifetime");
	}
	Require(validation->liveOwnedDevices.load() == before + 1,
			"Destroying the acquisition facade prematurely destroyed a retained native device");
	VkFence fence = VK_NULL_HANDLE;
	VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	Check(vkCreateFence(retained->binding.device, &create, nullptr, &fence), "create fence on retained owned device");
	vkDestroyFence(retained->binding.device, fence, nullptr);
	retained.reset();
	Require(validation->liveOwnedDevices.load() == before,
			"Final owned-device reader did not complete native teardown");
	std::puts("case=owned-device-acquisition uuidMismatchRollback=yes retainedDeviceLifetime=yes PASS");
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
		Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate inspection adapters");
		std::vector<VkPhysicalDevice> adapters(count);
		Check(vkEnumeratePhysicalDevices(instance, &count, adapters.data()), "enumerate inspection adapters");
		VulkanExternalImageSpec spec;
		spec.extent = {32, 24, 1};
		for (auto adapter : adapters)
		{
			const auto caps = VulkanExternalDevice::Query(adapter, spec, VK_API_VERSION_1_1);
			if (!caps.CanExportImage() || !caps.CanImportImage() || !caps.CanExportSemaphore() ||
				!caps.CanImportSemaphore())
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
				info.enabledExtensionCount = SharingExtensions.size();
				info.ppEnabledExtensionNames = SharingExtensions.data();
				Check(vkCreateDevice(physical, &info, nullptr, &native), "create inspection producer device");
				vkGetDeviceQueue(native, family, index, &queue);
				VkPhysicalDeviceProperties properties;
				vkGetPhysicalDeviceProperties(physical, &properties);
				std::printf("adapter=%s queueFamily=%u queueIndex=%u optionalFeatures=none externalFD=enabled\n",
							properties.deviceName, family, index);
				return true;
			}
		}
		return false;
	}
};

// Probe-owned GPU source generation. Unknown completion must not free pending
// command/image owners. This exceptional process policy is confined to the probe.
struct Commands
{
	Device& device;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool submitted = false;
	bool complete = false;
	explicit Commands(Device& owner) : device(owner)
	{
		VkCommandPoolCreateInfo create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		create.queueFamilyIndex = device.family;
		Check(vkCreateCommandPool(device.native, &create, nullptr, &pool), "create source paint pool");
		VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocate.commandPool = pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		Check(vkAllocateCommandBuffers(device.native, &allocate, &command), "allocate source paint commands");
		VkFenceCreateInfo signal{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		Check(vkCreateFence(device.native, &signal, nullptr, &fence), "create source paint fence");
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkBeginCommandBuffer(command, &begin), "begin source paint");
	}
	~Commands()
	{
		if (submitted && !complete)
		{
			std::fputs("FAIL: probe source paint completion unknown; preserving GPU resources\n", stderr);
			std::fflush(nullptr);
			std::_Exit(1);
		}
		if (fence)
			vkDestroyFence(device.native, fence, nullptr);
		if (pool)
			vkDestroyCommandPool(device.native, pool, nullptr);
	}
	void Finish()
	{
		Check(vkEndCommandBuffer(command), "finish source paint");
		VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		info.commandBufferCount = 1;
		info.pCommandBuffers = &command;
		Check(vkQueueSubmit(device.queue, 1, &info, fence), "submit source paint");
		submitted = true;
		Check(vkWaitForFences(device.native, 1, &fence, VK_TRUE, Timeout), "wait source paint fence");
		complete = true;
	}
};

using EyeColors = std::array<std::array<std::uint8_t, 4>, 2>;
constexpr EyeColors Original{{{{255, 0, 0, 255}}, {{0, 255, 0, 255}}}};
constexpr EyeColors Reused{{{{255, 255, 0, 255}}, {{0, 255, 255, 255}}}};

void Paint(Device& device, const VulkanImage& image, const EyeColors& colors)
{
	Commands work(device);
	VulkanImageBarrier(work.command, image, image.ApplicationLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	for (std::uint32_t eye = 0; eye < 2; ++eye)
	{
		VkClearColorValue color{};
		for (std::uint32_t channel = 0; channel < 4; ++channel)
			color.float32[channel] = static_cast<float>(colors[eye][channel]) / 255;
		const VkImageSubresourceRange slice{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
		vkCmdClearColorImage(work.command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &slice);
	}
	VulkanImageBarrier(work.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.ApplicationLayout(),
					   VK_ACCESS_TRANSFER_WRITE_BIT,
					   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	work.Finish();
}

PoseSample Pose()
{
	return {IdentityPose(), TrackedPoseFlags, {Duration{1}}};
}
XrFovf Fov()
{
	const auto angle = std::atan(1.0f);
	return {-angle, angle, angle, -angle};
}

std::shared_ptr<const FrameLayers> Layers(XrSwapchain source, RuntimeTime time = {Duration{1}})
{
	ProjectionLayerDescription layer;
	layer.space.worldPose = Pose();
	for (std::uint32_t eye = 0; eye < 2; ++eye)
		layer.views.push_back({IdentityPose(), Pose(), Fov(), {source, {{0, 0}, {16, 16}}, eye}, {}});
	return std::make_shared<const FrameLayers>(time, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
											   std::vector<LayerDescription>{std::move(layer)});
}

CompositionPlan Plan(const FrameLayers& layers, std::uint32_t width, std::uint32_t height)
{
	const CompositionTarget target{width, height, {{{Pose(), Fov()}, {Pose(), Fov()}}}};
	CompositionPlan plan;
	Require(PrepareComposition(
				layers, target,
				[](XrSwapchain)
				{
					return XrExtent2Di{16, 16};
				},
				plan) == XR_SUCCESS,
			"Prepare shared inspection composition");
	return plan;
}

class DebugCapture
{
  public:
	explicit DebugCapture(VulkanBackend& backend, CaptureTarget target = {})
	{
		auto instance = std::make_shared<Instance>(reinterpret_cast<XrInstance>(100), std::span<const char* const>{},
												   XR_MAKE_VERSION(1, 0, 27));
		session = std::make_shared<Session>(reinterpret_cast<XrSession>(101), instance);
		Require(session->ConfigureCapturePool(backend.Reservations(), true) == XR_SUCCESS,
				"Configure owned capture budget");
		FrameSchedulerConfig timing;
		timing.period = Duration{5000000};
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
				"Start owned inspection capture session");
		controller = std::make_unique<DebugController>(session);
		const auto acquired = controller->Acquire();
		Require(acquired.has_value(), "Acquire production debug lease");
		lease = acquired->id;
		Require(AwaitOperation(Command(DebugCommand::Pause)).progress == DebugProgress::Paused,
				"Pause production capture session");
		step = Command(DebugCommand::Step, {}, target);
		Await(
			[&]
			{
				const auto operation = controller->Read(step);
				Require(operation && operation->phase != DebugPhase::Failed, "Reserve inspection capture failed");
				if (operation->captures.empty() || !session->Scheduler().Snapshot().permits)
					return false;
				capture = operation->captures.front();
				return true;
			},
			"Reserve inspection capture timed out");
		Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
				"Issue exactly one inspection capture frame");
	}
	~DebugCapture()
	{
		session->Close();
		controller->Close(false);
	}
	DebugOperationId Command(DebugCommand kind, CaptureId id = {}, CaptureTarget target = {})
	{
		const auto result = controller->Submit(lease, {{next++}, kind, 1, target, {}, id});
		Require(result.result == DebugAdmission::Accepted, "Submit owned inspection debug command");
		return result.operation;
	}
	DebugOperation AwaitOperation(DebugOperationId id)
	{
		DebugOperation result;
		Await(
			[&]
			{
				const auto operation = controller->Read(id);
				Require(operation && operation->phase != DebugPhase::Failed &&
							operation->phase != DebugPhase::Interrupted && operation->phase != DebugPhase::Cancelled,
						"Owned inspection debug operation failed");
				result = *operation;
				return result.phase == DebugPhase::Complete;
			},
			"Owned inspection debug operation timed out");
		return result;
	}
	std::shared_ptr<Session> session;
	std::unique_ptr<DebugController> controller;
	FramePrediction prediction;
	CaptureId capture;
	DebugOperationId step;
	DebugLeaseId lease;
	std::uint64_t next = 1;
};

void Verify(const VulkanInspectionFrame& frame, std::uint32_t width, std::uint32_t height, const EyeColors& expected,
			std::optional<std::uint32_t> layer = {})
{
	auto ticket = frame.Export(layer);
	AwaitComplete(*ticket);
	const auto pixels = ticket->Pixels();
	const std::uint32_t layers = layer ? 1 : 2;
	Require(pixels && pixels->format == VK_FORMAT_R8G8B8A8_SRGB && pixels->width == width && pixels->height == height &&
				pixels->layers == layers &&
				pixels->rgba.size() == static_cast<std::size_t>(width) * height * layers * 4,
			"Owned retained export shape/format mismatch");
	for (std::uint32_t eye = 0; eye < layers; ++eye)
		for (std::uint32_t pixel = 0; pixel < width * height; ++pixel)
			for (std::uint32_t channel = 0; channel < 4; ++channel)
			{
				const auto offset = (static_cast<std::size_t>(eye) * width * height + pixel) * 4 + channel;
				Require(pixels->rgba[offset] == expected[layer.value_or(eye)][channel],
						"Retained owned output pixel changed after source/device destruction");
			}
	std::printf("explicitExport=%ux%u layers=%u allPixels=exact retainedAfterBorrowedTeardown=yes\n", width, height,
				layers);
}

void VerifyControlLayer(const std::shared_ptr<VulkanInspectionFrame>& frame, const EyeColors& expected,
						std::uint32_t layer)
{
	const auto provider = control::CreateVulkanImageInspectionProvider();
	const auto reader = provider->Retain(frame);
	const auto original = reader->Description();
	Require(original.arrayLayers == 2 && original.views.size() == 2,
			"Control slice fixture must retain stereo view metadata");
	const auto exported = reader->Export(layer);
	const auto description = control::DecodeImageDescription(control::EncodeImageDescription(exported->Description()));
	Require(description.arrayLayers == 1 && description.views.size() == 1 && description.width == original.width &&
				description.height == original.height,
			"Single-layer export retained incompatible stereo metadata");
	const auto components = [](const FrameView& view)
	{
		return std::array{view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
						  view.pose.orientation.w, view.pose.position.x,	view.pose.position.y,
						  view.pose.position.z,	   view.fov.angleLeft,		view.fov.angleRight,
						  view.fov.angleUp,		   view.fov.angleDown};
	};
	Require(components(description.views.front()) == components(original.views.at(layer)),
			"Single-layer export changed its selected view metadata");
	AwaitComplete(*exported);
	const auto bytes = exported->Read();
	Require(bytes.size() == static_cast<std::size_t>(description.width) * description.height * 4 &&
				exported->ByteSize() == bytes.size(),
			"Control slice export byte count changed");
	for (std::size_t offset = 0; offset < bytes.size(); ++offset)
		Require(std::to_integer<std::uint8_t>(bytes[offset]) == expected[layer][offset % 4],
				"Control slice export contains pixels from another eye");
	std::puts("case=control-layer-export selectedView=exact metadataRoundTrip=yes pixels=exact PASS");
}

std::shared_ptr<VulkanFrame> Producer(VulkanBackend& backend, const std::shared_ptr<VulkanImage>& source)
{
	const auto layers = Layers(reinterpret_cast<XrSwapchain>(201));
	const auto plan = Plan(*layers, 32, 24);
	std::shared_ptr<VulkanFrame> output;
	Require(backend.Prepare(
				plan,
				[&](XrSwapchain)
				{
					return source;
				},
				output) == XR_SUCCESS,
			"Prepare external inspection regression output");
	return output;
}

void CancelLargeBatch(const VulkanBinding& binding, const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	constexpr std::size_t count = 40;
	VulkanBackend backend(binding, count, SharingExtensions, VK_API_VERSION_1_1);
	VulkanExternalDevice external(backend.Device(), SharingExtensions, VK_API_VERSION_1_1);
	VulkanInspection inspection(external.Identity(), count, validation);
	auto source = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 2, SourceUsage);
	std::vector<std::unique_ptr<VulkanInspectionTransfer>> transfers;
	std::vector<std::shared_ptr<VulkanInspectionFrame>> outputs;
	for (std::size_t index = 0; index < count; ++index)
	{
		auto transfer = inspection.Prepare(Producer(backend, source));
		outputs.push_back(transfer->Output());
		transfers.push_back(std::move(transfer));
	}
	transfers.clear();
	Check(inspection.Close(Timeout), "Drain forty cancelled inspection transfers");
	for (const auto& output : outputs)
		Require(output->PollCompletion() == CompletionStatus::Failed, "Cancelled batch left a pending or ready output");
	outputs.clear();
	source.reset();
	Require(backend.Close() == XR_SUCCESS, "Cancelled batch retained borrowed resources");
	std::puts("case=large-inspection-batch cancelled=40 drained=yes borrowedResourcesReleased=yes PASS");
}

void RejectedPreparation(VulkanBackend& backend, const std::shared_ptr<VulkanImage>& source,
						 const VulkanExternalIdentity& identity,
						 const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	{
		VulkanInspection closed(identity, 1, validation);
		Check(closed.Close(Timeout), "close inspector before rejected preparation");
		auto producer = Producer(backend, source);
		bool rejected = false;
		try
		{
			closed.Prepare(producer);
		}
		catch (const VulkanError& error)
		{
			rejected = error.Result() == VK_ERROR_INITIALIZATION_FAILED;
		}
		Require(rejected, "Closed inspector accepted preparation");
		VulkanInspection available(identity, 1, validation);
		auto retry = available.Prepare(producer);
		Require(backend.Submit(producer) == XR_SUCCESS, "Submit producer after closed-inspector rejection");
		retry->Commit();
		AwaitComplete(*retry->Output());
		Check(available.Close(Timeout), "close inspector after successful source retry");
	}
	{
		VulkanInspection bounded(identity, 1, validation);
		auto blockerProducer = Producer(backend, source);
		auto blocker = bounded.Prepare(blockerProducer);
		auto producer = Producer(backend, source);
		bool rejected = false;
		try
		{
			bounded.Prepare(producer);
		}
		catch (const VulkanError& error)
		{
			rejected = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
		}
		Require(rejected, "Full inspector accepted extra preparation");
		VulkanInspection available(identity, 1, validation);
		auto retry = available.Prepare(producer);
		Require(backend.Submit(producer) == XR_SUCCESS, "Submit producer after inspector-capacity rejection");
		retry->Commit();
		AwaitComplete(*retry->Output());
		blocker.reset();
		VulkanInspection cancelledRetryService(identity, 1, validation);
		auto cancelledRetry = cancelledRetryService.Prepare(blockerProducer);
		Check(bounded.Close(Timeout), "cancel unsubmitted inspector capacity blocker");
		// Draining the old cancelled job must not revoke this source's newer
		// consumer claim. Only one binary-semaphore wait may be admitted.
		VulkanInspection conflicting(identity, 1, validation);
		rejected = false;
		try
		{
			conflicting.Prepare(blockerProducer);
		}
		catch (const VulkanError& error)
		{
			rejected = error.Result() == VK_ERROR_TOO_MANY_OBJECTS;
		}
		Require(rejected, "Retired cancelled inspection revoked the source's newer single-consumer claim");
		Require(backend.Submit(blockerProducer) == XR_SUCCESS, "Submit producer after prepared-transfer cancellation");
		cancelledRetry->Commit();
		AwaitComplete(*cancelledRetry->Output());
		Check(cancelledRetryService.Close(Timeout), "close inspector after cancelled-transfer retry");
		Check(available.Close(Timeout), "close inspector after capacity-rejected source retry");
	}
	std::puts("case=rejected-inspection-prepare closedAndCapacity=checked sameProducerRetry=ready PASS");
}

std::shared_ptr<VulkanInspectionFrame> EarlyClose(VulkanBackend& backend, const std::shared_ptr<VulkanImage>& source,
												  const VulkanExternalIdentity& identity,
												  const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	auto producer = Producer(backend, source);
	std::shared_ptr<VulkanInspectionFrame> output;
	VkResult closed = VK_SUCCESS;
	CompletionStatus firstTerminal = CompletionStatus::Pending;
	{
		VulkanInspection inspection(identity, 1, validation);
		auto transfer = inspection.Prepare(producer);
		output = transfer->Output();
		Require(backend.Submit(producer) == XR_SUCCESS, "Submit producer before zero-time inspector close");
		transfer->Commit();
		closed = inspection.Close(0);
		Require(closed == VK_SUCCESS || closed == VK_TIMEOUT, "Unexpected zero-time inspection close failure");
		firstTerminal = output->PollCompletion();
		Require(firstTerminal != CompletionStatus::Pending, "Closed inspection left a nonterminal output");
		// Explicitly exercise concurrent post-worker control paths. A timed-out
		// copy still needs retirement, even though its public result is terminal.
		std::barrier start(4);
		auto firstDrain = std::async(std::launch::async,
									 [&]
									 {
										 start.arrive_and_wait();
										 return inspection.Drain(Timeout);
									 });
		auto secondDrain = std::async(std::launch::async,
									  [&]
									  {
										  start.arrive_and_wait();
										  return inspection.Drain(Timeout);
									  });
		auto repeatedClose = std::async(std::launch::async,
										[&]
										{
											start.arrive_and_wait();
											return inspection.Close(Timeout);
										});
		start.arrive_and_wait();
		Check(firstDrain.get(), "first concurrent post-stop inspection drain");
		Check(secondDrain.get(), "second concurrent post-stop inspection drain");
		Check(repeatedClose.get(), "concurrent repeated inspection close");
		Require(output->PollCompletion() == firstTerminal, "Concurrent drain resurrected terminal inspection output");
	}
	const auto terminal = output->PollCompletion();
	Require(terminal == firstTerminal, "Inspection teardown changed an already terminal output result");
	Require(terminal == CompletionStatus::Complete || (closed == VK_TIMEOUT && terminal == CompletionStatus::Failed),
			"Service teardown orphaned a permanently pending retained inspection output");
	// A Failed result is cancellation metadata, not evidence of GPU idleness.
	// Observe the actual producer fence before releasing borrowed native owners.
	AwaitComplete(*producer);
	std::printf("case=zero-time-close result=%s retainedOutput=%s orphanPending=no concurrentDrainClose=checked PASS\n",
				closed == VK_TIMEOUT ? "timeout" : "success",
				terminal == CompletionStatus::Complete ? "ready" : "failed");
	return output;
}

using RawColor = std::array<std::uint8_t, 4>;
constexpr std::array<RawColor, 4> RawColors{
	{{{64, 128, 192, 77}}, {{17, 45, 93, 125}}, {{111, 37, 201, 222}}, {{33, 199, 78, 159}}}};

constexpr std::array<float, 4> HdrColor{-0.5f, 0.5f, 2.0f, 0.25f};

void PaintRaw(Device& device, VulkanBackend& backend, const VulkanImage& image)
{
	const bool hdr = ColorPixelBytes(image.format) > 4 || image.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	auto patch = backend.CreateImage(hdr ? image.format : VK_FORMAT_R8G8B8A8_UNORM, {4, 5}, 1, SourceUsage);
	Check(backend.InitializeImage(patch)->Wait(Timeout), "initialize raw source patch");
	Commands work(device);
	VulkanImageBarrier(work.command, image, image.ApplicationLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	VkClearColorValue border{{0, 0, 1, 1}};
	VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.layers};
	vkCmdClearColorImage(work.command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &border, 1, &all);
	VulkanImageBarrier(work.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	VulkanImageBarrier(work.command, *patch, patch->ApplicationLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	const VkImageSubresourceRange slice{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	for (std::uint32_t layer = 0; layer < RawColors.size(); ++layer)
	{
		VkClearColorValue color{};
		for (std::uint32_t channel = 0; channel < 4; ++channel)
			color.float32[channel] = hdr ? HdrColor[channel] : static_cast<float>(RawColors[layer][channel]) / 255;
		vkCmdClearColorImage(work.command, patch->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &slice);
		VulkanImageBarrier(work.command, *patch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
						   VK_ACCESS_TRANSFER_READ_BIT);
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
		copy.dstOffset = {static_cast<std::int32_t>(2 + layer), static_cast<std::int32_t>(3 + layer), 0};
		copy.extent = {4, 5, 1};
		vkCmdCopyImage(work.command, patch->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
					   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		VulkanImageBarrier(work.command, *patch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
						   VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	VulkanImageBarrier(work.command, *patch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, patch->ApplicationLayout(),
					   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	VulkanImageBarrier(work.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.ApplicationLayout(),
					   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	work.Finish();
}

std::shared_ptr<const FrameLayers> RawLayers(XrSwapchain source, RuntimeTime time)
{
	ProjectionLayerDescription projection;
	projection.space.worldPose = Pose();
	for (std::uint32_t view = 0; view < 4; ++view)
		projection.views.push_back(
			{IdentityPose(),
			 Pose(),
			 Fov(),
			 {source, {{static_cast<std::int32_t>(2 + view), static_cast<std::int32_t>(3 + view)}, {4, 5}}, view},
			 {}});
	QuadLayerDescription quad{{XR_NULL_HANDLE, 0, Pose()},	XR_EYE_VISIBILITY_BOTH, IdentityPose(), Pose(), {1, 1},
							  {source, {{5, 6}, {4, 5}}, 3}};
	return std::make_shared<const FrameLayers>(time, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
											   std::vector<LayerDescription>{std::move(projection), quad});
}

struct RawRetention
{
	std::vector<std::shared_ptr<VulkanInspectionFrame>> frames;
	std::vector<SnapshotRecord> readers;
};
RawRetention CaptureRaw(Device& device, const VulkanBinding& binding,
						const std::shared_ptr<VulkanInspectionValidation>& validation, VkFormat format)
{
	RawRetention retained;
	VulkanBackend backend(binding, 8, SharingExtensions, VK_API_VERSION_1_1);
	VulkanExternalDevice external(backend.Device(), SharingExtensions, VK_API_VERSION_1_1);
	VulkanInspection inspection(external.Identity(), 8, validation);
	auto source = backend.CreateImage(format, {16, 16}, 4, SourceUsage);
	Check(backend.InitializeImage(source)->Wait(Timeout), "initialize four-view raw source");
	PaintRaw(device, backend, *source);
	const auto sourceId = reinterpret_cast<XrSwapchain>(201);
	for (std::uint32_t index = 0; index < 5; ++index)
	{
		const CaptureTarget target{CaptureKind::LayerView, index < 4 ? 0U : 1U, index < 4 ? index : 0U};
		DebugCapture capture(backend, target);
		Require(capture.controller->Snapshot().layerViews, "Actual raw capture capability missing");
		auto layers = RawLayers(sourceId, capture.prediction.displayTime);
		auto plan = Plan(*layers, 8, 6);
		const FrameKey key{capture.session->Snapshot().generation, capture.prediction.id};
		std::shared_ptr<VulkanFrame> composed, raw;
		Require(backend.Prepare(
					plan,
					[&](XrSwapchain)
					{
						return source;
					},
					composed, capture.session->Captures().ProducerReservation(key)) == XR_SUCCESS,
				"Prepare reserved composed companion for raw capture");
		const auto selected = FindLayerImage(*layers, target.layer, target.view);
		Require(selected.has_value(), "Find selected raw source metadata");
		const auto rawToken = capture.session->Captures().ProducerReservation(key, target);
		auto invalid = *selected;
		invalid.rectangle.offset.x = 16;
		auto unchanged = composed;
		Require(backend.PrepareCapture(
					invalid,
					[&](XrSwapchain)
					{
						return source;
					},
					unchanged, rawToken) == XR_ERROR_SWAPCHAIN_RECT_INVALID &&
					unchanged == composed && !rawToken->Consumed(),
				"Invalid raw rectangle changed output or consumed its reserved retry capacity");
		Require(selected.has_value() &&
					backend.PrepareCapture(
						*selected,
						[&](XrSwapchain)
						{
							return source;
						},
						raw, capture.session->Captures().ProducerReservation(key, target)) == XR_SUCCESS,
				"Prepare exact selected layer/view GPU copy");
		unchanged = raw;
		Require(backend.PrepareCapture(
					*selected,
					[&](XrSwapchain)
					{
						return source;
					},
					unchanged, rawToken) == XR_ERROR_CALL_ORDER_INVALID &&
					unchanged == raw,
				"Consumed raw reservation was accepted twice or replaced its output");
		auto displayTransfer = inspection.Prepare(composed);
		auto rawTransfer = inspection.Prepare(raw);
		auto output = rawTransfer->Output();
		const std::array duplicate{composed, composed};
		Require(backend.Submit(duplicate) == XR_ERROR_VALIDATION_FAILURE && !composed->Submitted() && !raw->Submitted(),
				"Rejected duplicate batch consumed a producer or accepted partial GPU work");
		const std::array batch{composed, raw};
		const std::array outputs{CaptureOutput{target, output}};
		XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			  nullptr, capture.prediction.displayTime.value.count(),
							XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 2,	   nullptr};
		Require(capture.session->EndFrame(
					capture.prediction, &info, layers, displayTransfer->Output(),
					[&]
					{
						const auto result = backend.Submit(batch);
						if (result == XR_SUCCESS)
						{
							displayTransfer->Commit();
							rawTransfer->Commit();
						}
						return result;
					},
					outputs) == XR_SUCCESS,
				"Atomic raw plus composed native frame acceptance");
		const auto operation = capture.AwaitOperation(capture.step);
		Require(operation.progress == DebugProgress::SnapshotReady && operation.capture == capture.capture,
				"Raw GPU readiness needed another host frame call or selected composed output");
		auto reader = capture.session->Captures().Read(capture.capture);
		Require(reader && reader->phase == SnapshotPhase::Ready && reader->target == target && reader->frame == key &&
					reader->output.get() == output.get() && reader->metadata && reader->metadata->submission &&
					reader->metadata->submission->layers == layers && output->Image().layers == 1 &&
					output->Image().extent.width == 4 && output->Image().extent.height == 5 &&
					output->Image().format == format && output->View(0),
				"Raw capture lost exact target, rectangle, format, metadata or single-slice identity");
		Require(capture.AwaitOperation(capture.Command(DebugCommand::Release, capture.capture)).progress ==
					DebugProgress::Released,
				"Release exact raw capture");
		retained.frames.push_back(std::move(output));
		retained.readers.push_back(std::move(*reader));
	}
	// Replace every source byte; retained outputs must still describe their original views.
	{
		Commands work(device);
		VulkanImageBarrier(work.command, *source, source->ApplicationLayout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkClearColorValue clear{{1, 0, 1, 1}};
		VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4};
		vkCmdClearColorImage(work.command, source->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
		VulkanImageBarrier(work.command, *source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, source->ApplicationLayout(),
						   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
		work.Finish();
	}
	Check(backend.Drain(Timeout), "drain raw and composed common producer fence");
	Check(inspection.Close(Timeout), "close raw inspection service");
	source.reset();
	Require(backend.Close() == XR_SUCCESS, "Raw readers retained borrowed application resources");
	std::printf("case=exact-layer-view format=%d rawOuter=2 rawInset=2 rawQuad=1 crop=4x5 preview=8x6 atomicBatch=yes "
				"sourceReused=yes PASS\n",
				static_cast<int>(format));
	return retained;
}
void VerifyRaw(const RawRetention& retained)
{
	Require(retained.frames.size() == 5 && retained.readers.size() == 5, "Retained raw target count changed");
	for (std::size_t index = 0; index < retained.frames.size(); ++index)
	{
		auto ticket = retained.frames[index]->Export();
		AwaitComplete(*ticket);
		auto pixels = ticket->Pixels();
		Require(pixels && pixels->format == retained.frames[index]->Image().format && pixels->width == 4 &&
					pixels->height == 5 && pixels->layers == 1 && pixels->rgba.size() == 4 * 5 * ColorPixelBytes(pixels->format),
				"Delayed raw export shape/format mismatch");
		const auto& expected = RawColors[std::min<std::size_t>(index, 3)];
		const auto stride = ColorPixelBytes(pixels->format);
		const bool packed = pixels->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		const auto bytes = std::as_bytes(std::span(pixels->rgba));
		if (stride > 4 || packed)
		{
			for (std::size_t offset = 0; offset < bytes.size(); offset += stride)
			{
				const auto color = DecodeColorPixel(pixels->format, bytes.subspan(offset, stride));
				for (std::size_t channel = 0; channel < 4; ++channel)
				{
					const float levels = channel == 3 ? 3.0f : 1023.0f;
					const float value = packed ? std::round(std::clamp(HdrColor[channel], 0.0f, 1.0f) * levels) / levels : HdrColor[channel];
					Require(std::abs(color[channel] - value) <= (packed && channel < 3 ? 1.0f / 1023 : 0.0001f),
						"HDR crop/slice/range/alpha changed");
				}
			}
			auto provider = control::CreateVulkanImageInspectionProvider();
			auto reader = provider->Retain(retained.frames[index]);
			auto exported = reader->Export(std::nullopt);
			Require(exported->ByteSize() == bytes.size() && exported->Description().format == pixels->format,
				"Control HDR export size/format mismatch");
			AwaitComplete(*exported);
			Require(std::ranges::equal(exported->Read(), bytes), "Control HDR export changed exact source bytes");
		}
		else
			for (std::size_t offset = 0; offset < pixels->rgba.size(); ++offset)
				Require(pixels->rgba[offset] == expected[offset % 4], "Raw crop/slice/format/alpha bytes changed");
		Require(retained.readers[index].output.get() == retained.frames[index].get() &&
					retained.readers[index].phase == SnapshotPhase::Ready,
				"Raw metadata reader changed after native application parent teardown");
	}
	std::puts("case=delayed-exact-raw-export-after-application-device-destruction everyByte=checked PASS");
}

SnapshotAdmission Promote(const std::shared_ptr<const FrameOutput>& output, const FramePool& pool,
						  std::shared_ptr<FrameReservation> reservation)
{
	const auto frame = std::dynamic_pointer_cast<const VulkanInspectionFrame>(output);
	return frame && frame->PromoteCaptureBudget(pool, std::move(reservation)) ? SnapshotAdmission::Accepted
																			  : SnapshotAdmission::Capacity;
}

SnapshotRecord CaptureOwned(VulkanBackend& backend, VulkanInspection& inspection,
							const std::shared_ptr<VulkanImage>& source, XrSwapchain sourceId)
{
	DebugCapture capture(backend);
	auto layers = Layers(sourceId, capture.prediction.displayTime);
	auto plan = Plan(*layers, 32, 24);
	const FrameKey key{capture.session->Snapshot().generation, capture.prediction.id};
	std::shared_ptr<VulkanFrame> producer;
	Require(backend.Prepare(
				plan,
				[&](XrSwapchain)
				{
					return source;
				},
				producer, capture.session->Captures().ProducerReservation(key)) == XR_SUCCESS,
			"Prepare exact retained budget image");
	auto transfer = inspection.Prepare(producer);
	auto output = transfer->Output();
	XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			  nullptr, capture.prediction.displayTime.value.count(),
						XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,	   nullptr};
	Require(capture.session->EndFrame(capture.prediction, &info, layers, output,
									  [&]
									  {
										  const auto result = backend.Submit(producer);
										  if (result == XR_SUCCESS)
											  transfer->Commit();
										  return result;
									  }) == XR_SUCCESS,
			"Submit exact retained budget image");
	capture.AwaitOperation(capture.step);
	auto reader = capture.session->Captures().Read(capture.capture);
	Require(reader && reader->phase == SnapshotPhase::Ready, "Retain exact budget snapshot");
	capture.AwaitOperation(capture.Command(DebugCommand::Release, capture.capture));
	return std::move(*reader);
}

SnapshotRecord CaptureSequence(Device& device, const VulkanBinding& binding,
	const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	VulkanBackend backend(binding, 1, SharingExtensions, VK_API_VERSION_1_1, 1);
	VulkanExternalDevice external(backend.Device(), SharingExtensions, VK_API_VERSION_1_1);
	VulkanInspection inspection(external.Identity(), 2, validation);
	auto source = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 2, SourceUsage);
	Check(backend.InitializeImage(source)->Wait(Timeout), "Initialize sequence capture source");
	Paint(device, *source, Original);
	const auto sourceId = reinterpret_cast<XrSwapchain>(402);
	Instance instance(reinterpret_cast<XrInstance>(400), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(401), instance);
	Require(session.ConfigureCapturePool(backend.Reservations(), true) == XR_SUCCESS,
		"Configure sequence native capture budget");
	FrameSchedulerConfig timing;
	timing.period = Duration{5000000};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
		"Begin real-time native sequence capture");
	PoseSequenceStep step;
	step.capture = true;
	step.head = HeadPosePatch{};
	step.head->position[0] = 4;
	const CommandSource owner{CommandSourceKind::Mcp, 91};
	const auto sequence = session.Commands().StartPoseSequence(owner,
		std::make_shared<const PoseSequence>(std::vector{step}, true));
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
		"Issue native sequence frame without a manual permit");
	const FrameKey key{session.Snapshot().generation, prediction.id};
	const auto receipt = session.History().Find(key)->sequenceCapture;
	Require(receipt && receipt->request.sequence == sequence->id && receipt->request.frame == key.frame &&
		receipt->reservation.result == SnapshotAdmission::Accepted && !session.Scheduler().Snapshot().paused,
		"Native sequence capture changed pacing or lost frame identity");
	auto layers = Layers(sourceId, prediction.displayTime);
	auto plan = Plan(*layers, 32, 24);
	std::shared_ptr<VulkanFrame> producer;
	Require(backend.Prepare(plan,
		[&](XrSwapchain)
		{
			return source;
		}, producer, session.Captures().ProducerReservation(key)) == XR_SUCCESS,
		"Prepare sequence's reserved native producer");
	auto transfer = inspection.Prepare(producer);
	auto output = transfer->Output();
	const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, prediction.displayTime.value.count(),
		XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, nullptr};
	Require(session.EndFrame(prediction, &end, layers, output,
		[&]
		{
			const auto result = backend.Submit(producer);
			if (result == XR_SUCCESS)
				transfer->Commit();
			return result;
		}) == XR_SUCCESS, "Submit native sequence capture");
	AwaitComplete(*output);
	session.Captures().Poll();
	auto reader = session.Captures().Read(receipt->reservation.id);
	Require(reader && reader->phase == SnapshotPhase::Ready && reader->metadata->key == key &&
		reader->metadata->submission->commands->configuration.tracking.head.pose.position.x == 4,
		"Sequence pixels do not retain their exact commanded frame");
	Paint(device, *source, Reused);
	Require(session.WaitFrame(prediction) == XR_SUCCESS, "Sequence capacity pressure stopped real-time frames");
	const auto pressure = session.History().Find({key.generation, prediction.id})->sequenceCapture;
	Require(pressure && pressure->reservation.result == SnapshotAdmission::Capacity &&
		!pressure->reservation.id.value && session.BeginFrame() == XR_SUCCESS && session.EndFrame(prediction) == XR_SUCCESS,
		"Native capture pressure lost its explicit result or substituted a frame");
	session.ReleaseCommandSource(owner, true);
	session.Captures().Poll();
	Require(!session.Captures().Read(receipt->reservation.id), "Disconnected sequence retained its capture ticket");
	session.Close();
	session.Display().PollRetired();
	transfer.reset();
	producer.reset();
	output.reset();
	Check(backend.Drain(Timeout), "Drain sequence capture producer");
	Check(inspection.Close(Timeout), "Close sequence inspection service");
	source.reset();
	Require(backend.Close() == XR_SUCCESS, "Retained sequence image held borrowed application resources");
	std::puts("case=sequence-capture realtime=yes exactPoseFrame=yes capacity=explicit sourceReused=yes ownerReleased=yes PASS");
	return std::move(*reader);
}

struct LiveResult
{
	FrameKey key;
	std::shared_ptr<VulkanFrame> producer;
	std::shared_ptr<VulkanInspectionFrame> output;
	bool submitted = false;
};
LiveResult SubmitLive(Session& session, VulkanBackend& backend, VulkanInspection& inspection,
					  const std::shared_ptr<VulkanImage>& source, XrSwapchain sourceId)
{
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Issue ordinary live frame");
	LiveResult result{{session.Snapshot().generation, prediction.id}};
	auto layers = Layers(sourceId, prediction.displayTime);
	auto plan = Plan(*layers, 32, 24);
	std::unique_ptr<VulkanInspectionTransfer> transfer;
	auto token = backend.LiveReservations().Reserve();
	if (token)
	{
		Require(backend.Prepare(
					plan,
					[&](XrSwapchain)
					{
						return source;
					},
					result.producer, std::move(token)) == XR_SUCCESS,
				"Prepare independent live budget output");
		transfer = inspection.Prepare(result.producer);
		result.output = transfer->Output();
	}
	XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			  nullptr, prediction.displayTime.value.count(),
						XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,	   nullptr};
	std::function<XrResult()> submit;
	if (result.producer)
		submit = [&]
		{
			result.submitted = true;
			const auto accepted = backend.Submit(result.producer);
			if (accepted == XR_SUCCESS)
				transfer->Commit();
			return accepted;
		};
	Require(session.EndFrame(prediction, &info, layers, result.output, submit, {}, FrameSubmissionPolicy::Live) ==
				XR_SUCCESS,
			"Ordinary capacity pressure stalled CPU frame coordination");
	return result;
}

std::vector<SnapshotRecord> LiveCapacity(Device& device, const VulkanBinding& binding,
										 const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	VulkanBackend backend(binding, 2, SharingExtensions, VK_API_VERSION_1_1, 1);
	VulkanExternalDevice external(backend.Device(), SharingExtensions, VK_API_VERSION_1_1);
	VulkanInspection inspection(external.Identity(), 4, validation);
	auto source = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 2, SourceUsage);
	Check(backend.InitializeImage(source)->Wait(Timeout), "Initialize live-pressure source");
	Paint(device, *source, Original);
	const auto sourceId = reinterpret_cast<XrSwapchain>(202);
	std::optional<SnapshotRecord> first = CaptureOwned(backend, inspection, source, sourceId);
	auto second = CaptureOwned(backend, inspection, source, sourceId);
	Require(backend.Reservations().InUse() == 2 && !backend.Reservations().Reserve(),
			"Exhaust exact snapshot capacity");
	auto instance = std::make_shared<Instance>(reinterpret_cast<XrInstance>(300), std::span<const char* const>{},
											   XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(301), instance);
	Require(session.ConfigureCapturePool(backend.Reservations(), true, Promote) == XR_SUCCESS,
			"Configure live snapshot promotion");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.period = Duration{5000000};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Begin live-capacity session");
	auto live = SubmitLive(session, backend, inspection, source, sourceId);
	Require(live.submitted && live.output, "Full debug capacity prevented independent live production");

	AwaitComplete(*live.output);
	live.producer.reset();
	Require(session.Display().Pause(), "Freeze live selection");
	while (session.Display().NextTick() <= session.History().Find(live.key)->prediction.displayTick)
		Require(session.Display().Step().has_value(), "Select real live output");
	Require(session.Display().Snapshot().latest->source == live.key && backend.LiveReservations().InUse() == 1,
			"Live output was not selected on its independent budget");
	const auto oldImage = live.output->Image().image;
	auto dropped = SubmitLive(session, backend, inspection, source, sourceId);
	const auto droppedRecord = session.History().Find(dropped.key);
	Require(!dropped.producer && !dropped.submitted &&
				droppedRecord->submission->outputDisposition == FrameOutputDisposition::Dropped &&
				droppedRecord->submission->dropReason == FrameDropReason::NativeCapacity &&
				session.Display().Snapshot().latest->source == live.key &&
				session.Snapshot().lastSubmitted == dropped.key.frame,
			"Live native pressure changed selected source, fabricated a clear, or lost explicit CPU drop metadata");
	session.Scheduler().Pause();
	Require(session.CaptureBoundary().result == SnapshotAdmission::Capacity,
			"Full debug pool silently accepted live promotion");
	first.reset();
	Await(
		[&]
		{
			return backend.Reservations().InUse() == 1;
		},
		"Release exact budget reader");
	const auto boundary = session.CaptureBoundary();
	Require(boundary.result == SnapshotAdmission::Accepted && boundary.captures.size() == 1,
			"Promote retained live image for pause");
	session.Captures().Poll();
	auto pinned = session.Captures().Read(boundary.captures.front());
	Require(pinned && pinned->phase == SnapshotPhase::Ready && pinned->frame == live.key &&
				pinned->output.get() == live.output.get() && live.output->Image().image == oldImage &&
				backend.Reservations().InUse() == 2 && backend.LiveReservations().InUse() == 0,
			"Boundary snapshot kept live capacity occupied or changed its image identity");
	Require(live.output->PromoteCaptureBudget(backend.Reservations()) && backend.Reservations().InUse() == 2,
			"Repeated promotion consumed another debug slot");
	const auto tick = std::max(session.Scheduler().Snapshot().nextAdmission, session.Display().NextTick());
	const auto host = session.ClockSource()->Now();
	Require(session.Display().Resume(host, tick) && session.Scheduler().Resume(host, tick) == XR_SUCCESS,
			"Resume after exact boundary promotion");
	Paint(device, *source, Reused);
	auto resumed = SubmitLive(session, backend, inspection, source, sourceId);
	Require(resumed.submitted && resumed.output && backend.Reservations().InUse() == 2,
			"Retained boundary snapshot prevented ordinary output after resume");
	AwaitComplete(*resumed.output);
	resumed.producer.reset();
	session.Captures().Release(boundary.captures.front());
	live.output.reset();
	session.Close();
	resumed.output.reset();
	Check(inspection.Drain(Timeout), "Drain live completion and retirement");
	session.Display().PollRetired();
	backend.RetireCompleted();
	Await(
		[&]
		{
			return backend.LiveReservations().InUse() == 0;
		},
		"Return ordinary live slot after session closure");
	{
		Session pressure(reinterpret_cast<XrSession>(302), instance);
		Require(pressure.Ready(config.epoch) == XR_SUCCESS && pressure.Begin(begin, config) == XR_SUCCESS &&
					pressure.Display().Pause(),
				"Begin presentation-pressure session");
		for (std::size_t index = 0; index < pressure.Display().Snapshot().capacity; ++index)
		{
			FramePrediction clear;
			Require(pressure.WaitFrame(clear) == XR_SUCCESS && pressure.BeginFrame() == XR_SUCCESS &&
						pressure.EndFrame(clear) == XR_SUCCESS,
					"Fill exact presentation queue with submitted clears");
		}
		auto rejected = SubmitLive(pressure, backend, inspection, source, sourceId);
		const auto history = pressure.History().Find(rejected.key);
		Require(rejected.producer && !rejected.submitted && !rejected.producer->Submitted() &&
					history->submission->outputDisposition == FrameOutputDisposition::Dropped &&
					history->submission->dropReason == FrameDropReason::PresentationCapacity,
				"Presentation pressure submitted GPU work or failed ordinary frame coordination");
		Await(
			[&]
			{
				return rejected.output->PollCompletion() == CompletionStatus::Failed;
			},
			"Cancel unsubmitted dropped live transfer");
		rejected.producer.reset();
		rejected.output.reset();
		FramePrediction clear;
		Require(pressure.WaitFrame(clear) == XR_SUCCESS && pressure.BeginFrame() == XR_SUCCESS &&
					pressure.EndFrame(clear, nullptr, {}, {}, {}, {}, FrameSubmissionPolicy::Live) == XR_SUCCESS,
				"Full presentation queue blocked an ordinary clear");
		const FrameKey clearKey{pressure.Snapshot().generation, clear.id};
		while (pressure.Display().NextTick() <= clear.displayTick)
			Require(pressure.Display().Step().has_value(), "Advance to live clear marker");
		Require(pressure.Display().Snapshot().latest->kind == PresentationKind::Clear &&
					pressure.Display().Snapshot().latest->source == clearKey,
				"Live capacity pressure repeated a stale image instead of the exact latest clear");
		pressure.Close();
	}
	Check(inspection.Close(Timeout), "Close live-budget inspector");
	Check(backend.Drain(Timeout), "Drain independent live and debug producers");
	source.reset();
	Require(backend.Close() == XR_SUCCESS && !backend.LiveReservations().Reserve(),
			"Close native live budget with retained owned snapshots");
	std::puts("case=live-debug-capacity debugFull=yes liveFull=explicitDrop presentationFull=explicitDrop "
			  "nativeDropSubmissions=0 boundaryPromotion=sameImage resume=progress clear=mailbox PASS");
	return {std::move(second), std::move(*pinned)};
}

bool Run(bool verify, const std::shared_ptr<VulkanInspectionValidation>& validation)
{
	std::shared_ptr<VulkanInspectionFrame> retained, resized, closedOutput;
	auto retirements = std::make_shared<std::pair<std::mutex, std::vector<SharedImageKey>>>();
	std::optional<SnapshotRecord> retainedCpuReader, sequenceRetention;
	RawRetention rawRetention, srgbRawRetention;
	std::array<RawRetention, 2> bgraRetention;
	std::array<RawRetention, 3> hdrRetention;
	std::vector<simxr::probe::RetainedSubmission> automatic;
	std::vector<SnapshotRecord> liveRetention;
	{
		simxr::probe::Instance instance;
		instance.Create();
		Device device;
		if (!device.Create(instance.handle))
		{
			std::puts("SKIP: adapter lacks complete opaque-FD image and binary-semaphore export/import capability");
			return false;
		}
		const VulkanBinding binding{instance.handle, device.physical, device.native, device.family, device.index};
		for (const auto format :
			 {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM})
		{
			VulkanBackend backend(binding, 2, SharingExtensions, VK_API_VERSION_1_1);
			VulkanExternalDevice external(backend.Device(), SharingExtensions, VK_API_VERSION_1_1);
			VulkanInspection inspection(external.Identity(), 1, validation);
			auto source = backend.CreateImage(format, {16, 16}, 4, SourceUsage);
			Check(backend.InitializeImage(source)->Wait(Timeout), "Initialize automatic retention source");
			PaintRaw(device, backend, *source);
			automatic.push_back(simxr::probe::RetainSubmittedLayers(backend, inspection, source));
			Paint(device, *source, Reused);
			Check(inspection.Close(Timeout), "Close automatic retention inspector");
			Check(backend.Drain(Timeout), "Drain automatic retention producers");
			source.reset();
			Require(backend.Close() == XR_SUCCESS, "Automatic retention kept borrowed native images alive");
		}
		sequenceRetention = CaptureSequence(device, binding, validation);
		liveRetention = LiveCapacity(device, binding, validation);
		rawRetention = CaptureRaw(device, binding, validation, VK_FORMAT_R8G8B8A8_UNORM);
		srgbRawRetention = CaptureRaw(device, binding, validation, VK_FORMAT_R8G8B8A8_SRGB);
		bgraRetention[0] = CaptureRaw(device, binding, validation, VK_FORMAT_B8G8R8A8_UNORM);
		bgraRetention[1] = CaptureRaw(device, binding, validation, VK_FORMAT_B8G8R8A8_SRGB);
		hdrRetention[0] = CaptureRaw(device, binding, validation, VK_FORMAT_R16G16B16A16_SFLOAT);
		hdrRetention[1] = CaptureRaw(device, binding, validation, VK_FORMAT_R32G32B32A32_SFLOAT);
		hdrRetention[2] = CaptureRaw(device, binding, validation, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
		{
			VulkanBackend disabled(binding, 1);
			Require(!disabled.Sharing(), "Unconfigured logical sharing became enabled");
			Require(disabled.Close() == XR_SUCCESS, "Close unconfigured backend");
		}
		CancelLargeBatch(binding, validation);
		VulkanBackend backend(binding, 3, SharingExtensions, VK_API_VERSION_1_1);
		Require(backend.Sharing(), "Explicit logical external-sharing configuration unavailable");
		bool rejectedBorrow = false;
		try
		{
			VulkanBackend invalid(backend.Device());
		}
		catch (const std::invalid_argument&)
		{
			rejectedBorrow = true;
		}
		Require(rejectedBorrow, "Borrowed device accepted as an independently owned compositor");
		VulkanExternalDevice external(backend.Device(), SharingExtensions, VK_API_VERSION_1_1);
		VerifyOwnedDeviceAcquisition(external.Identity(), validation);
		VulkanInspection inspection(external.Identity(), 3, validation);
		const auto sourceId = reinterpret_cast<XrSwapchain>(200);
		auto source = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {16, 16}, 2, SourceUsage);
		{
			auto ticket = backend.InitializeImage(source);
			Check(ticket->Wait(Timeout), "initialize inspection source");
		}
		Paint(device, *source, Original);
		RejectedPreparation(backend, source, external.Identity(), validation);
		closedOutput = EarlyClose(backend, source, external.Identity(), validation);
		{
			DebugCapture capture(backend);
			auto layers = Layers(sourceId, capture.prediction.displayTime);
			auto plan = Plan(*layers, 32, 24);
			std::shared_ptr<VulkanFrame> producer;
			const auto key = FrameKey{capture.session->Snapshot().generation, capture.prediction.id};
			Require(backend.Prepare(
						plan,
						[&](XrSwapchain)
						{
							return source;
						},
						producer, capture.session->Captures().ProducerReservation(key)) == XR_SUCCESS,
					"Prepare captured external producer output");
			auto transfer = inspection.Prepare(producer, {}, {}, VulkanInspectionKind::Required,
											   [retirements](SharedImageKey image)
											   {
												   std::lock_guard lock(retirements->first);
												   retirements->second.push_back(image);
											   });
			retained = transfer->Output();
			const std::array<std::shared_ptr<const VulkanInspectionFrame>, 2> pending{retained, retained};
			Require(!VulkanInspectionFrame::Compose(plan, pending), "Pending imports were sampled by composition");
			Require(retained && retained->PollCompletion() == CompletionStatus::Pending,
					"Uncommitted owned transfer reported GPU completion");
			XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			  nullptr, capture.prediction.displayTime.value.count(),
								XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,	   nullptr};
			Require(capture.session->EndFrame(capture.prediction, &info, layers, retained,
											  [&]
											  {
												  const auto result = backend.Submit(producer);
												  if (result == XR_SUCCESS)
													  transfer->Commit();
												  return result;
											  }) == XR_SUCCESS,
					"Commit captured owned transfer at actual producer acceptance");
			// No WaitFrame/BeginFrame/EndFrame occurs from here until capture
			// readiness: the owned queue and controller worker progress independently.
			const auto completed = capture.AwaitOperation(capture.step);
			Require(completed.progress == DebugProgress::SnapshotReady && completed.capture == capture.capture,
					"Controller did not observe independently completed owned copy");
			retainedCpuReader = capture.session->Captures().Read(capture.capture);
			Require(retainedCpuReader && retainedCpuReader->phase == SnapshotPhase::Ready &&
						retainedCpuReader->output.get() == retained.get() && retainedCpuReader->frame == key &&
						retainedCpuReader->metadata && retainedCpuReader->metadata->submission &&
						retainedCpuReader->metadata->submission->layers == layers,
					"Owned snapshot lost exact private output/CPU metadata identity");
			Require(capture.session->Scheduler().Snapshot().paused && capture.session->Display().Paused(),
					"Independent copy secretly reopened paused frame/display admission");
			Require(capture.AwaitOperation(capture.Command(DebugCommand::Release, capture.capture)).progress ==
							DebugProgress::Released &&
						!capture.session->Captures().Read(capture.capture),
					"Release owned capture logical lookup");
			std::puts("case=controller-owned-copy-ready paused=yes furtherXRCalls=0 readerRetained=yes PASS");
		}
		AwaitComplete(*retained);
		const auto originalImage = retained->Image().image;
		Require(retained->Image().device->binding.device != device.native && retained->View(0) && retained->View(1),
				"Inspection output still belongs to borrowed application device");
		Paint(device, *source, Reused);
		{
			auto layers = Layers(sourceId);
			auto plan = Plan(*layers, 40, 30);
			std::shared_ptr<VulkanFrame> producer;
			Require(backend.Prepare(
						plan,
						[&](XrSwapchain)
						{
							return source;
						},
						producer) == XR_SUCCESS,
					"Prepare resized later external output");
			auto transfer = inspection.Prepare(producer);
			resized = transfer->Output();
			Require(backend.Submit(producer) == XR_SUCCESS, "Submit resized later external output");
			transfer->Commit();
			AwaitComplete(*resized);
		}
		Require(retained->Image().image == originalImage && retained->Image().extent.width == 32 &&
					retained->Image().extent.height == 24 && resized->Image().extent.width == 40 &&
					resized->Image().extent.height == 30,
				"Resize mutated old private owned output");
		source.reset();
		Check(inspection.Close(Timeout), "close independent inspection service with retained output readers");
		Check(backend.Drain(Timeout), "drain actual borrowed producer jobs");
		Require(backend.Close() == XR_SUCCESS, "Owned final readers prevented borrowed backend teardown");
		std::puts("case=source-reuse-resize retainedPrivateOutputs=2 PASS");
	}
	// Both the borrowed device/instance and the inspection service have now gone.
	// Private outputs retain their independent device and explicit export queue.
	AwaitComplete(*retained);
	AwaitComplete(*resized);
	Require(closedOutput && closedOutput->PollCompletion() != CompletionStatus::Pending,
			"Retained closed-service output lost its terminal result after borrowed teardown");
	Require(retainedCpuReader && retainedCpuReader->output.get() == retained.get(),
			"Explicit reader did not survive logical/session/native parent teardown");
	simxr::probe::VerifyImageDelivery(retained, resized, verify,
									  [retirements](SharedImageKey image)
									  {
										  std::lock_guard lock(retirements->first);
										  return std::count(retirements->second.begin(), retirements->second.end(),
															image) == 1;
									  });
	if (verify)
	{
		const auto sequenceImage = std::dynamic_pointer_cast<const VulkanInspectionFrame>(sequenceRetention->output);
		Require(sequenceImage != nullptr, "Sequence capture lost its independent native image");
		Verify(*sequenceImage, 32, 24, Original);
		Verify(*retained, 32, 24, Original);
		Verify(*retained, 32, 24, Original, 1);
		VerifyControlLayer(retained, Original, 1);
		Verify(*resized, 40, 30, Reused);
		VerifyRaw(rawRetention);
		VerifyRaw(srgbRawRetention);
		for (const auto& bgra : bgraRetention)
			VerifyRaw(bgra);
		for (const auto& hdr : hdrRetention)
			VerifyRaw(hdr);
		for (const auto& snapshot : liveRetention)
		{
			const auto image = std::dynamic_pointer_cast<const VulkanInspectionFrame>(snapshot.output);
			Require(image && snapshot.phase == SnapshotPhase::Ready,
					"Retain exact image across ordinary live pressure");
			Verify(*image, 32, 24, Original);
		}
	}
	bgraRetention = {};
	hdrRetention = {};
	rawRetention.readers.clear();
	srgbRawRetention.readers.clear();
	for (auto& retained : automatic)
		simxr::probe::VerifySubmittedLayers(std::move(retained), verify);
	automatic.clear();
	simxr::probe::VerifyRetainedComposition(std::move(rawRetention.frames), std::move(srgbRawRetention.frames), verify);
	std::printf("case=owned-retention-after-borrowed-instance-and-device-destruction normalReadback=%s PASS\n",
				verify ? "explicit-verification-only" : "none");
	return true;
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
			std::fprintf(stderr, "Unknown inspection probe argument: %s\n", argv[index]);
			return 1;
		}
	}
	try
	{
		auto validation = std::make_shared<simxr::VulkanInspectionValidation>();
		const bool supported = Run(verify, validation);
		Await(
			[&]
			{
				return validation->liveOwnedDevices.load() == 0;
			},
			"Owned delivery devices did not finish native teardown");
		Require(simxr::probe::s_validationErrors.load() == 0 && validation->errors.load() == 0,
				"Validation errors in borrowed or independently owned Vulkan instance, including teardown");
		if (!supported)
			return 77;
		std::printf("Production owned-device Vulkan inspection PASS mode=%s validationErrors=0\n",
					verify ? "explicit-pixel-export" : "GPU-only");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
