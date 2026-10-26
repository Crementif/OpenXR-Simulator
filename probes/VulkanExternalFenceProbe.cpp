#include "Pch.h"
#include "VulkanProbeInstance.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <stdexcept>
#include <thread>

namespace simxr::probe
{
static constexpr std::array s_sharingExtensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension};
static constexpr std::array s_fenceExtensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension,
											  VulkanExternalFenceExtension};
static constexpr std::uint64_t s_timeout = 5000000000ULL;
static bool ValidNonInherited(VulkanNativeHandle handle)
{
#ifdef _WIN32
	DWORD flags = 0;
	return GetHandleInformation(handle, &flags) && !(flags & HANDLE_FLAG_INHERIT);
#else
	const auto flags = fcntl(handle, F_GETFD);
	return flags >= 0 && (flags & FD_CLOEXEC);
#endif
}
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
template <class Function> static void Rejected(Function operation, VkResult expected, const char* message)
{
	try
	{
		operation();
	}
	catch (const VulkanError& error)
	{
		Require(error.Result() == expected, message);
		return;
	}
	throw std::runtime_error(message);
}
struct FenceDevice
{
	VkDevice m_native = VK_NULL_HANDLE;
	bool m_timeline = false;
	std::shared_ptr<VulkanDevice> m_device;
	FenceDevice(VkInstance instance, VkPhysicalDevice physical, std::uint32_t family,
				std::span<const char* const> extensions, bool controlledGate = false, bool coreTimeline = false)
	{
		const float priority = 1;
		std::vector<const char*> enabled(extensions.begin(), extensions.end());
		VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
		if (controlledGate)
		{
			std::uint32_t count = 0;
			Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr),
				  "query timeline gate extension");
			std::vector<VkExtensionProperties> names(count);
			Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, names.data()),
				  "query timeline gate extension");
			if (coreTimeline || std::any_of(names.begin(), names.end(),
											[](const auto& name)
											{
												return std::strcmp(name.extensionName,
																   VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0;
											}))
			{
				VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timelineFeatures};
				vkGetPhysicalDeviceFeatures2(physical, &features);
				m_timeline = timelineFeatures.timelineSemaphore;
				if (m_timeline && !coreTimeline)
					enabled.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
			}
		}
		VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queue.queueFamilyIndex = family;
		queue.queueCount = 1;
		queue.pQueuePriorities = &priority;
		VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		create.queueCreateInfoCount = 1;
		create.pQueueCreateInfos = &queue;
		create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
		create.ppEnabledExtensionNames = enabled.data();
		create.pNext = m_timeline ? &timelineFeatures : nullptr;
		Check(vkCreateDevice(physical, &create, nullptr, &m_native), "create fence proof device");
		try
		{
			m_device = std::make_shared<VulkanDevice>(VulkanBinding{instance, physical, m_native, family, 0});
		}
		catch (...)
		{
			vkDestroyDevice(m_native, nullptr);
			throw;
		}
	}
	FenceDevice(const FenceDevice&) = delete;
	FenceDevice& operator=(const FenceDevice&) = delete;
	~FenceDevice()
	{
		m_device.reset();
		vkDestroyDevice(m_native, nullptr);
	}
};
// An optional host-signaled timeline gate holds consumer-only GPU work pending.
// Neither the observer nor an application/inspection queue waits on the consumer.
struct ConsumerFenceWork
{
	std::shared_ptr<VulkanDevice> m_device;
	std::shared_ptr<VulkanExternalFence> m_completion;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_command = VK_NULL_HANDLE;
	VkSemaphore m_gate = VK_NULL_HANDLE;
	VkFence m_retirementFence = VK_NULL_HANDLE;
	PFN_vkSignalSemaphoreKHR m_signalSemaphore = nullptr;
	bool m_gateReleased = false;
	bool m_retirementSubmitted = false;
	bool m_submitted = false;
	ConsumerFenceWork(std::shared_ptr<VulkanDevice> owner, std::shared_ptr<VulkanExternalFence> fence, bool timeline)
		: m_device(std::move(owner)), m_completion(std::move(fence))
	{
		try
		{
			VkCommandPoolCreateInfo create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			create.queueFamilyIndex = m_device->binding.queueFamily;
			Check(vkCreateCommandPool(m_device->binding.device, &create, nullptr, &m_pool),
				  "create consumer command pool");
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocate.commandPool = m_pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocate.commandBufferCount = 1;
			Check(vkAllocateCommandBuffers(m_device->binding.device, &allocate, &m_command),
				  "allocate consumer command");

			if (timeline)
			{
				m_signalSemaphore = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
					vkGetDeviceProcAddr(m_device->binding.device, "vkSignalSemaphoreKHR"));
				Require(m_signalSemaphore != nullptr, "Consumer timeline command unavailable");
				VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
				type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
				VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
				Check(vkCreateSemaphore(m_device->binding.device, &semaphore, nullptr, &m_gate),
					  "create consumer timeline gate");
			}
			VkFenceCreateInfo fenceCreate{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			Check(vkCreateFence(m_device->binding.device, &fenceCreate, nullptr, &m_retirementFence),
				  "create internal consumer retirement fence");
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			Check(vkBeginCommandBuffer(m_command, &begin), "begin consumer command");
			vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
								 0, nullptr, 0, nullptr, 0, nullptr);
			Check(vkEndCommandBuffer(m_command), "end consumer command");
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}

	void Cleanup() noexcept
	{
		if (m_submitted)
		{
			if (m_gate && !m_gateReleased)
			{
				VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
				signal.semaphore = m_gate;
				signal.value = 1;
				if (m_signalSemaphore(m_device->binding.device, &signal) != VK_SUCCESS)
					std::_Exit(1);
			}
			const auto fence = m_retirementSubmitted ? m_retirementFence : m_completion->Get();
			if (vkWaitForFences(m_device->binding.device, 1, &fence, VK_TRUE, s_timeout) != VK_SUCCESS)
			{
				std::fprintf(stderr, "FAIL: consumer completion unknown; retaining native owners until process exit\n");
				std::_Exit(1);
			}
		}
		if (m_retirementFence)
			vkDestroyFence(m_device->binding.device, m_retirementFence, nullptr);
		if (m_gate)
			vkDestroySemaphore(m_device->binding.device, m_gate, nullptr);
		if (m_pool)
			vkDestroyCommandPool(m_device->binding.device, m_pool, nullptr);
		m_gate = VK_NULL_HANDLE;
		m_pool = VK_NULL_HANDLE;
	}
	ConsumerFenceWork(const ConsumerFenceWork&) = delete;
	ConsumerFenceWork& operator=(const ConsumerFenceWork&) = delete;
	~ConsumerFenceWork()
	{
		Cleanup();
	}

	void Submit()
	{
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		const std::uint64_t value = 1;
		VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		if (m_gate)
		{
			timeline.waitSemaphoreValueCount = 1;
			timeline.pWaitSemaphoreValues = &value;
			submit.pNext = &timeline;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &m_gate;
			submit.pWaitDstStageMask = &stage;
		}
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &m_command;
		CheckSubmission(vkQueueSubmit(m_device->queue, 1, &submit, m_completion->Get()), "submit consumer-only work");
		m_submitted = true;
		// This private marker proves native retirement to validation as well as
		// the driver; external-payload observers alone do not track command use.
		VkSubmitInfo marker{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		CheckSubmission(vkQueueSubmit(m_device->queue, 1, &marker, m_retirementFence),
						"submit internal retirement marker");
		m_retirementSubmitted = true;
	}
	void Release()
	{
		if (!m_gate)
			return;
		VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		signal.semaphore = m_gate;
		signal.value = 1;
		Check(m_signalSemaphore(m_device->binding.device, &signal), "release consumer timeline gate");
		m_gateReleased = true;
	}
};
static void ExerciseMemoryAndSemaphore(VulkanExternalDevice& exporter, VulkanExternalDevice& importer)
{
	VulkanExternalImageSpec spec;
	spec.extent = {8, 8, 1};
	auto image = exporter.CreateExportable(spec);
	auto memory = image->ExportMemory();
	Require(memory && ValidNonInherited(memory.Get()), "Export non-inherited image memory");
	auto invalid = image->Descriptor();
	invalid.identity.deviceUuid[0] ^= 1;
	Rejected(
		[&]
		{
			importer.ImportImage(invalid, memory);
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Reject another adapter's image handle");
	Require(memory && ValidNonInherited(memory.Get()), "Rejected import retains caller memory handle");
	auto importedImage = importer.ImportImage(image->Descriptor(), memory);
	Require(!memory && importedImage->Descriptor().allocationSize == image->Descriptor().allocationSize,
			"Import exact dedicated allocation and consume caller ownership on success");
	// Warm capability lookup must still validate a changed shape and each allocation descriptor.
	auto oversized = spec;
	oversized.extent.width = std::numeric_limits<std::uint32_t>::max();
	Rejected(
		[&]
		{
			exporter.CreateExportable(oversized);
		},
		VK_ERROR_FORMAT_NOT_SUPPORTED, "Reject oversized extent after successful format query");
	memory = image->ExportMemory();
	invalid = image->Descriptor();
	--invalid.allocationSize;
	Rejected(
		[&]
		{
			importer.ImportImage(invalid, memory);
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Cached image capability bypassed allocation validation");
	Require(memory && ValidNonInherited(memory.Get()), "Rejected cached import consumed caller memory handle");
	auto repeated = importer.ImportImage(image->Descriptor(), memory);
	Require(!memory && repeated->Image()->extent.width == spec.extent.width,
			"Valid retry after cached import rejection failed");
#ifdef _WIN32
	// Repeat exports while unrelated handles are recycled. Each transfer must
	// remain independently owned and refer to the original image allocation.
	for (unsigned iteration = 0; iteration < 64; ++iteration)
	{
		auto largerSpec = spec;
		largerSpec.extent = {256, 256, 1};
		auto larger = exporter.CreateExportable(largerSpec);
		auto source = image->ExportMemory();
		auto recycled = larger->ExportMemory();
		recycled.Reset();
		auto imported = importer.ImportImage(image->Descriptor(), source);
		Require(!source && imported->Descriptor().allocationSize == image->Descriptor().allocationSize,
				"Recycled Win32 handle changed the imported allocation");
	}
#endif
	auto semaphore = exporter.CreateExportableSemaphore();
	auto handle = semaphore->ExportOnce();
	Require(handle && ValidNonInherited(handle.Get()), "Export non-inherited semaphore");
	auto importedSemaphore = importer.ImportSemaphore(exporter.Identity(), handle);
	Require(!handle, "Successful semaphore import consumes caller ownership");
	const auto signal = semaphore->Get(), wait = importedSemaphore->Get();
	VkSubmitInfo producer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	producer.signalSemaphoreCount = 1;
	producer.pSignalSemaphores = &signal;
	CheckSubmission(vkQueueSubmit(exporter.Device()->queue, 1, &producer, VK_NULL_HANDLE), "Signal external semaphore");
	// Drain accepted work before any exceptional teardown of the signal owner.
	if (vkQueueWaitIdle(exporter.Device()->queue) != VK_SUCCESS)
		std::_Exit(1);
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkSubmitInfo consumer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	consumer.waitSemaphoreCount = 1;
	consumer.pWaitSemaphores = &wait;
	consumer.pWaitDstStageMask = &stage;
	CheckSubmission(vkQueueSubmit(importer.Device()->queue, 1, &consumer, VK_NULL_HANDLE), "Wait imported semaphore");
	if (vkQueueWaitIdle(importer.Device()->queue) != VK_SUCCESS)
		std::_Exit(1);
	std::puts("PASS dedicated image import and two-device binary semaphore signal/wait");
}
struct TimelineGate
{
	VkDevice m_device;
	VkSemaphore m_handle = VK_NULL_HANDLE;
	explicit TimelineGate(VkDevice device) : m_device(device) {}
	TimelineGate(const TimelineGate&) = delete;
	TimelineGate& operator=(const TimelineGate&) = delete;
	~TimelineGate()
	{
		if (m_handle)
			vkDestroySemaphore(m_device, m_handle, nullptr);
	}
};
static bool ExerciseTimeline(VkInstance instance, VkPhysicalDevice physical, std::uint32_t family, std::uint32_t api)
{
	const auto caps = VulkanExternalDevice::QueryTimeline(physical, api);
	if (!caps.CanImportSemaphore() || !caps.CanExportSemaphore())
		return false;
	const bool coreTimeline = api >= VK_API_VERSION_1_2;
	constexpr std::array names{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension,
							   VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
	const auto extensions = std::span(names).first(coreTimeline ? 2 : names.size());
	FenceDevice consumer(instance, physical, family, s_sharingExtensions, true, coreTimeline);
	FenceDevice observer(instance, physical, family, s_sharingExtensions, true, coreTimeline);
	Require(consumer.m_timeline && observer.m_timeline, "Timeline capability must enable device feature");
	VulkanExternalDevice exporter(consumer.m_device, extensions, api, true);
	VulkanExternalDevice importer(observer.m_device, extensions, api, true);
	VulkanExternalDevice disabled(observer.m_device, extensions, api);
	Rejected(
		[&]
		{
			disabled.CreateExportableTimeline();
		},
		VK_ERROR_FEATURE_NOT_PRESENT, "Unconfirmed logical-device timeline feature allowed export");
	auto completion = exporter.CreateExportableTimeline();
	auto handle = completion->ExportOnce();
	Require(handle && ValidNonInherited(handle.Get()), "Timeline export must not be inheritable");
	Rejected(
		[&]
		{
			completion->ExportOnce();
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Timeline exported twice");
	auto wrong = exporter.Identity();
	wrong.deviceUuid[0] ^= 1;
	Rejected(
		[&]
		{
			importer.ImportTimeline(wrong, handle);
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Timeline accepted mismatched adapter");
	Require(bool(handle), "Rejected timeline import consumed caller ownership");
	auto imported = importer.ImportTimeline(exporter.Identity(), handle);
	Require(!handle, "Successful timeline import retained caller handle");
	Require(imported->PollCompletion() == CompletionStatus::Pending, "Unsignaled imported timeline completed");
	// Hold the consumer queue on a private host-controlled gate. The observer
	// only queries the shared counter and never submits work or waits on it.
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
	TimelineGate gate(consumer.m_native);
	Check(vkCreateSemaphore(consumer.m_native, &create, nullptr, &gate.m_handle), "create timeline proof gate");
	const auto signal = completion->Get();
	const std::uint64_t value = 1;
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
	timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues = timeline.pSignalSemaphoreValues = &value;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
	submit.waitSemaphoreCount = submit.signalSemaphoreCount = 1;
	submit.pWaitSemaphores = &gate.m_handle;
	submit.pWaitDstStageMask = &stage;
	submit.pSignalSemaphores = &signal;
	CheckSubmission(vkQueueSubmit(consumer.m_device->queue, 1, &submit, VK_NULL_HANDLE),
					"submit timeline completion proof");
	std::atomic<bool> stayedPending{true};
	std::array<std::jthread, 4> readers;
	for (auto& reader : readers)
		reader = std::jthread(
			[&]
			{
				for (int poll = 0; poll < 100; ++poll)
					if (completion->PollCompletion() != CompletionStatus::Pending ||
						imported->PollCompletion() != CompletionStatus::Pending)
						stayedPending.store(false);
			});
	for (auto& reader : readers)
		reader.join();
	auto release = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
		vkGetDeviceProcAddr(consumer.m_native, coreTimeline ? "vkSignalSemaphore" : "vkSignalSemaphoreKHR"));
	VkSemaphoreSignalInfo releaseGate{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	releaseGate.semaphore = gate.m_handle;
	releaseGate.value = 1;
	if (!release || release(consumer.m_native, &releaseGate) != VK_SUCCESS ||
		vkQueueWaitIdle(consumer.m_device->queue) != VK_SUCCESS)
		std::_Exit(1);
	Require(stayedPending.load(), "Timeline completed while consumer work was gated");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (imported->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	Require(imported->PollCompletion() == CompletionStatus::Complete, "Imported timeline missed consumer completion");
	std::puts("PASS external timeline pending->complete; observer queue submissions=0; one-shot export and retained "
			  "failed-import ownership");
	return true;
}
static void Exercise(VkInstance instance, VkPhysicalDevice physical, std::uint32_t family)
{
	FenceDevice consumer(instance, physical, family, s_fenceExtensions, true);
	FenceDevice observer(instance, physical, family, s_fenceExtensions);
	FenceDevice disabled(instance, physical, family, s_sharingExtensions);
	VulkanExternalDevice exporter(consumer.m_device, s_fenceExtensions, VK_API_VERSION_1_1);
	VulkanExternalDevice importer(observer.m_device, s_fenceExtensions, VK_API_VERSION_1_1);
	VulkanExternalDevice unavailable(disabled.m_device, s_sharingExtensions, VK_API_VERSION_1_1);
	auto native = exporter.CreateExportableFence();
	Require(native->Device() == consumer.m_device && native->Identity() == exporter.Identity(),
			"Fence owner identity mismatch");
	Require(native->PollCompletion() == CompletionStatus::Pending, "Unsubmitted fence falsely complete");
	auto fd = native->ExportOnce();
	Require(fd && ValidNonInherited(fd.Get()), "Exported fence handle must be close-on-exec");
	Rejected(
		[&]
		{
			native->ExportOnce();
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Second fence export accepted");
	auto mismatched = exporter.Identity();
	mismatched.driverUuid[0] ^= 1;
	const auto original = fd.Get();
	Rejected(
		[&]
		{
			importer.ImportFence(mismatched, fd);
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Wrong driver identity accepted");
	Require(fd.Get() == original && ValidNonInherited(original), "Rejected identity consumed caller handle");
	mismatched = exporter.Identity();
	mismatched.deviceUuid[0] ^= 1;
	Rejected(
		[&]
		{
			importer.ImportFence(mismatched, fd);
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Wrong device identity accepted");
	Rejected(
		[&]
		{
			unavailable.CreateExportableFence();
		},
		VK_ERROR_EXTENSION_NOT_PRESENT, "Disabled fence extension allowed export");
	Rejected(
		[&]
		{
			unavailable.ImportFence(exporter.Identity(), fd);
		},
		VK_ERROR_EXTENSION_NOT_PRESENT, "Disabled fence extension allowed import");
	Require(fd.Get() == original && ValidNonInherited(original), "Disabled fence extension consumed caller handle");
	// Optional fence absence leaves the existing semaphore path usable.
	auto semaphore = unavailable.CreateExportableSemaphore();
	Require(semaphore->Get() != VK_NULL_HANDLE, "Optional fence requirement broke semaphore sharing");
	auto imported = importer.ImportFence(exporter.Identity(), fd);
	Require(!fd && imported->Device() == observer.m_device && imported->Identity() == native->Identity(),
			"Successful fence import did not transfer handle ownership");
	Rejected(
		[&]
		{
			imported->ExportOnce();
		},
		VK_ERROR_INVALID_EXTERNAL_HANDLE, "Imported fence unexpectedly re-exportable");
	Require(imported->PollCompletion() == CompletionStatus::Pending, "Imported unsignaled fence falsely complete");
	ConsumerFenceWork work(consumer.m_device, native, consumer.m_timeline);
	std::printf("controlledConsumerGate=%s observerSubmissions=0\n",
				consumer.m_timeline ? "timeline" : "unavailable; pre-submit pending verified");
	work.Submit();
	for (int poll = 0; consumer.m_timeline && poll != 100; ++poll)
		Require(native->PollCompletion() == CompletionStatus::Pending &&
					imported->PollCompletion() == CompletionStatus::Pending,
				"Consumer fence completed before host released its consumer-only timeline gate");
	if (consumer.m_timeline)
	{
		std::atomic<bool> pending{true};
		std::array<std::jthread, 4> readers;
		for (auto& reader : readers)
			reader = std::jthread(
				[&]
				{
					for (int poll = 0; poll != 100; ++poll)
						if (imported->PollCompletion() != CompletionStatus::Pending)
							pending.store(false);
				});
		for (auto& reader : readers)
			reader.join();
		Require(pending.load(), "Concurrent consumer observers falsely completed pending work");
	}
	work.Release();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (native->PollCompletion() != CompletionStatus::Complete ||
		   imported->PollCompletion() != CompletionStatus::Complete)
	{
		Require(native->PollCompletion() != CompletionStatus::Failed &&
					imported->PollCompletion() != CompletionStatus::Failed,
				"Cross-device fence reported failed completion");
		Require(std::chrono::steady_clock::now() < deadline, "Cross-device fence completion timed out");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Only the fence payload is needed after actual consumer work retirement.
	for (int poll = 0; poll != 100; ++poll)
		Require(imported->PollCompletion() == CompletionStatus::Complete, "Terminal fence observation regressed");
	std::printf(
		"PASS: two-device opaque-native fence pending->complete, one-shot export, success-only handle consumption, "
		"optional disabled path; no pixels/readback, observer queue submissions=0\n");
}
} // namespace simxr::probe
int main(int argc, char** argv)
{
	using namespace simxr;
	using namespace simxr::probe;
	const bool coreTimeline = argc == 2 && std::strcmp(argv[1], "--core-timeline") == 0;
	const bool timelineOnly = argc == 2 && std::strcmp(argv[1], "--timeline-only") == 0;
	if (argc > 1 && !coreTimeline && !timelineOnly)
		return 2;
	const auto api = coreTimeline ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
	try
	{
		{
			simxr::probe::Instance instance;
			instance.Create(api);
			std::uint32_t count = 0;
			Check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "enumerate fence adapters");
			std::vector<VkPhysicalDevice> adapters(count);
			Check(vkEnumeratePhysicalDevices(instance.handle, &count, adapters.data()), "enumerate fence adapters");
			bool exercised = false, sharedResources = false;
			for (auto physical : adapters)
			{
				VkPhysicalDeviceProperties properties;
				vkGetPhysicalDeviceProperties(physical, &properties);
				if (properties.apiVersion < api)
					continue;
				Require(!VulkanExternalDevice::QueryFence(VK_NULL_HANDLE, VK_API_VERSION_1_1).CanExport() &&
							!VulkanExternalDevice::QueryFence(physical, VK_API_VERSION_1_0).CanImport(),
						"External fence query accepted absent device or unsupported API");
				const auto capabilities = VulkanExternalDevice::QueryFence(physical, VK_API_VERSION_1_1);
				std::printf("adapter=%s fenceQuery=%d compatible=0x%x features=0x%x import=%d export=%d\n",
							properties.deviceName, capabilities.result, capabilities.fence.compatibleHandleTypes,
							capabilities.fence.externalFenceFeatures, capabilities.CanImport(),
							capabilities.CanExport());
				VulkanExternalImageSpec image;
				image.extent = {1, 1, 1};
				const auto sharing = VulkanExternalDevice::Query(physical, image, VK_API_VERSION_1_1);
				if (!sharing.CanExportImage() || !sharing.CanImportImage() || !sharing.CanExportSemaphore() ||
					!sharing.CanImportSemaphore())
					continue;
				vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
				std::vector<VkQueueFamilyProperties> families(count);
				vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
				for (std::uint32_t family = 0; family != count; ++family)
					if (families[family].queueCount && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
					{
						if (!sharedResources)
						{
							FenceDevice producer(instance.handle, physical, family, s_sharingExtensions);
							FenceDevice consumer(instance.handle, physical, family, s_sharingExtensions);
							VulkanExternalDevice exporter(producer.m_device, s_sharingExtensions, VK_API_VERSION_1_1);
							VulkanExternalDevice importer(consumer.m_device, s_sharingExtensions, VK_API_VERSION_1_1);
							ExerciseMemoryAndSemaphore(exporter, importer);
							sharedResources = true;
						}
						exercised |= ExerciseTimeline(instance.handle, physical, family, api);
						if (!timelineOnly && capabilities.CanImport() && capabilities.CanExport())
						{
							Exercise(instance.handle, physical, family);
							exercised = true;
						}
						break;
					}
				if (exercised)
					break;
			}
			if (!exercised)
			{
				std::printf("SKIP: no adapter supports the production image/semaphore path plus opaque-native "
							"fence or timeline completion\n");
				return 77;
			}
		}
		Require(simxr::probe::s_validationErrors.load() == 0, "Validation errors after all fence/device teardown");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
