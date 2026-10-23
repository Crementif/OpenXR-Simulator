#include "VulkanImageDeliveryProbe.h"
#include "Pch.h"
#include "VulkanProbeInstance.h"
#include "control/ControlSchema.h"
#include "control/ImageInspection.h"
#include "control/LocalControlClient.h"
#include "control/backends/vulkan/VulkanImageConsumer.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "graphics/ColorPixels.h"
#include "graphics/SnapshotBudget.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>

namespace simxr::probe
{
struct SubmissionFault
{
	VkQueue queue = VK_NULL_HANDLE;
	VkResult result = VK_SUCCESS;
	bool fired = false;
	void (*beforeReturn)(void*) noexcept = nullptr;
	void* context = nullptr;
};
static thread_local SubmissionFault s_submissionFault;
static thread_local bool s_failRecording = false;
// GNU --wrap affects this probe and linked static-library call sites only.
// Dynamic Vulkan loader/layer/driver calls keep their native symbol behavior.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __real_vkQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit(VkQueue queue, std::uint32_t count,
															   const VkSubmitInfo* submits, VkFence fence)
{
	if (s_submissionFault.queue != VK_NULL_HANDLE && s_submissionFault.queue == queue)
	{
		s_submissionFault.queue = VK_NULL_HANDLE;
		s_submissionFault.fired = true;
		if (s_submissionFault.beforeReturn)
			s_submissionFault.beforeReturn(s_submissionFault.context);
		return s_submissionFault.result;
	}
	return __real_vkQueueSubmit(queue, count, submits, fence);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __real_vkEndCommandBuffer(VkCommandBuffer);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEndCommandBuffer(VkCommandBuffer command)
{
	if (std::exchange(s_failRecording, false))
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	return __real_vkEndCommandBuffer(command);
}

static constexpr std::uint64_t s_timeout = 5000000000ULL;
static constexpr std::array s_extensions{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
										 VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
										 VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
using Palette = std::array<std::array<std::uint8_t, 4>, 2>;
static constexpr Palette s_original{{{{255, 0, 0, 255}}, {{0, 255, 0, 255}}}};
static constexpr Palette s_resized{{{{255, 255, 0, 255}}, {{0, 255, 255, 255}}}};
static void Require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}
static void CheckSubmission(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
	{
		std::fprintf(stderr, "FAIL: %s returned %d; preserving uncertain submitted owners\n", operation, result);
		std::_Exit(1);
	}
	Check(result, operation);
}
static void VerifyPreservationOwnership()
{
	struct Observations
	{
		unsigned destroyed = 0;
	};
	struct NativeOwner
	{
		VulkanNativePreservation<NativeOwner, 2>::Reservation preservation{"CPU preservation proof is full"};
		std::shared_ptr<Observations> observations;
		std::shared_ptr<FrameReservation> charge;
		std::shared_ptr<const void> deviceDependency;
		NativeOwner(std::shared_ptr<Observations> observations, const FramePool& budget,
					std::shared_ptr<const void> dependency)
			: observations(std::move(observations)), charge(budget.Reserve()), deviceDependency(std::move(dependency))
		{
			Require(charge && budget.Consume(charge), "Reserve CPU preservation proof charge");
		}
		~NativeOwner()
		{
			++observations->destroyed;
		}
	};
	FramePool budget{2};
	auto observations = std::make_shared<Observations>();
	auto dependency = std::make_shared<int>(42);
	std::weak_ptr<const void> retainedDependency = dependency;
	auto uncertain = std::make_unique<NativeOwner>(observations, budget, dependency);
	auto completed = std::make_unique<NativeOwner>(observations, budget, std::make_shared<int>(17));
	dependency.reset();
	uncertain->preservation.Preserve(std::move(uncertain));
	Require(!uncertain && budget.InUse() == 2 && observations->destroyed == 0 && !retainedDependency.expired(),
			"Preservation destroyed an unknown owner, released its charge or dropped its native dependency");
	completed.reset();
	Require(budget.InUse() == 1 && observations->destroyed == 1,
			"Completed owner did not return its preservation admission and charge");
	auto replacement = std::make_unique<NativeOwner>(observations, budget, std::make_shared<int>(18));
	bool full = false;
	try
	{
		auto excess = std::make_unique<NativeOwner>(observations, budget, std::make_shared<int>(19));
	}
	catch (const VulkanError& error)
	{
		full = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
	}
	Require(full && budget.InUse() == 2 && !retainedDependency.expired(),
			"Unknown preservation admitted unbounded owners or returned its charged budget");
	replacement.reset();
	Require(budget.InUse() == 1 && observations->destroyed == 2,
			"Capacity rejection damaged the existing preserved owner");
	std::puts("case=bounded-native-preservation cpuOwnershipOnly=yes allocationFreeTransfer=yes "
			  "chargedOwnerRetained=yes PASS");
}

static void Await(const std::function<CompletionStatus()>& poll, const char* message)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		const auto status = poll();
		if (status == CompletionStatus::Complete)
			return;
		Require(status != CompletionStatus::Failed, message);
		Require(std::chrono::steady_clock::now() < deadline, message);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
template <class Exception, class Function> static void Rejected(Function operation, const char* message)
{
	try
	{
		operation();
	}
	catch (const Exception&)
	{
		return;
	}
	throw std::runtime_error(message);
}
class DeliverySubmissionFailure
{
  public:
	DeliverySubmissionFailure(VkQueue queue, VkResult result, void (*beforeReturn)(void*) noexcept = nullptr,
							  void* context = nullptr)
	{
		s_submissionFault = {queue, result, false, beforeReturn, context};
	}
	DeliverySubmissionFailure(const DeliverySubmissionFailure&) = delete;
	DeliverySubmissionFailure& operator=(const DeliverySubmissionFailure&) = delete;
	~DeliverySubmissionFailure()
	{
		s_submissionFault = {};
	}
	bool Fired() const noexcept
	{
		return s_submissionFault.fired;
	}
};
static std::shared_ptr<VulkanImageDelivery> ShareEventually(const std::shared_ptr<VulkanInspectionFrame>& frame)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		try
		{
			return frame->Share();
		}
		catch (const VulkanError& error)
		{
			Require(error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY, "Unexpected delivery admission error");
			Require(std::chrono::steady_clock::now() < deadline,
					"Delivery capacity failed to recover after real retirement");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}
static void VerifyKnownSubmissionFailure(const std::shared_ptr<VulkanInspectionFrame>& source)
{
	FramePool sourceBudget{1}, retainedBudget{1};
	Require(source->PromoteCaptureBudget(sourceBudget), "Classify source before injected queue OOM");
	for (int attempt = 0; attempt != 16; ++attempt)
	{
		const auto expected = attempt % 2 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_OUT_OF_HOST_MEMORY;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (;;)
		{
			bool fired = false;
			VkResult result = VK_SUCCESS;
			{
				DeliverySubmissionFailure injection(source->Image().device->queue, expected);
				try
				{
					source->Share();
				}
				catch (const VulkanError& error)
				{
					result = error.Result();
				}
				fired = injection.Fired();
			}
			if (fired)
			{
				Require(result == expected, "Known queue OOM did not propagate its exact error");
				break;
			}
			Require(result == VK_ERROR_OUT_OF_POOL_MEMORY,
					"Queue OOM injection missed its synchronous owned submission");
			Require(std::chrono::steady_clock::now() < deadline,
					"Known queue OOM permanently consumed global delivery capacity");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	Require(source->PromoteCaptureBudget(retainedBudget), "Move source away from rejected queue OOM copies");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (sourceBudget.InUse() != 0)
	{
		Require(std::chrono::steady_clock::now() < deadline, "Rejected OOM copies retained source capture budget");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	std::puts("case=delivery-submit-OOM attempts=16 host=8 device=8 nativeSubmissionsOnInjectedFailure=0 "
			  "sourceBudget=recovered PASS");
}

static void VerifyBackendKnownSubmissionFailure(const std::shared_ptr<VulkanDevice>& device)
{
	constexpr std::array errors{VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY};
	for (const auto expected : errors)
	{
		{
			VulkanBackend backend(device->binding, 2);
			Require(!backend.Sharing(), "Ordinary OOM backend unexpectedly required FD sharing");
			auto image = backend.CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {4, 4}, 2,
											 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
			VkResult result = VK_SUCCESS;
			bool fired = false;
			{
				DeliverySubmissionFailure injection(device->queue, expected);
				try
				{
					backend.InitializeImage(image);
				}
				catch (const VulkanError& error)
				{
					result = error.Result();
				}
				fired = injection.Fired();
			}
			Require(fired && result == expected, "Initialization OOM did not propagate exact rejected result");
			Require(backend.Drain(0) == VK_SUCCESS, "Rejected initialization OOM left unknown native work");
			Require(backend.Close() == XR_ERROR_LIMIT_REACHED, "Rejected initialization lost its real source reader");
			image.reset();
			Require(backend.Close() == XR_SUCCESS, "Rejected initialization OOM prevented safe backend teardown");
		}
		{
			VulkanBackend backend(device->binding, 2);
			CompositionPlan plan;
			plan.width = plan.height = 4;
			std::array<std::shared_ptr<VulkanFrame>, 2> frames;
			std::array<std::shared_ptr<const VulkanSubmission>, 2> completions;
			for (std::size_t index = 0; index < frames.size(); ++index)
			{
				Require(backend.Prepare(
							plan,
							[](XrSwapchain)
							{
								return std::shared_ptr<VulkanImage>{};
							},
							frames[index]) == XR_SUCCESS,
						"Prepare empty-draw batch OOM output");
				completions[index] = std::dynamic_pointer_cast<const VulkanSubmission>(frames[index]->Completion());
				Require(completions[index] && !frames[index]->Submitted(), "Prepared batch already appeared submitted");
			}
			XrResult result;
			bool fired;
			{
				DeliverySubmissionFailure injection(device->queue, expected);
				result = backend.Submit(frames);
				fired = injection.Fired();
			}
			Require(fired && result == XR_ERROR_RUNTIME_FAILURE, "Batch OOM changed native error mapping");
			for (std::size_t index = 0; index < frames.size(); ++index)
				Require(frames[index]->Submitted() && frames[index]->PollCompletion() == CompletionStatus::Failed &&
							completions[index]->NativeIdle() && completions[index]->Wait(0) == expected,
						"Known-OOM leader/follower invented completion or lost exact rejection result");
			Require(backend.Drain(0) == VK_SUCCESS, "Known-OOM batch did not retire without GPU acceptance");
			Require(backend.Close() == XR_ERROR_LIMIT_REACHED, "Known-OOM batch ignored retained output owners");
			frames = {};
			Require(backend.Close() == XR_ERROR_LIMIT_REACHED,
					"Known-OOM batch ignored retained native completion owners");
			completions = {};
			Require(backend.Close() == XR_SUCCESS, "Known-OOM batch stranded native frame or completion resources");
		}
	}
	std::puts("case=borrowed-backend-submit-OOM initialize=host+device batch=2-leader+follower "
			  "output=Failed-not-Complete Wait=exactOOM Drain=idle heldOwners=honored PASS");
}

static void VerifySubmissionPublicationBoundary(const std::shared_ptr<VulkanDevice>& owner)
{
	VulkanBackend backend(owner->binding, 1);
	CompositionPlan plan;
	plan.width = plan.height = 4;
	std::shared_ptr<VulkanFrame> frame;
	Require(backend.Prepare(
				plan,
				[](XrSwapchain)
				{
					return std::shared_ptr<VulkanImage>{};
				},
				frame) == XR_SUCCESS,
			"Prepare synthetic-loss publication boundary");
	struct Observation
	{
		std::shared_ptr<VulkanDevice> device;
		std::shared_ptr<const VulkanSubmission> completion;
		bool idle = true, observed = false;
		CompletionStatus status = CompletionStatus::Failed;
		static void BeforeReturn(void* pointer) noexcept
		{
			auto& state = *static_cast<Observation*>(pointer);
			// Synthetic observation only: the wrapper has not entered the
			// driver, and this dedicated backend will never submit GPU work.
			state.device->lost.store(true);
			state.idle = state.completion->NativeIdle();
			state.status = state.completion->PollCompletion();
			state.observed = true;
		}
	} state{backend.Device(), std::dynamic_pointer_cast<const VulkanSubmission>(frame->Completion())};
	Require(state.completion != nullptr, "Missing native completion for publication boundary");
	XrResult result;
	bool fired;
	{
		DeliverySubmissionFailure injection(owner->queue, VK_ERROR_DEVICE_LOST, &Observation::BeforeReturn, &state);
		result = backend.Submit(frame);
		fired = injection.Fired();
	}
	Require(fired && state.observed && !state.idle && state.status == CompletionStatus::Pending,
			"Lost-device observation reclaimed native ownership before submit-call publication");
	Require(result == XR_ERROR_RUNTIME_FAILURE && frame->Submitted() &&
				frame->PollCompletion() == CompletionStatus::Failed && state.completion->NativeIdle(),
			"Published synthetic loss did not remain Failed with safe native cleanup");
	Require(backend.Drain(0) == VK_ERROR_DEVICE_LOST && backend.Close() == XR_ERROR_LIMIT_REACHED,
			"Synthetic-loss teardown skipped real frame readers or waited for unsubmitted GPU work");
	frame.reset();
	Require(backend.Close() == XR_ERROR_LIMIT_REACHED, "Synthetic-loss teardown skipped native completion reader");
	state.completion.reset();
	Require(backend.Close() == XR_SUCCESS, "Synthetic-loss submission failed safe borrowed-backend teardown");
	std::puts("case=submit-publication-boundary loss=synthetic driverSubmissions=0 beforeReturn=Pending+NotIdle "
			  "afterReturn=Failed+Idle heldOwners=honored PASS");
}

struct DeliveryDevice
{
	DeliveryDevice() = default;
	DeliveryDevice(const DeliveryDevice&) = delete;
	DeliveryDevice& operator=(const DeliveryDevice&) = delete;
	std::shared_ptr<Instance> m_instance;
	VkDevice m_native = VK_NULL_HANDLE;
	~DeliveryDevice()
	{
		if (m_native)
			vkDestroyDevice(m_native, nullptr);
	}
};
static std::shared_ptr<VulkanDevice> ConsumerDevice(std::shared_ptr<Instance> instance,
													const VulkanExternalIdentity& identity)
{
	std::uint32_t count = 0;
	Check(vkEnumeratePhysicalDevices(instance->handle, &count, nullptr), "enumerate delivery consumer adapters");
	std::vector<VkPhysicalDevice> adapters(count);
	Check(vkEnumeratePhysicalDevices(instance->handle, &count, adapters.data()),
		  "enumerate delivery consumer adapters");
	for (auto physical : adapters)
	{
		VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids};
		vkGetPhysicalDeviceProperties2(physical, &properties);
		if (std::memcmp(ids.deviceUUID, identity.deviceUuid.data(), VK_UUID_SIZE) ||
			std::memcmp(ids.driverUUID, identity.driverUuid.data(), VK_UUID_SIZE))
			continue;
		const auto caps = VulkanExternalDevice::QueryFence(physical, VK_API_VERSION_1_1);
		Require(caps.CanImport() && caps.CanExport(), "Consumer adapter lacks advertised delivery fence capability");
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
		for (std::uint32_t family = 0; family != count; ++family)
			if (families[family].queueCount && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			{
				auto owner = std::make_shared<DeliveryDevice>();
				owner->m_instance = std::move(instance);
				const float priority = 1;
				VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
				queue.queueFamilyIndex = family;
				queue.queueCount = 1;
				queue.pQueuePriorities = &priority;
				VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
				create.queueCreateInfoCount = 1;
				create.pQueueCreateInfos = &queue;
				create.enabledExtensionCount = static_cast<std::uint32_t>(s_extensions.size());
				create.ppEnabledExtensionNames = s_extensions.data();
				Check(vkCreateDevice(physical, &create, nullptr, &owner->m_native),
					  "create independent delivery consumer");
				return std::make_shared<VulkanDevice>(
					VulkanBinding{owner->m_instance->handle, physical, owner->m_native, family, 0}, owner);
			}
	}
	throw std::runtime_error("No consumer adapter matches both delivery UUIDs");
}
// External fences may not let validation infer command retirement. An ordinary
// same-queue marker proves actual retirement independently for local cleanup.
static void SignalAndRetire(const std::shared_ptr<VulkanExternalFence>& external)
{
	const auto device = external->Device();
	VkFence marker = VK_NULL_HANDLE;
	VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	Check(vkCreateFence(device->binding.device, &create, nullptr, &marker), "create consumer marker fence");
	VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	const auto first = vkQueueSubmit(device->queue, 1, &empty, external->Get());
	if (first != VK_SUCCESS)
	{
		vkDestroyFence(device->binding.device, marker, nullptr);
		CheckSubmission(first, "submit consumer fence signal");
	}
	const auto second = vkQueueSubmit(device->queue, 1, &empty, marker);
	if (second != VK_SUCCESS || vkWaitForFences(device->binding.device, 1, &marker, VK_TRUE, s_timeout) != VK_SUCCESS)
	{
		std::fputs("FAIL: consumer signal retirement unknown; preserving submitted native owners\n", stderr);
		std::_Exit(1);
	}
	vkDestroyFence(device->binding.device, marker, nullptr);
	Await(
		[&]
		{
			return external->PollCompletion();
		},
		"External consumer fence failed to signal");
}
class DeliveryConsumerCopy
{
  public:
	DeliveryConsumerCopy(std::shared_ptr<VulkanDevice> owner, std::shared_ptr<VulkanImageDelivery> source,
						 bool readback, bool testRegistration = false)
		: m_device(std::move(owner)), m_delivery(std::move(source)),
		  m_external(m_device, s_extensions, VK_API_VERSION_1_1)
	{
		try
		{
			m_completion = m_external.CreateExportableFence();
			auto fenceFd = m_completion->ExportOnce();
			if (testRegistration)
			{
				Rejected<std::logic_error>(
					[&]
					{
						m_delivery->ExportHandles();
					},
					"Delivery exposed handles before registration");
				auto wrong = m_external.Identity();
				wrong.deviceUuid[0] ^= 1;
				const auto saved = fenceFd.Get();
				Rejected<VulkanError>(
					[&]
					{
						m_delivery->RegisterConsumer(wrong, fenceFd);
					},
					"Delivery accepted wrong consumer UUID");
				Require(fenceFd.Get() == saved, "Wrong-identity registration consumed caller FD");
			}
			auto duplicate = fenceFd.Duplicate();
			m_delivery->RegisterConsumer(m_external.Identity(), fenceFd);
			Require(!fenceFd, "Successful consumer registration did not consume caller FD");
			Rejected<std::logic_error>(
				[&]
				{
					m_delivery->RegisterConsumer(m_external.Identity(), duplicate);
				},
				"Duplicate consumer registration accepted");
			Require(static_cast<bool>(duplicate), "Duplicate registration consumed caller FD");
			auto handles = m_delivery->ExportHandles();
			{
				auto retry = m_delivery->ExportHandles();
				Require(retry.memory && retry.ready && retry.memory.Get() != handles.memory.Get() &&
							retry.ready.Get() != handles.ready.Get(),
						"Same-consumer export retry did not duplicate FD ownership");
			}
			Initialize(m_delivery->Descriptor(), handles, readback);
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}
	DeliveryConsumerCopy(std::shared_ptr<VulkanDevice> owner, const VulkanExternalImageDescriptor& descriptor,
						 VulkanDeliveryHandles handles, std::shared_ptr<VulkanExternalFence> fence, bool readback)
		: m_device(std::move(owner)), m_external(m_device, s_extensions, VK_API_VERSION_1_1),
		  m_completion(std::move(fence))
	{
		try
		{
			Initialize(descriptor, handles, readback);
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}
	// Explicit analysis of private storage produced by the production consumer.
	// Normal headless/companion consumption never constructs this readback job.
	DeliveryConsumerCopy(std::shared_ptr<VulkanDevice> owner, const VulkanImage& image)
		: m_privateImage(owner, &image), m_device(std::move(owner)),
		  m_external(m_device, s_extensions, VK_API_VERSION_1_1)
	{
		try
		{
			m_completion = m_external.CreateExportableFence();
			VkCommandPoolCreateInfo create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			create.queueFamilyIndex = m_device->binding.queueFamily;
			Check(vkCreateCommandPool(m_device->binding.device, &create, nullptr, &m_pool),
				  "create explicit private analysis pool");
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocate.commandPool = m_pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocate.commandBufferCount = 1;
			Check(vkAllocateCommandBuffers(m_device->binding.device, &allocate, &m_command),
				  "allocate private analysis command");
			VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			Check(vkCreateFence(m_device->binding.device, &fence, nullptr, &m_retirement),
				  "create private analysis fence");
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			Check(vkBeginCommandBuffer(m_command, &begin), "begin private analysis");
			RecordReadback();
			Check(vkEndCommandBuffer(m_command), "finish private analysis");
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}

	DeliveryConsumerCopy(const DeliveryConsumerCopy&) = delete;
	DeliveryConsumerCopy& operator=(const DeliveryConsumerCopy&) = delete;
	~DeliveryConsumerCopy()
	{
		Cleanup();
	}
	void Submit()
	{
		if (m_submitted)
			throw std::logic_error("Consumer copy was already submitted");
		const auto semaphore = m_ready ? m_ready->Get() : VK_NULL_HANDLE;
		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = m_ready ? 1 : 0;
		submit.pWaitSemaphores = &semaphore;
		submit.pWaitDstStageMask = &stage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &m_command;
		CheckSubmission(vkQueueSubmit(m_device->queue, 1, &submit, m_completion->Get()),
						"submit actual delivery consumer copy");
		m_submitted = true;
		VkSubmitInfo marker{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		CheckSubmission(vkQueueSubmit(m_device->queue, 1, &marker, m_retirement), "submit consumer retirement marker");
		m_markerSubmitted = true;
	}
	void Finish()
	{
		Require(m_submitted && m_markerSubmitted, "Consumer copy was not submitted");
		if (vkWaitForFences(m_device->binding.device, 1, &m_retirement, VK_TRUE, s_timeout) != VK_SUCCESS)
		{
			std::fputs("FAIL: delivery consumer GPU retirement unknown; preserving all submitted owners\n", stderr);
			std::_Exit(1);
		}
		m_complete = true;
		Await(
			[&]
			{
				return m_completion->PollCompletion();
			},
			"Consumer external fence did not match real copy completion");
		if (m_delivery)
			Await(
				[&]
				{
					return m_delivery->PollConsumerCompletion();
				},
				"Producer did not independently observe consumer completion");
	}
	std::weak_ptr<VulkanImageDelivery> ReleaseDelivery()
	{
		std::weak_ptr<VulkanImageDelivery> released = m_delivery;
		m_delivery.reset();
		return released;
	}
	CompletionStatus ConsumerCompletion() const noexcept
	{
		return m_completion->PollCompletion();
	}
	void Verify(const Palette& palette)
	{
		Require(m_complete && m_buffer != VK_NULL_HANDLE, "Pixel verification requires explicit completed readback");
		void* mapping = nullptr;
		Check(vkMapMemory(m_device->binding.device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapping),
			  "map explicit delivery analysis");
		try
		{
			if (!m_coherent)
			{
				VkMappedMemoryRange invalidate{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
				invalidate.memory = m_memory;
				invalidate.size = VK_WHOLE_SIZE;
				Check(vkInvalidateMappedMemoryRanges(m_device->binding.device, 1, &invalidate),
					  "invalidate delivery analysis memory");
			}
			const auto* bytes = static_cast<const std::uint8_t*>(mapping);
			Require(m_privateImage->layers == 2 && m_privateImage->format == VK_FORMAT_R8G8B8A8_SRGB,
					"Unexpected consumer palette image shape");
			const auto pixels = static_cast<std::size_t>(m_privateImage->extent.width) * m_privateImage->extent.height;
			for (std::size_t eye = 0; eye < 2; ++eye)
				for (std::size_t pixel = 0; pixel < pixels; ++pixel)
					for (std::size_t channel = 0; channel < 4; ++channel)
						Require(bytes[(eye * pixels + pixel) * 4 + channel] == palette[eye][channel],
								"Delivered private image changed exact retained RGBA");
		}
		catch (...)
		{
			vkUnmapMemory(m_device->binding.device, m_memory);
			throw;
		}
		vkUnmapMemory(m_device->binding.device, m_memory);
	}
	void VerifyBytes(std::span<const std::byte> expected)
	{
		const auto size = static_cast<std::size_t>(m_privateImage->extent.width) * m_privateImage->extent.height *
						  m_privateImage->layers * ColorPixelBytes(m_privateImage->format);
		Require(m_complete && m_buffer && expected.size() == size, "Explicit GPU/CPU comparison shape differs");
		void* mapping = nullptr;
		Check(vkMapMemory(m_device->binding.device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapping),
			  "map transported image comparison");
		try
		{
			if (!m_coherent)
			{
				VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
				range.memory = m_memory;
				range.size = VK_WHOLE_SIZE;
				Check(vkInvalidateMappedMemoryRanges(m_device->binding.device, 1, &range),
					  "invalidate transported image comparison");
			}
			Require(std::memcmp(mapping, expected.data(), size) == 0,
					"Transported GPU image differs from exact source export");
		}
		catch (...)
		{
			vkUnmapMemory(m_device->binding.device, m_memory);
			throw;
		}
		vkUnmapMemory(m_device->binding.device, m_memory);
	}

	const VulkanImage& Image() const noexcept
	{
		return *m_privateImage;
	}

  private:
	void Initialize(const VulkanExternalImageDescriptor& descriptor, VulkanDeliveryHandles& handles, bool readback)
	{
		m_imported = m_external.ImportImage(descriptor, handles.memory);
		m_ready = m_external.ImportSemaphore(descriptor.identity, handles.ready);
		Require(!handles.memory && !handles.ready, "Consumer imports did not consume their FD ownership");
		m_privateImage = VulkanImage::Create(
			m_device, descriptor.image.format, {descriptor.image.extent.width, descriptor.image.extent.height},
			descriptor.image.arrayLayers, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		VkCommandPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		poolCreate.queueFamilyIndex = m_device->binding.queueFamily;
		Check(vkCreateCommandPool(m_device->binding.device, &poolCreate, nullptr, &m_pool),
			  "create delivery consumer pool");
		VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocate.commandPool = m_pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		Check(vkAllocateCommandBuffers(m_device->binding.device, &allocate, &m_command),
			  "allocate delivery copy command");
		VkFenceCreateInfo marker{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		Check(vkCreateFence(m_device->binding.device, &marker, nullptr, &m_retirement),
			  "create delivery retirement fence");
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkBeginCommandBuffer(m_command, &begin), "begin delivery consumer copy");
		VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		acquire.oldLayout = acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		acquire.dstQueueFamilyIndex = m_device->binding.queueFamily;
		acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		acquire.image = m_imported->Image()->image;
		acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, descriptor.image.arrayLayers};
		vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
							 nullptr, 0, nullptr, 1, &acquire);
		VulkanImageBarrier(m_command, *m_privateImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
						   VK_ACCESS_TRANSFER_WRITE_BIT);
		VkImageCopy copy{};
		copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, descriptor.image.arrayLayers};
		copy.extent = descriptor.image.extent;
		vkCmdCopyImage(m_command, m_imported->Image()->image, VK_IMAGE_LAYOUT_GENERAL, m_privateImage->image,
					   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		auto release = acquire;
		release.srcQueueFamilyIndex = m_device->binding.queueFamily;
		release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		release.dstAccessMask = 0;
		vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
							 nullptr, 0, nullptr, 1, &release);
		if (readback)
			RecordReadback();
		Check(vkEndCommandBuffer(m_command), "finish delivery consumer copy");
	}

	void RecordReadback()
	{
		const auto size = static_cast<VkDeviceSize>(m_privateImage->extent.width) * m_privateImage->extent.height *
						  m_privateImage->layers * ColorPixelBytes(m_privateImage->format);
		VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		create.size = size;
		create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		Check(vkCreateBuffer(m_device->binding.device, &create, nullptr, &m_buffer),
			  "create explicit delivery analysis buffer");
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(m_device->binding.device, m_buffer, &requirements);
		const auto type = m_device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		VkPhysicalDeviceMemoryProperties properties;
		vkGetPhysicalDeviceMemoryProperties(m_device->binding.physicalDevice, &properties);
		m_coherent = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = type;
		Check(vkAllocateMemory(m_device->binding.device, &allocation, nullptr, &m_memory),
			  "allocate explicit delivery analysis memory");
		Check(vkBindBufferMemory(m_device->binding.device, m_buffer, m_memory, 0),
			  "bind explicit delivery analysis memory");
		VulkanImageBarrier(m_command, *m_privateImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
						   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, m_privateImage->layers};
		copy.imageExtent = {m_privateImage->extent.width, m_privateImage->extent.height, 1};
		vkCmdCopyImageToBuffer(m_command, m_privateImage->image, VK_IMAGE_LAYOUT_GENERAL, m_buffer, 1, &copy);
		VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.buffer = m_buffer;
		host.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
							 &host, 0, nullptr);
	}
	void Cleanup() noexcept
	{
		if (m_submitted && !m_complete)
		{
			const auto fence = m_markerSubmitted ? m_retirement : m_completion->Get();
			if (vkWaitForFences(m_device->binding.device, 1, &fence, VK_TRUE, s_timeout) != VK_SUCCESS)
			{
				std::fputs("FAIL: delivery consumer GPU retirement unknown; preserving all submitted owners\n", stderr);
				std::_Exit(1);
			}
		}
		if (m_retirement)
			vkDestroyFence(m_device->binding.device, m_retirement, nullptr);
		if (m_pool)
			vkDestroyCommandPool(m_device->binding.device, m_pool, nullptr);
		if (m_buffer)
			vkDestroyBuffer(m_device->binding.device, m_buffer, nullptr);
		if (m_memory)
			vkFreeMemory(m_device->binding.device, m_memory, nullptr);
		m_retirement = VK_NULL_HANDLE;
		m_pool = VK_NULL_HANDLE;
		m_buffer = VK_NULL_HANDLE;
		m_memory = VK_NULL_HANDLE;
	}
	std::shared_ptr<const VulkanImage> m_privateImage;
	const std::shared_ptr<VulkanDevice> m_device;
	std::shared_ptr<VulkanImageDelivery> m_delivery;
	VulkanExternalDevice m_external;
	std::shared_ptr<VulkanExternalImage> m_imported;
	std::shared_ptr<VulkanExternalSemaphore> m_ready;
	std::shared_ptr<VulkanExternalFence> m_completion;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_command = VK_NULL_HANDLE;
	VkFence m_retirement = VK_NULL_HANDLE;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
	bool m_coherent = false, m_submitted = false, m_markerSubmitted = false, m_complete = false;
};
void VerifyConsumerImage(const std::shared_ptr<VulkanInspectionFrame>& source, std::span<const std::byte> expected)
{
	auto provider = control::CreateVulkanImageInspectionProvider();
	auto reader = provider->Retain(source);
	Require(reader->Capabilities().gpuPreview, "Raw format cannot be delivered to the native consumer");
	const auto original = reader->Description();
	VulkanExternalIdentity identity;
	std::transform(original.identity.device.begin(), original.identity.device.end(), identity.deviceUuid.begin(),
				   [](auto value)
				   {
					   return std::to_integer<std::uint8_t>(value);
				   });
	std::transform(original.identity.driver.begin(), original.identity.driver.end(), identity.driverUuid.begin(),
				   [](auto value)
				   {
					   return std::to_integer<std::uint8_t>(value);
				   });
	auto instance = std::make_shared<Instance>();
	instance->Create();
	auto device = ConsumerDevice(instance, identity);
	control::VulkanImageConsumer consumer(device, s_extensions, VK_API_VERSION_1_1);
	auto delivery = reader->Share(consumer.Identity(), consumer.ConsumerFence());
	Await(
		[&]
		{
			return delivery->ProducerCompletion();
		},
		"Raw delivery producer did not complete");
	const auto description = delivery->Description();
	auto handles = delivery->PrepareHandles();
	delivery->MarkTransferred();
	auto malformed = description;
	malformed.format = VK_FORMAT_D32_SFLOAT;
	Rejected<std::invalid_argument>(
		[&]
		{
			consumer.Import(malformed, handles);
		},
		"Native color consumer accepted a depth format");
	for (const auto& handle : handles)
		Require(fcntl(static_cast<int>(handle->Value()), F_GETFD) >= 0,
				"Rejected raw format consumed caller descriptor");
	consumer.Import(description, handles);
	handles.clear();
	reader.reset();
	Require(description.format == original.format && description.width == original.width &&
				description.height == original.height && description.arrayLayers == original.arrayLayers &&
				consumer.Image().format == original.format && consumer.Views().size() == original.arrayLayers &&
				consumer.Views().front() && consumer.Sampler() && consumer.SampleReady() &&
				consumer.Image().device->binding.device != source->Image().device->binding.device,
			"Native consumer changed raw format, shape or private sampling resources");
	Require(delivery->ConsumerCompletion() == CompletionStatus::Pending,
			"Releasing raw reader invented native consumer completion");
	consumer.Submit();
	Await(
		[&]
		{
			return consumer.Poll();
		},
		"Production raw consumer copy failed");
	Require(consumer.NativeIdle(), "Ready raw consumer copy lacks native retirement");
	Await(
		[&]
		{
			return delivery->ConsumerCompletion();
		},
		"Raw delivery did not observe actual consumer completion");
	delivery.reset();
	DeliveryConsumerCopy readback(device, consumer.Image());
	readback.Submit();
	readback.Finish();
	readback.VerifyBytes(expected);
	std::printf("case=production-consumer-raw-format format=%u bytes=%zu privateCopy=exact "
				"channelsRangeAlpha=preserved viewsSampler=created deliveryReleased=yes PASS\n",
				original.format, expected.size());
}
void VerifyImageDelivery(std::shared_ptr<VulkanInspectionFrame> original,
						 std::shared_ptr<VulkanInspectionFrame> resized, bool verifyPixels,
						 const std::function<bool(SharedImageKey)>& retired)
{
	VerifyPreservationOwnership();
	Require(original && resized, "Missing retained images for delivery proof");
	if (!original->CanShare() || !resized->CanShare())
	{
		std::puts("SKIP: immutable GPU delivery requires optional enabled OPAQUE_FD consumer fence support");
		return;
	}
	VerifyKnownSubmissionFailure(original);
	std::array<std::shared_ptr<VulkanImageDelivery>, 8> deliveries;
	deliveries[0] = ShareEventually(original);
	deliveries[1] = ShareEventually(original);
	deliveries[2] = ShareEventually(resized);
	for (std::size_t index = 3; index < deliveries.size(); ++index)
		deliveries[index] = ShareEventually(original);
	Require(deliveries[0]->Key().pool.value && deliveries[0]->Key().image.value &&
				deliveries[0]->Key().pool == deliveries[1]->Key().pool &&
				deliveries[0]->Key().image != deliveries[1]->Key().image &&
				deliveries[0]->Key().pool != deliveries[2]->Key().pool,
			"Delivery resize/generation/allocation identities collapsed");
	bool full = false;
	try
	{
		original->Share();
	}
	catch (const VulkanError& error)
	{
		full = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
	}
	Require(full, "Delivery admission exceeded its independent eight-image capacity");
	for (const auto& delivery : deliveries)
		Await(
			[&]
			{
				return delivery->PollCompletion();
			},
			"Delivery producer GPU copy failed");
	auto instance = std::make_shared<Instance>();
	instance->Create();
	auto first = ConsumerDevice(instance, deliveries[0]->Descriptor().identity);
	auto second = ConsumerDevice(instance, deliveries[1]->Descriptor().identity);
	// Repeated constructor rejection must return both the ordinary consumer
	// budget and the pre-reserved preservation cell before the next attempt.
	for (unsigned index = 0; index < 12; ++index)
	{
		bool rejected = false;
		try
		{
			control::VulkanImageConsumer invalid(first, {}, VK_API_VERSION_1_1);
		}
		catch (const VulkanError& error)
		{
			rejected = error.Result() == VK_ERROR_EXTENSION_NOT_PRESENT;
		}
		Require(rejected, "Consumer without enabled logical extensions did not reject acquisition");
	}
	{
		std::vector<std::unique_ptr<control::VulkanImageConsumer>> consumers(SnapshotBudget::Capacity());
		for (auto& consumer : consumers)
			consumer = std::make_unique<control::VulkanImageConsumer>(first, s_extensions, VK_API_VERSION_1_1);
		Rejected<VulkanError>(
			[&]
			{
				control::VulkanImageConsumer excess(first, s_extensions, VK_API_VERSION_1_1);
			},
			"Native consumer admission exceeded the shared snapshot budget");
	}
	std::printf("case=consumer-acquisition-rollback rejected=12 usableCapacity=%zu PASS\n", SnapshotBudget::Capacity());
	VerifyBackendKnownSubmissionFailure(first);
	VerifySubmissionPublicationBoundary(ConsumerDevice(instance, deliveries[0]->Descriptor().identity));
	Require(first->binding.device != second->binding.device &&
				first->binding.device != original->Image().device->binding.device,
			"GPU delivery reused producer or another consumer logical device");
	DeliveryConsumerCopy one(first, deliveries[0], verifyPixels, true);
	DeliveryConsumerCopy two(second, deliveries[1], verifyPixels);
	for (const auto expected : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY})
	{
		DeliverySubmissionFailure injection(first->queue, expected);
		Rejected<std::runtime_error>(
			[&]
			{
				one.Submit();
			},
			"Consumer copy accepted injected queue OOM");
		Require(injection.Fired() && one.ConsumerCompletion() == CompletionStatus::Pending,
				"Rejected probe consumer copy invented GPU completion");
	}
	Require(deliveries[0]->PollConsumerCompletion() == CompletionStatus::Pending,
			"Unsubmitted consumer falsely complete");
	two.Submit();
	two.Finish();
	Require(deliveries[0]->PollCompletion() == CompletionStatus::Complete &&
				deliveries[0]->PollConsumerCompletion() == CompletionStatus::Pending &&
				deliveries[1]->PollConsumerCompletion() == CompletionStatus::Complete,
			"One consumer completion changed another consumer or producer readiness");
	one.Submit();
	one.Finish();
	Rejected<std::logic_error>(
		[&]
		{
			one.Submit();
		},
		"Completed consumer copy accepted a second submission");
	DeliveryConsumerCopy newer(first, deliveries[2], verifyPixels);
	newer.Submit();
	newer.Finish();
	Require(one.Image().image != newer.Image().image && one.Image().extent.width == 32 &&
				one.Image().extent.height == 24 && newer.Image().extent.width == 40 &&
				newer.Image().extent.height == 30,
			"Resized delivery mutated retained private consumer storage");
	{
		VulkanExternalDevice consumer(first, s_extensions, VK_API_VERSION_1_1);
		auto completed = consumer.CreateExportableFence();
		SignalAndRetire(completed);
		auto fd = completed->ExportOnce();
		const auto saved = fd.Get();
		Rejected<std::invalid_argument>(
			[&]
			{
				deliveries[3]->RegisterConsumer(consumer.Identity(), fd);
			},
			"Pre-signaled consumer fence accepted");
		Require(fd.Get() == saved, "Rejected completed consumer fence consumed FD");
		auto pending = consumer.CreateExportableFence();
		auto pendingFd = pending->ExportOnce();
		deliveries[3]->RegisterConsumer(consumer.Identity(), pendingFd);
		SignalAndRetire(pending);
		Await(
			[&]
			{
				return deliveries[3]->PollConsumerCompletion();
			},
			"Pending consumer fence completion was lost");
		Rejected<std::logic_error>(
			[&]
			{
				deliveries[3]->ExportHandles();
			},
			"Delivery exposed handles after consumer signaled before exposure");
	}
	// Returning an unexposed completed image recovers delivery budget without
	// changing or releasing the original capture's GPU allocation.
	FramePool sourceBudget{1}, retainedBudget{1};
	Require(original->PromoteCaptureBudget(sourceBudget), "Classify source for independent delivery retirement test");
	{
		// Preparing SCM_RIGHTS does not prove any peer received the handles.
		// A failed send must remain reclaimable after the producer fence even
		// though its registered consumer fence never signals.
		VulkanExternalDevice consumer(first, s_extensions, VK_API_VERSION_1_1);
		auto pending = consumer.CreateExportableFence();
		auto fd = pending->ExportOnce();
		deliveries[7]->RegisterConsumer(consumer.Identity(), fd);
		auto unsent = deliveries[7]->ExportHandles(false);
		Require(unsent.memory && unsent.ready, "Unsent transport preparation omitted handles");
	}
	deliveries[7].reset();
	std::shared_ptr<VulkanImageDelivery> replacement;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!replacement)
	{
		try
		{
			replacement = original->Share();
		}
		catch (const VulkanError& error)
		{
			Require(error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY, "Unexpected delivery budget recovery failure");
			Require(std::chrono::steady_clock::now() < deadline,
					"Released delivery budget did not recover independently");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	DeliveryConsumerCopy waiting(first, replacement, verifyPixels);
	Require(original->PromoteCaptureBudget(retainedBudget), "Move retained source image to its later budget");
	// No producer/delivery PollCompletion, Share, XR or native submission below.
	// Only the independent retirement observer can release the accepted copy's
	// old source reservation after its actual fence, even with an exposed but
	// not-yet-submitted consumer still retaining this delivery.
	const auto sourceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (sourceBudget.InUse() != 0)
	{
		Require(std::chrono::steady_clock::now() < sourceDeadline,
				"Pending consumer covertly retained original capture source budget");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Require(replacement->PollConsumerCompletion() == CompletionStatus::Pending,
			"Source retirement invented consumer completion");
	const auto abandonedKey = replacement->Key();
	const auto released = waiting.ReleaseDelivery();
	replacement.reset();
	Require(released.expired(), "Pending consumer test retained a hidden public delivery reader");
	// CPU release/disconnect does not prove consumer completion. Let the
	// background observer run repeatedly while every other slot remains held;
	// allocation must stay full until this consumer's actual GPU fence signals.
	for (int observation = 0; observation != 20; ++observation)
	{
		Require(waiting.ConsumerCompletion() == CompletionStatus::Pending,
				"Unsubmitted consumer unexpectedly completed");
		bool stillFull = false;
		try
		{
			original->Share();
		}
		catch (const VulkanError& error)
		{
			stillFull = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
		}
		Require(stillFull, "CPU release reused native delivery capacity before actual consumer completion");
		Require(!retired(abandonedKey), "Delivery retirement was reported before consumer completion");
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	waiting.Submit();
	waiting.Finish();
	std::shared_ptr<VulkanImageDelivery> recovered;
	const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!recovered)
	{
		try
		{
			recovered = original->Share();
		}
		catch (const VulkanError& error)
		{
			Require(error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY, "Unexpected post-consumer recovery failure");
			Require(std::chrono::steady_clock::now() < recoveryDeadline,
					"Actual consumer completion did not recover abandoned delivery capacity");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	Require(retired(abandonedKey), "Actual delivery destruction omitted its immutable image identity");
	Require(recovered->Key() != abandonedKey, "Recovered delivery reused an old immutable allocation identity");
	Await(
		[&]
		{
			return recovered->PollCompletion();
		},
		"Released delivery did not return independent budget");
	if (verifyPixels)
	{
		one.Verify(s_original);
		two.Verify(s_original);
		newer.Verify(s_resized);
		waiting.Verify(s_original);
	}
	std::printf("case=immutable-gpu-delivery consumers=2 independentCopies=yes producerAndConsumerCompletion=separate "
				"capacity=8 abandonedConsumer=retainedUntilFence captureSourceBudget=returnedBeforeConsumer "
				"resizeGeneration=new normalReadback=%s "
				"retainedAfterBorrowedAndServiceTeardown=yes PASS\n",
				verifyPixels ? "explicit-private-pixel-analysis" : "none");
}

void VerifyControlImage(const std::string& socket, std::uint64_t session, std::uint64_t generation,
						std::uint64_t capture, const std::filesystem::path& gate, bool verifyPixels)
{
	namespace wire = simxr::control;
	namespace tag = wire::tag;
	{
		wire::LocalControlClient client(socket, std::chrono::milliseconds{5000});
		const auto call = [&](wire::OpCode code, std::vector<wire::Field> fields = {},
							  std::vector<std::shared_ptr<wire::TransferHandle>> handles = {})
		{
			wire::ControlPacket packet;
			packet.message.opcode = static_cast<std::uint16_t>(code);
			packet.message.fields = std::move(fields);
			packet.handles = std::move(handles);
			auto response = client.Request(std::move(packet));
			const auto error = response.message.Find(tag::Error);
			if (!error || error->AsU32() != static_cast<std::uint32_t>(wire::ErrorCode::Ok))
			{
				const auto detail = response.message.Find(tag::Detail);
				throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
																	: "Native control operation failed");
			}
			return response;
		};
		const auto id = [](const wire::ControlPacket& packet, std::uint16_t field)
		{
			const auto value = packet.message.Find(field);
			Require(value && value->AsU64(), "Missing control uint64 result");
			return *value->AsU64();
		};
		const auto phase = [](const wire::ControlPacket& packet, std::uint16_t field)
		{
			const auto value = packet.message.Find(field);
			Require(value && value->AsText(), "Missing control phase result");
			return std::string(*value->AsText());
		};
		call(wire::OpCode::Hello,
			 {wire::Field::Text(tag::ClientName, "Native GPU consumer"),
			  wire::Field::U64(tag::RequiredCapabilities, static_cast<std::uint64_t>(wire::Capability::GpuPreview))});
		const auto retained = call(wire::OpCode::RetainImage, {wire::Field::U64(tag::Session, session),
															   wire::Field::U64(tag::Generation, generation),
															   wire::Field::U64(tag::CaptureId, capture)});
		const auto image = id(retained, tag::ImageReaderId);
		Require(id(retained, tag::Session) == session && id(retained, tag::Generation) == generation,
				"Retained image changed session identity");
		std::printf("READY image=%llu mode=%s\n", static_cast<unsigned long long>(image),
					verifyPixels ? "pixels" : "GPU-only");
		std::fflush(stdout);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!std::filesystem::exists(gate))
		{
			Require(std::chrono::steady_clock::now() < deadline,
					"Native consumer was not released after application teardown");
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		const auto sessions = call(wire::OpCode::Sessions);
		const auto list = sessions.message.Find(tag::Sessions);
		Require(list != nullptr, "Session response omitted its list");
		const auto decoded = wire::DecodeFields(list->value);
		Require(decoded.error == wire::DecodeError::None && decoded.fields.empty(),
				"Application public session survived teardown gate or returned an invalid list");
		const auto info = call(wire::OpCode::ImageInfo, {wire::Field::U64(tag::ImageReaderId, image)});
		Require(id(info, tag::Session) == session && id(info, tag::Generation) == generation,
				"Historical GPU reader changed original identity");
		const auto field = info.message.Find(tag::ImageDescriptor);
		Require(field != nullptr, "Image reader omitted description");
		const auto source = wire::DecodeImageDescription(*field);
		VulkanExternalIdentity identity;
		std::transform(source.identity.device.begin(), source.identity.device.end(), identity.deviceUuid.begin(),
					   [](auto value)
					   {
						   return std::to_integer<std::uint8_t>(value);
					   });
		std::transform(source.identity.driver.begin(), source.identity.driver.end(), identity.driverUuid.begin(),
					   [](auto value)
					   {
						   return std::to_integer<std::uint8_t>(value);
					   });
		auto instance = std::make_shared<Instance>();
		instance->Create();
		auto device = ConsumerDevice(instance, identity);
		{
			std::vector<std::unique_ptr<wire::VulkanImageConsumer>> bounded;
			for (std::size_t index = 0; index < SnapshotBudget::Capacity(); ++index)
				bounded.push_back(
					std::make_unique<wire::VulkanImageConsumer>(device, s_extensions, VK_API_VERSION_1_1));
			bool rejected = false;
			try
			{
				wire::VulkanImageConsumer excess(device, s_extensions, VK_API_VERSION_1_1);
			}
			catch (const VulkanError& error)
			{
				rejected = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
			}
			Require(rejected, "Private native consumer exceeded the shared snapshot budget");
			auto unsent = bounded.front()->ConsumerFence();
			bounded.front()->AbandonUnsent();
			Require(bounded.front()->NativeIdle(), "Unsent consumer fence required a GPU acknowledgement");
		}
		enum class ConsumerFailure
		{
			Recording,
			HostSubmission,
			DeviceSubmission
		};
		for (const auto failure :
			 {ConsumerFailure::Recording, ConsumerFailure::HostSubmission, ConsumerFailure::DeviceSubmission})
		{
			const auto expected = failure == ConsumerFailure::DeviceSubmission ? VK_ERROR_OUT_OF_DEVICE_MEMORY
																			   : VK_ERROR_OUT_OF_HOST_MEMORY;
			wire::VulkanImageConsumer rejected(device, s_extensions, VK_API_VERSION_1_1);
			const auto offered = call(wire::OpCode::ShareImage,
									  {wire::Field::U64(tag::ImageReaderId, image),
									   wire::Field::Bytes(tag::DeviceUuid, source.identity.device),
									   wire::Field::Bytes(tag::DriverUuid, source.identity.driver),
									   wire::Field::U32(tag::HandleCount, 1)},
									  {rejected.ConsumerFence()});
			const auto descriptor = offered.message.Find(tag::ImageDescriptor);
			Require(descriptor != nullptr, "Rejected-copy delivery omitted its image description");
			const auto description = wire::DecodeImageDescription(*descriptor);
			if (failure == ConsumerFailure::Recording)
			{
				s_failRecording = true;
				Rejected<VulkanError>(
					[&]
					{
						rejected.Import(description, offered.handles);
					},
					"Consumer import ignored command recording failure");
				Require(!s_failRecording, "Recording failure was not exercised");
				DeliverySubmissionFailure injection(device->queue, expected);
				Rejected<std::logic_error>(
					[&]
					{
						rejected.Submit();
					},
					"Partially prepared consumer accepted submission");
				Require(!injection.Fired(), "Partially prepared consumer reached the native queue");
			}
			else
			{
				rejected.Import(description, offered.handles);
				VkResult observed = VK_SUCCESS;
				{
					DeliverySubmissionFailure injection(device->queue, expected);
					try
					{
						rejected.Submit();
					}
					catch (const VulkanError& error)
					{
						observed = error.Result();
					}
					Require(injection.Fired() && observed == expected,
							"Native consumer OOM did not preserve exact rejection");
				}
				Require(rejected.Poll() == CompletionStatus::Failed, "Rejected native copy falsely published Ready");
			}
			rejected.CancelUnused();
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!rejected.NativeIdle())
			{
				Require(rejected.Poll() != CompletionStatus::Complete && std::chrono::steady_clock::now() < until,
						"Rejected copy became Ready or failed to acknowledge its unused consumer fence");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const auto delivery = id(offered, tag::DeliveryId);
			Require(phase(call(wire::OpCode::DeliveryInfo, {wire::Field::U64(tag::DeliveryId, delivery)}),
						  tag::ConsumerPhase) == "complete",
					"Unused rejected consumer did not signal its real offered fence");
			call(wire::OpCode::ReleaseDelivery, {wire::Field::U64(tag::DeliveryId, delivery)});
		}
		wire::VulkanImageConsumer consumer(device, s_extensions, VK_API_VERSION_1_1);
		const auto shared = call(
			wire::OpCode::ShareImage,
			{wire::Field::U64(tag::ImageReaderId, image), wire::Field::Bytes(tag::DeviceUuid, source.identity.device),
			 wire::Field::Bytes(tag::DriverUuid, source.identity.driver), wire::Field::U32(tag::HandleCount, 1)},
			{consumer.ConsumerFence()});
		const auto delivery = id(shared, tag::DeliveryId);
		const auto described = shared.message.Find(tag::ImageDescriptor);
		Require(described && shared.handles.size() == 2 && id(shared, tag::PoolGeneration) &&
					id(shared, tag::SharedImageId),
				"GPU delivery omitted strong identity or native handles");
		const auto value = wire::DecodeImageDescription(*described);
		Require(value.identity == source.identity &&
					value.handleKind == static_cast<std::uint32_t>(wire::TransferHandleKind::OpaqueFd) &&
					(value.format == VK_FORMAT_R8G8B8A8_UNORM || value.format == VK_FORMAT_R8G8B8A8_SRGB) &&
					value.width == source.width && value.height == source.height &&
					value.arrayLayers == source.arrayLayers && value.depth == 1 && value.mipLevels == 1 &&
					value.samples == 1 && value.flags == 0 && value.tiling == VK_IMAGE_TILING_OPTIMAL &&
					value.imageType == VK_IMAGE_TYPE_2D && value.layout == VK_IMAGE_LAYOUT_GENERAL,
				"Unsupported or substituted native delivery image specification");
		for (unsigned invalid = 0; invalid != 5; ++invalid)
		{
			auto malformed = value;
			if (invalid == 0)
				malformed.identity.driver[0] ^= std::byte{1};
			if (invalid == 1)
				malformed.depth = 2;
			if (invalid == 2)
				malformed.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
			if (invalid == 3)
				malformed.mipLevels = 2;
			if (invalid == 4)
				malformed.handleKind = 0;
			Rejected<std::invalid_argument>(
				[&]
				{
					consumer.Import(malformed, shared.handles);
				},
				"Malformed native image description was accepted");
			for (const auto& handle : shared.handles)
				Require(fcntl(static_cast<int>(handle->Value()), F_GETFD) >= 0,
						"Rejected native metadata consumed caller descriptor");
		}
		consumer.Import(value, shared.handles);
		std::optional<std::uint64_t> exportId;
		if (verifyPixels)
			exportId =
				id(call(wire::OpCode::StartExport, {wire::Field::U64(tag::ImageReaderId, image)}), tag::ExportId);
		call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, image)});
		Require(phase(call(wire::OpCode::DeliveryInfo, {wire::Field::U64(tag::DeliveryId, delivery)}),
					  tag::ConsumerPhase) == "pending",
				"CPU release invented external consumer completion");
		consumer.Submit();
		Await(
			[&]
			{
				return consumer.Poll();
			},
			"Production native consumer copy failed");
		Require(phase(call(wire::OpCode::DeliveryInfo, {wire::Field::U64(tag::DeliveryId, delivery)}),
					  tag::ConsumerPhase) == "complete",
				"Runtime failed to observe native consumer completion independently");
		call(wire::OpCode::ReleaseDelivery, {wire::Field::U64(tag::DeliveryId, delivery)});
		if (exportId)
		{
			const auto exportDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			wire::ControlPacket status;
			do
			{
				status = call(wire::OpCode::ExportInfo, {wire::Field::U64(tag::ExportId, *exportId)});
				Require(phase(status, tag::Phase) != "failed" && std::chrono::steady_clock::now() < exportDeadline,
						"Explicit delayed export failed");
				if (phase(status, tag::Phase) == "pending")
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (phase(status, tag::Phase) == "pending");
			const auto total = id(status, tag::TotalBytes);
			Require(total == std::uint64_t{value.width} * value.height * value.arrayLayers * 4,
					"Export description substituted another shape");
			Require(total <= std::numeric_limits<std::size_t>::max(), "Explicit export exceeds addressable memory");
			std::vector<std::byte> reference;
			reference.reserve(static_cast<std::size_t>(total));
			while (reference.size() < total)
			{
				const auto chunk =
					call(wire::OpCode::ReadExport,
						 {wire::Field::U64(tag::ExportId, *exportId), wire::Field::U64(tag::Offset, reference.size()),
						  wire::Field::U64(tag::Length, wire::MaxPayload - 4096)});
				const auto bytes = chunk.message.Find(tag::Bytes);
				Require(bytes && !bytes->value.empty() && id(chunk, tag::Offset) == reference.size() &&
							id(chunk, tag::Length) == bytes->value.size() &&
							bytes->value.size() <= total - reference.size(),
						"Invalid explicit export chunk identity");
				reference.insert(reference.end(), bytes->value.begin(), bytes->value.end());
			}
			DeliveryConsumerCopy explicitReadback(device, consumer.Image());
			explicitReadback.Submit();
			explicitReadback.Finish();
			explicitReadback.VerifyBytes(reference);
			call(wire::OpCode::ReleaseExport, {wire::Field::U64(tag::ExportId, *exportId)});
		}
	}
	Require(s_validationErrors.load() == 0, "Native control consumer validation failed through teardown");
	std::printf("PASS production endpoint native GPU consumer originalSession=%llu generation=%llu capture=%llu "
				"applicationDeviceDestroyed=yes mode=%s validationErrors=0\n",
				static_cast<unsigned long long>(session), static_cast<unsigned long long>(generation),
				static_cast<unsigned long long>(capture),
				verifyPixels ? "explicit-source-and-private-pixels" : "no-readback");
}
} // namespace simxr::probe
