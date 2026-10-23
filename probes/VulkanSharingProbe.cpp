#include "Pch.h"

#include "LinuxFdChannel.h"
#include "VulkanProbeSupport.h"
#include "VulkanProcessPeer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace simxr::probe
{
static void ProveSharing(VkPhysicalDevice physicalDevice, std::uint32_t family, std::uint32_t consumerQueue,
						 bool verifyPixels)
{
	Device producer;
	Device consumer;
	producer.Create(physicalDevice, family, 0);
	consumer.Create(physicalDevice, family, consumerQueue);
	auto source = producer.CreateImage(true);
	auto imported = consumer.CreateImage(true);
	VkMemoryRequirements sourceRequirements{}, importRequirements{};
	vkGetImageMemoryRequirements(producer.handle, source, &sourceRequirements);
	vkGetImageMemoryRequirements(consumer.handle, imported, &importRequirements);
	if (sourceRequirements.size != importRequirements.size)
		throw std::runtime_error("Producer/consumer allocation sizes differ");
	auto memoryType = MemoryType(physicalDevice, sourceRequirements.memoryTypeBits & importRequirements.memoryTypeBits);
	auto memory = producer.Allocate(source, memoryType, sourceRequirements.size, true);
	VkMemoryGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	exportInfo.memory = memory;
	exportInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	int fd = -1;
	Check(producer.Get<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(producer.handle, &exportInfo, &fd),
		  "export image memory FD");
	consumer.Allocate(imported, memoryType, sourceRequirements.size, true, fd);
	auto ready = producer.CreateSemaphore(true);
	auto importedReady = consumer.CreateSemaphore(false);
	ImportSemaphore(producer, ready, consumer, importedReady);
	auto finished = consumer.CreateSemaphore(true);
	auto importedFinished = producer.CreateSemaphore(false);
	ImportSemaphore(consumer, finished, producer, importedFinished);

	std::vector<VkImage> retainedImages;
	for (std::uint32_t frame = 0; frame < 3; ++frame)
	{
		auto retained = consumer.CreateImage(false);
		retainedImages.push_back(retained);
		VkMemoryRequirements retainedRequirements{};
		vkGetImageMemoryRequirements(consumer.handle, retained, &retainedRequirements);
		consumer.Allocate(retained, MemoryType(physicalDevice, retainedRequirements.memoryTypeBits),
						  retainedRequirements.size, false);
		auto produce = producer.BeginCommands();
		Barrier(produce, source, frame ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT, frame ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
				frame ? family : VK_QUEUE_FAMILY_IGNORED);
		VkClearColorValue color{{0, 0, 0, 1}};
		color.float32[frame] = 1;
		vkCmdClearColorImage(produce, source, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &ColorRange);
		Barrier(produce, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
				family, VK_QUEUE_FAMILY_EXTERNAL);
		producer.Submit(produce, frame ? importedFinished : VK_NULL_HANDLE, ready);

		auto consume = consumer.BeginCommands();
		Barrier(consume, imported, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
				VK_QUEUE_FAMILY_EXTERNAL, family);
		Barrier(consume, retained, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = ImageExtent;
		vkCmdCopyImage(consume, imported, VK_IMAGE_LAYOUT_GENERAL, retained, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		Barrier(consume, imported, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
				family, VK_QUEUE_FAMILY_EXTERNAL);
		Barrier(consume, retained, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED);
		consumer.Submit(consume, importedReady, finished);
		consumer.WaitForCompletion();
		std::printf("frame=%u ready=complete consumer=complete retained=GPU-only\n", frame + 1);
	}
	producer.WaitForCompletion();
	if (verifyPixels)
		for (std::uint32_t frame = 0; frame < retainedImages.size(); ++frame)
			consumer.VerifyImage(physicalDevice, retainedImages[frame], frame);
}
static void ProveCrossProcess(VkPhysicalDevice physicalDevice, std::uint32_t family, std::uint32_t consumerQueue,
							  bool verifyPixels, bool disconnect, const std::string& companion,
							  const std::string& screenshot)
{
	Device producer;
	producer.Create(physicalDevice, family, 0);
	auto source = producer.CreateImage(true);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(producer.handle, source, &requirements);
	SharingPacket packet;
	const auto identity = Identity(physicalDevice);
	std::copy_n(identity.deviceUUID, VK_UUID_SIZE, packet.deviceUuid.begin());
	std::copy_n(identity.driverUUID, VK_UUID_SIZE, packet.driverUuid.begin());
	packet.allocationSize = requirements.size;
	packet.memoryType = MemoryType(physicalDevice, requirements.memoryTypeBits);
	packet.family = family;
	packet.consumerQueue = consumerQueue;
	auto memory = producer.Allocate(source, packet.memoryType, requirements.size, true);
	auto ready = producer.CreateSemaphore(true);
	auto finished = producer.CreateSemaphore(true);
	sharing::Descriptors exported;
	VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	info.memory = memory;
	info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	Check(producer.Get<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(producer.handle, &info, &exported.values[0]),
		  "export process image memory");
	exported.values[1] = ExportSemaphore(producer, ready);
	exported.values[2] = ExportSemaphore(producer, finished);
	ConsumerProcess consumer(verifyPixels, disconnect, companion, screenshot);
	sharing::Send(consumer.Socket(), &packet, sizeof(packet), exported.values);
	for (std::uint32_t frame = 0; frame < 3; ++frame)
	{
		auto commands = producer.BeginCommands();
		Barrier(commands, source, frame ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT, frame ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
				frame ? family : VK_QUEUE_FAMILY_IGNORED);
		VkClearColorValue color{{0, 0, 0, 1}};
		color.float32[frame] = 1;
		vkCmdClearColorImage(commands, source, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &ColorRange);
		Barrier(commands, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
				family, VK_QUEUE_FAMILY_EXTERNAL);
		producer.Submit(commands, frame ? finished : VK_NULL_HANDLE, ready);
		sharing::Send(consumer.Socket(), &frame, sizeof(frame));
		std::uint32_t completed = UINT32_MAX;
		sharing::Descriptors acknowledgement;
		consumer.Receive(&completed, sizeof(completed), acknowledgement);
		if (completed != frame)
			throw std::runtime_error("Consumer completion identity mismatch");
		producer.WaitForCompletion();
		std::printf("processFrame=%u consumerPid=separate retained=GPU-only completed=true\n", frame + 1);
		if (disconnect)
		{
			consumer.ObserveClosed();
			consumer.Join();
			auto recover = producer.BeginCommands();
			Barrier(recover, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_QUEUE_FAMILY_EXTERNAL, family);
			vkCmdClearColorImage(recover, source, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &ColorRange);
			producer.Submit(recover, finished, VK_NULL_HANDLE);
			producer.WaitForCompletion();
			std::puts("disconnect=observed producerQueue=usable sourceReused=true noFutureConsumerWait=true");
			return;
		}
	}
	std::uint32_t complete = 0;
	sharing::Descriptors final;
	consumer.Receive(&complete, sizeof(complete), final);
	if (complete != 3)
		throw std::runtime_error("Consumer retention proof failed");
	consumer.Join();
	// A repeated join must never wait for or reap another child process.
	consumer.Join();
}

} // namespace simxr::probe

static bool HasPath(int index, int argc, char** argv)
{
	if (index + 1 >= argc || !*argv[index + 1])
		return false;
	constexpr std::array options{
		"--verify-pixels",		 "--companion", "--companion-screenshot",  "--cross-process", "--verify-disconnect",
		"--consumer-disconnect", "--consumer",	"--consumer-verify-pixels"};
	return std::none_of(options.begin(), options.end(),
						[&](const char* option)
						{
							return std::strcmp(argv[index + 1], option) == 0;
						});
}

int main(int argc, char** argv)
{
	using namespace simxr::probe;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	bool verifyPixels = false;
	std::string companion;
	std::string companionScreenshot;
	bool crossProcess = false;
	bool consumer = false;
	bool disconnect = false;
	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--verify-pixels") == 0)
			verifyPixels = true;
		else if (std::strcmp(argv[index], "--companion") == 0 && HasPath(index, argc, argv))
		{
			companion = argv[++index];
			crossProcess = true;
		}
		else if (std::strcmp(argv[index], "--companion-screenshot") == 0 && HasPath(index, argc, argv))
			companionScreenshot = argv[++index];
		else if (std::strcmp(argv[index], "--cross-process") == 0)
			crossProcess = true;
		else if (std::strcmp(argv[index], "--verify-disconnect") == 0)
		{
			crossProcess = true;
			disconnect = true;
		}
		else if (std::strcmp(argv[index], "--consumer-disconnect") == 0)
		{
			consumer = true;
			disconnect = true;
		}
		else if (std::strcmp(argv[index], "--consumer") == 0)
			consumer = true;
		else if (std::strcmp(argv[index], "--consumer-verify-pixels") == 0)
		{
			consumer = true;
			verifyPixels = true;
		}
		else
		{
			std::fprintf(stderr, "Usage: vulkan_sharing_probe [--cross-process] [--verify-pixels] "
								 "[--verify-disconnect] [--companion PATH] [--companion-screenshot PATH]\n");
			return 2;
		}
	}
	if ((consumer && (crossProcess || !companionScreenshot.empty())) || (!companion.empty() && disconnect) ||
		(!companionScreenshot.empty() && companion.empty()))
	{
		std::fputs("Companion display, screenshot and disconnect options are inconsistent\n", stderr);
		return 2;
	}
	if (verifyPixels && disconnect)
	{
		std::fputs("Pixel verification and early disconnect are separate proofs\n", stderr);
		return 2;
	}
	std::printf("pixelAnalysis=%s\n", verifyPixels ? "explicit readback after GPU-only handoff" : "disabled");
	try
	{
		if (consumer)
		{
			RunConsumer(verifyPixels, disconnect);
			if (s_validationErrors.load())
				throw std::runtime_error("Consumer Vulkan validation failed during teardown");
			return 0;
		}
		bool qualified = false;
		{
			Instance instance;
			instance.Create();
			std::uint32_t count = 0;
			Check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "enumerate physical devices");
			std::vector<VkPhysicalDevice> devices(count);
			Check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "enumerate physical devices");
			for (auto device : devices)
			{
				std::uint32_t family = 0;
				std::uint32_t queue = 0;
				if (!SupportsSharing(device, family, queue))
					continue;
				if (crossProcess)
					ProveCrossProcess(device, family, queue, verifyPixels, disconnect, companion, companionScreenshot);
				else
					ProveSharing(device, family, queue, verifyPixels);
				qualified = true;
			}
		}
		if (s_validationErrors.load())
			throw std::runtime_error("Vulkan validation reported errors");
		if (!qualified)
		{
			std::puts("SKIP: no adapter supports the required Vulkan OPAQUE_FD memory and semaphore path");
			return 77;
		}
		if (disconnect)
		{
			std::puts("PASS: consumer disconnected; owned producer queue and source reuse remain usable");
			return 0;
		}
		std::puts(verifyPixels ? "PASS: GPU-only handoff and exact retained colors after source reuse; explicitly "
								 "requested CPU analysis"
							   : "PASS: two owned devices, external image memory, GPU readiness, consumer completion "
								 "and retained copies; no pixel readback");
		return 0;
	}
	catch (const ConsumerUnavailable& error)
	{
		std::fprintf(stderr, "SKIP: %s\n", error.what());
		return 77;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
