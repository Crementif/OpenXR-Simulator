#include "VulkanProbeInstance.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
using namespace simxr;
using simxr::probe::Check;
constexpr std::array SharingExtensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension};
constexpr std::array FenceExtensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension,
									 VulkanExternalFenceExtension};
constexpr std::uint64_t Timeout = 5000000000ULL;
bool ValidNonInherited(VulkanNativeHandle handle)
{
#ifdef _WIN32
	DWORD flags = 0;
	return GetHandleInformation(handle, &flags) && !(flags & HANDLE_FLAG_INHERIT);
#else
	const auto flags = fcntl(handle, F_GETFD);
	return flags >= 0 && (flags & FD_CLOEXEC);
#endif
}
void Require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}
template <class Function> void Rejected(Function operation, VkResult expected, const char* message)
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
struct Device
{
	VkDevice native = VK_NULL_HANDLE;
	bool timeline = false;
	std::shared_ptr<VulkanDevice> device;
	Device(VkInstance instance, VkPhysicalDevice physical, std::uint32_t family,
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
				timeline = timelineFeatures.timelineSemaphore;
				if (timeline && !coreTimeline)
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
		create.pNext = timeline ? &timelineFeatures : nullptr;
		Check(vkCreateDevice(physical, &create, nullptr, &native), "create fence proof device");
		try
		{
			device = std::make_shared<VulkanDevice>(VulkanBinding{instance, physical, native, family, 0});
		}
		catch (...)
		{
			vkDestroyDevice(native, nullptr);
			throw;
		}
	}
	~Device()
	{
		device.reset();
		vkDestroyDevice(native, nullptr);
	}
};
// An optional host-signaled timeline gate holds consumer-only GPU work pending.
// Neither the observer nor an application/inspection queue waits on the consumer.
struct ConsumerWork
{
	std::shared_ptr<VulkanDevice> device;
	std::shared_ptr<VulkanExternalFence> completion;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkSemaphore gate = VK_NULL_HANDLE;
	VkFence retirementFence = VK_NULL_HANDLE;
	PFN_vkSignalSemaphoreKHR signalSemaphore = nullptr;
	bool gateReleased = false;
	bool retirementSubmitted = false;
	bool submitted = false;
	ConsumerWork(std::shared_ptr<VulkanDevice> owner, std::shared_ptr<VulkanExternalFence> fence, bool timeline)
		: device(std::move(owner)), completion(std::move(fence))
	{
		try
		{
			VkCommandPoolCreateInfo create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			create.queueFamilyIndex = device->binding.queueFamily;
			Check(vkCreateCommandPool(device->binding.device, &create, nullptr, &pool), "create consumer command pool");
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocate.commandPool = pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocate.commandBufferCount = 1;
			Check(vkAllocateCommandBuffers(device->binding.device, &allocate, &command), "allocate consumer command");

			if (timeline)
			{
				signalSemaphore = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
					vkGetDeviceProcAddr(device->binding.device, "vkSignalSemaphoreKHR"));
				Require(signalSemaphore != nullptr, "Consumer timeline command unavailable");
				VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
				type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
				VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
				Check(vkCreateSemaphore(device->binding.device, &semaphore, nullptr, &gate),
					  "create consumer timeline gate");
			}
			VkFenceCreateInfo fenceCreate{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			Check(vkCreateFence(device->binding.device, &fenceCreate, nullptr, &retirementFence),
				  "create internal consumer retirement fence");
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			Check(vkBeginCommandBuffer(command, &begin), "begin consumer command");
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
								 nullptr, 0, nullptr, 0, nullptr);
			Check(vkEndCommandBuffer(command), "end consumer command");
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}

	void Cleanup() noexcept
	{
		if (submitted)
		{
			if (gate && !gateReleased)
			{
				VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
				signal.semaphore = gate;
				signal.value = 1;
				if (signalSemaphore(device->binding.device, &signal) != VK_SUCCESS)
					std::_Exit(1);
			}
			const auto fence = retirementSubmitted ? retirementFence : completion->Get();
			if (vkWaitForFences(device->binding.device, 1, &fence, VK_TRUE, Timeout) != VK_SUCCESS)
			{
				std::fprintf(stderr, "FAIL: consumer completion unknown; retaining native owners until process exit\n");
				std::_Exit(1);
			}
		}
		if (retirementFence)
			vkDestroyFence(device->binding.device, retirementFence, nullptr);
		if (gate)
			vkDestroySemaphore(device->binding.device, gate, nullptr);
		if (pool)
			vkDestroyCommandPool(device->binding.device, pool, nullptr);
		gate = VK_NULL_HANDLE;
		pool = VK_NULL_HANDLE;
	}
	~ConsumerWork()
	{
		Cleanup();
	}

	void Submit()
	{
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		const std::uint64_t value = 1;
		VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		if (gate)
		{
			timeline.waitSemaphoreValueCount = 1;
			timeline.pWaitSemaphoreValues = &value;
			submit.pNext = &timeline;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &gate;
			submit.pWaitDstStageMask = &stage;
		}
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		Check(vkQueueSubmit(device->queue, 1, &submit, completion->Get()), "submit consumer-only work");
		submitted = true;
		// This private marker proves native retirement to validation as well as
		// the driver; external-payload observers alone do not track command use.
		VkSubmitInfo marker{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		Check(vkQueueSubmit(device->queue, 1, &marker, retirementFence), "submit internal retirement marker");
		retirementSubmitted = true;
	}
	void Release()
	{
		if (!gate)
			return;
		VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		signal.semaphore = gate;
		signal.value = 1;
		Check(signalSemaphore(device->binding.device, &signal), "release consumer timeline gate");
		gateReleased = true;
	}
};
void ExerciseMemoryAndSemaphore(VulkanExternalDevice& exporter, VulkanExternalDevice& importer)
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
	auto semaphore = exporter.CreateExportableSemaphore();
	auto handle = semaphore->ExportOnce();
	Require(handle && ValidNonInherited(handle.Get()), "Export non-inherited semaphore");
	auto importedSemaphore = importer.ImportSemaphore(exporter.Identity(), handle);
	Require(!handle, "Successful semaphore import consumes caller ownership");
	const auto signal = semaphore->Get(), wait = importedSemaphore->Get();
	VkSubmitInfo producer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	producer.signalSemaphoreCount = 1;
	producer.pSignalSemaphores = &signal;
	Check(vkQueueSubmit(exporter.Device()->queue, 1, &producer, VK_NULL_HANDLE), "Signal external semaphore");
	// Drain accepted work before any exceptional teardown of the signal owner.
	if (vkQueueWaitIdle(exporter.Device()->queue) != VK_SUCCESS)
		std::_Exit(1);
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkSubmitInfo consumer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	consumer.waitSemaphoreCount = 1;
	consumer.pWaitSemaphores = &wait;
	consumer.pWaitDstStageMask = &stage;
	Check(vkQueueSubmit(importer.Device()->queue, 1, &consumer, VK_NULL_HANDLE), "Wait imported semaphore");
	if (vkQueueWaitIdle(importer.Device()->queue) != VK_SUCCESS)
		std::_Exit(1);
	std::puts("PASS dedicated image import and two-device binary semaphore signal/wait");
}
bool ExerciseTimeline(VkInstance instance, VkPhysicalDevice physical, std::uint32_t family, std::uint32_t api)
{
	const auto caps = VulkanExternalDevice::QueryTimeline(physical, api);
	if (!caps.CanImportSemaphore() || !caps.CanExportSemaphore())
		return false;
	const bool coreTimeline = api >= VK_API_VERSION_1_2;
	constexpr std::array names{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension,
							   VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
	const auto extensions = std::span(names).first(coreTimeline ? 2 : names.size());
	Device consumer(instance, physical, family, SharingExtensions, true, coreTimeline);
	Device observer(instance, physical, family, SharingExtensions, true, coreTimeline);
	Require(consumer.timeline && observer.timeline, "Timeline capability must enable device feature");
	VulkanExternalDevice exporter(consumer.device, extensions, api, true);
	VulkanExternalDevice importer(observer.device, extensions, api, true);
	VulkanExternalDevice disabled(observer.device, extensions, api);
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
	VkSemaphore gate = VK_NULL_HANDLE;
	Check(vkCreateSemaphore(consumer.native, &create, nullptr, &gate), "create timeline proof gate");
	const auto signal = completion->Get();
	const std::uint64_t value = 1;
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
	timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues = timeline.pSignalSemaphoreValues = &value;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
	submit.waitSemaphoreCount = submit.signalSemaphoreCount = 1;
	submit.pWaitSemaphores = &gate;
	submit.pWaitDstStageMask = &stage;
	submit.pSignalSemaphores = &signal;
	Check(vkQueueSubmit(consumer.device->queue, 1, &submit, VK_NULL_HANDLE), "submit timeline completion proof");
	bool stayedPending = true;
	for (int poll = 0; poll < 100; ++poll)
		stayedPending &= imported->PollCompletion() == CompletionStatus::Pending;
	auto release = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
		vkGetDeviceProcAddr(consumer.native, coreTimeline ? "vkSignalSemaphore" : "vkSignalSemaphoreKHR"));
	VkSemaphoreSignalInfo releaseGate{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
	releaseGate.semaphore = gate;
	releaseGate.value = 1;
	if (!release || release(consumer.native, &releaseGate) != VK_SUCCESS ||
		vkQueueWaitIdle(consumer.device->queue) != VK_SUCCESS)
		std::_Exit(1);
	vkDestroySemaphore(consumer.native, gate, nullptr);
	Require(stayedPending, "Timeline completed while consumer work was gated");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (imported->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	Require(imported->PollCompletion() == CompletionStatus::Complete, "Imported timeline missed consumer completion");
	std::puts("PASS external timeline pending->complete; observer queue submissions=0; one-shot export and retained "
			  "failed-import ownership");
	return true;
}
void Exercise(VkInstance instance, VkPhysicalDevice physical, std::uint32_t family)
{
	Device consumer(instance, physical, family, FenceExtensions, true);
	Device observer(instance, physical, family, FenceExtensions);
	Device disabled(instance, physical, family, SharingExtensions);
	VulkanExternalDevice exporter(consumer.device, FenceExtensions, VK_API_VERSION_1_1);
	VulkanExternalDevice importer(observer.device, FenceExtensions, VK_API_VERSION_1_1);
	VulkanExternalDevice unavailable(disabled.device, SharingExtensions, VK_API_VERSION_1_1);
	auto native = exporter.CreateExportableFence();
	Require(native->Device() == consumer.device && native->Identity() == exporter.Identity(),
			"Fence owner identity mismatch");
	Require(native->PollCompletion() == CompletionStatus::Pending, "Unsubmitted fence falsely complete");
	auto fd = native->ExportOnce();
	Require(fd && ValidNonInherited(fd.Get()), "Exported fence handle must be close-on-exec");
	Rejected([&] { native->ExportOnce(); }, VK_ERROR_INVALID_EXTERNAL_HANDLE, "Second fence export accepted");
	auto mismatched = exporter.Identity();
	mismatched.driverUuid[0] ^= 1;
	const auto original = fd.Get();
	Rejected([&] { importer.ImportFence(mismatched, fd); }, VK_ERROR_INVALID_EXTERNAL_HANDLE,
			 "Wrong driver identity accepted");
	Require(fd.Get() == original && ValidNonInherited(original), "Rejected identity consumed caller handle");
	mismatched = exporter.Identity();
	mismatched.deviceUuid[0] ^= 1;
	Rejected([&] { importer.ImportFence(mismatched, fd); }, VK_ERROR_INVALID_EXTERNAL_HANDLE,
			 "Wrong device identity accepted");
	Rejected([&] { unavailable.CreateExportableFence(); }, VK_ERROR_EXTENSION_NOT_PRESENT,
			 "Disabled fence extension allowed export");
	Rejected([&] { unavailable.ImportFence(exporter.Identity(), fd); }, VK_ERROR_EXTENSION_NOT_PRESENT,
			 "Disabled fence extension allowed import");
	Require(fd.Get() == original && ValidNonInherited(original), "Disabled fence extension consumed caller handle");
	// Optional fence absence leaves the existing semaphore path usable.
	auto semaphore = unavailable.CreateExportableSemaphore();
	Require(semaphore->Get() != VK_NULL_HANDLE, "Optional fence requirement broke semaphore sharing");
	auto imported = importer.ImportFence(exporter.Identity(), fd);
	Require(!fd && imported->Device() == observer.device && imported->Identity() == native->Identity(),
			"Successful fence import did not transfer handle ownership");
	Rejected([&] { imported->ExportOnce(); }, VK_ERROR_INVALID_EXTERNAL_HANDLE,
			 "Imported fence unexpectedly re-exportable");
	Require(imported->PollCompletion() == CompletionStatus::Pending, "Imported unsignaled fence falsely complete");
	ConsumerWork work(consumer.device, native, consumer.timeline);
	std::printf("controlledConsumerGate=%s observerSubmissions=0\n",
				consumer.timeline ? "timeline" : "unavailable; pre-submit pending verified");
	work.Submit();
	for (int poll = 0; consumer.timeline && poll != 100; ++poll)
		Require(native->PollCompletion() == CompletionStatus::Pending &&
					imported->PollCompletion() == CompletionStatus::Pending,
				"Consumer fence completed before host released its consumer-only timeline gate");
	if (consumer.timeline)
	{
		std::atomic<bool> pending{true};
		std::array<std::thread, 4> readers;
		for (auto& reader : readers)
			reader = std::thread(
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
} // namespace
int main(int argc, char** argv)
{
	const bool coreTimeline = argc == 2 && std::strcmp(argv[1], "--core-timeline") == 0;
	if (argc > 1 && !coreTimeline)
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
							Device producer(instance.handle, physical, family, SharingExtensions);
							Device consumer(instance.handle, physical, family, SharingExtensions);
							VulkanExternalDevice exporter(producer.device, SharingExtensions, VK_API_VERSION_1_1);
							VulkanExternalDevice importer(consumer.device, SharingExtensions, VK_API_VERSION_1_1);
							ExerciseMemoryAndSemaphore(exporter, importer);
							sharedResources = true;
						}
						exercised |= ExerciseTimeline(instance.handle, physical, family, api);
						if (capabilities.CanImport() && capabilities.CanExport())
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
