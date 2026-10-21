#pragma once

#include "LinuxFdChannel.h"
#include "VulkanProbeSupport.h"

#include <chrono>
#include <exception>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <string>
#include <utility>

extern char** environ;

namespace simxr::probe
{
struct SharingPacket
{
	std::uint32_t version = 2;
	VkImageUsageFlags imageUsage = ImageUsage;
	std::array<std::uint8_t, VK_UUID_SIZE> deviceUuid{};
	std::array<std::uint8_t, VK_UUID_SIZE> driverUuid{};
	std::uint64_t allocationSize = 0;
	std::uint32_t memoryType = 0;
	std::uint32_t family = 0;
	std::uint32_t consumerQueue = 0;
};

class ConsumerUnavailable : public std::runtime_error
{
public:
	ConsumerUnavailable() : std::runtime_error("Consumer display is unavailable; qualification skipped") {}
};

class ConsumerProcess
{
public:
	explicit ConsumerProcess(bool verifyPixels, bool disconnect = false,
		const std::string& companion = {}, const std::string& screenshot = {}) :
		ConsumerProcess(companion.empty() ? "/proc/self/exe" : companion, Arguments(verifyPixels, disconnect, companion, screenshot))
	{
	}

	ConsumerProcess(const std::string& executable, std::vector<std::string> options)
	{
		int sockets[2];
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
			throw std::runtime_error("Cannot create sharing channel");
		FileDescriptor parent(sockets[0]);
		FileDescriptor child(sockets[1]);
		posix_spawn_file_actions_t actions;
		if (posix_spawn_file_actions_init(&actions))
			throw std::runtime_error("Cannot initialize consumer launch");
		struct Actions
		{
			posix_spawn_file_actions_t* value;
			~Actions() { posix_spawn_file_actions_destroy(value); }
		} actionGuard{&actions};
		if (posix_spawn_file_actions_addclose(&actions, sockets[0]) ||
			posix_spawn_file_actions_adddup2(&actions, sockets[1], 3) ||
			(sockets[1] != 3 && posix_spawn_file_actions_addclose(&actions, sockets[1])))
			throw std::runtime_error("Cannot configure consumer descriptors");
		std::vector<std::string> values{executable};
		for (auto& option : options) values.push_back(std::move(option));
		std::vector<char*> arguments;
		for (auto& value : values) arguments.push_back(value.data());
		arguments.push_back(nullptr);
		if (posix_spawn(&m_pid, executable.c_str(), &actions, nullptr, arguments.data(), environ))
			throw std::runtime_error("Cannot launch Vulkan consumer process");
		m_socket = sockets[0];
		parent.Release();
	}
	ConsumerProcess(const ConsumerProcess&) = delete;
	ConsumerProcess& operator=(const ConsumerProcess&) = delete;
	~ConsumerProcess()
	{
		if (m_socket >= 0) close(m_socket);
		if (m_pid > 0)
		{
			kill(m_pid, SIGKILL);
			int status;
			while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
		}
	}
	int Socket() const { return m_socket; }
	void Receive(void* data, std::size_t size, sharing::Descriptors& descriptors)
	{
		try { sharing::Receive(m_socket, data, size, descriptors); }
		catch (...)
		{
			const auto failure = std::current_exception();
			try { Join(); }
			catch (const ConsumerUnavailable&) { throw; }
			catch (...) {}
			std::rethrow_exception(failure);
		}
	}

	void ObserveClosed()
	{
		pollfd connection{m_socket, POLLIN, 0};
		int status;
		do { status = poll(&connection, 1, 5000); } while (status < 0 && errno == EINTR);
		char unexpected;
		if (status != 1 || recv(m_socket, &unexpected, 1, MSG_DONTWAIT) != 0)
			throw std::runtime_error("Consumer did not close cleanly at the completed boundary");
	}

	void Join()
	{
		int status = 0;
		pid_t result = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
		while (result == 0 && std::chrono::steady_clock::now() < deadline)
		{
			result = waitpid(m_pid, &status, WNOHANG);
			if (result < 0 && errno == EINTR) result = 0;
			if (result == 0) poll(nullptr, 0, 10);
		}
		const bool reaped = result == m_pid;
		if (reaped) m_pid = -1;
		if (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 77) throw ConsumerUnavailable{};
		if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			throw std::runtime_error("Vulkan consumer process failed");
	}
private:
	static std::vector<std::string> Arguments(bool verifyPixels, bool disconnect, const std::string& companion, const std::string& screenshot)
	{
		if (companion.empty())
			return {disconnect ? "--consumer-disconnect" : (verifyPixels ? "--consumer-verify-pixels" : "--consumer")};
		if (disconnect) throw std::invalid_argument("Companion display and early disconnect are separate proofs");
		std::vector<std::string> result{"--proof-consumer", "--validation"};
		if (verifyPixels) result.push_back("--proof-verify-pixels");
		if (!screenshot.empty()) { result.push_back("--screenshot"); result.push_back(screenshot); }
		return result;
	}
	int m_socket = -1;
	pid_t m_pid = -1;
};

inline void RunConsumer(bool verifyPixels, bool disconnect)
{
	SharingPacket packet;
	sharing::Descriptors descriptors;
	sharing::Receive(3, &packet, sizeof(packet), descriptors, 3);
	if (packet.version != 2 || !packet.allocationSize || packet.memoryType >= 32 ||
		(packet.imageUsage != ImageUsage && packet.imageUsage != (ImageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)))
		throw std::runtime_error("Invalid consumer sharing metadata");
	Instance instance;
	instance.Create();
	std::uint32_t count = 0;
	Check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "enumerate consumer adapters");
	std::vector<VkPhysicalDevice> devices(count);
	Check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "enumerate consumer adapters");
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	for (auto device : devices)
	{
		const auto identity = Identity(device);
		if (std::equal(packet.deviceUuid.begin(), packet.deviceUuid.end(), identity.deviceUUID) &&
			std::equal(packet.driverUuid.begin(), packet.driverUuid.end(), identity.driverUUID))
			physicalDevice = device;
	}
	if (!physicalDevice) throw std::runtime_error("Consumer adapter/driver UUID mismatch");
	std::uint32_t family = 0, queue = 0;
	if (!SupportsSharing(physicalDevice, family, queue, packet.imageUsage) || family != packet.family || queue != packet.consumerQueue)
		throw std::runtime_error("Consumer sharing capability mismatch");
	Device consumer;
	consumer.Create(physicalDevice, family, queue);
	auto imported = consumer.CreateImage(true, packet.imageUsage);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(consumer.handle, imported, &requirements);
	if (requirements.size != packet.allocationSize || !(requirements.memoryTypeBits & (1u << packet.memoryType)))
		throw std::runtime_error("Consumer image allocation requirements mismatch");
	consumer.Allocate(imported, packet.memoryType, packet.allocationSize, true, descriptors.Take(0));
	auto ready = ImportSemaphore(consumer, descriptors.Take(1));
	auto finished = ImportSemaphore(consumer, descriptors.Take(2));
	std::vector<VkImage> retainedImages;
	for (std::uint32_t frame = 0; frame < 3; ++frame)
	{
		std::uint32_t requested = UINT32_MAX;
		sharing::Descriptors control;
		sharing::Receive(3, &requested, sizeof(requested), control);
		if (requested != frame) throw std::runtime_error("Consumer frame identity mismatch");
		auto retained = consumer.CreateImage(false);
		retainedImages.push_back(retained);
		vkGetImageMemoryRequirements(consumer.handle, retained, &requirements);
		consumer.Allocate(retained, MemoryType(physicalDevice, requirements.memoryTypeBits), requirements.size, false);
		auto commands = consumer.BeginCommands();
		Barrier(commands, imported, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0,
			VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, family);
		Barrier(commands, retained, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = ImageExtent;
		vkCmdCopyImage(commands, imported, VK_IMAGE_LAYOUT_GENERAL, retained, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		Barrier(commands, imported, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
			0, family, VK_QUEUE_FAMILY_EXTERNAL);
		Barrier(commands, retained, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		consumer.Submit(commands, ready, finished);
		consumer.WaitForCompletion();
		sharing::Send(3, &frame, sizeof(frame));
		if (disconnect) return;
	}
	if (verifyPixels)
		for (std::uint32_t frame = 0; frame < retainedImages.size(); ++frame)
			consumer.VerifyImage(physicalDevice, retainedImages[frame], frame);
	if (s_validationErrors.load()) throw std::runtime_error("Consumer Vulkan validation failed");
	const std::uint32_t complete = 3;
	sharing::Send(3, &complete, sizeof(complete));
}

}
