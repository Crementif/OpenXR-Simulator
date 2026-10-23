#include "VulkanProcessPeer.h"
#include "Pch.h"

#include <cerrno>
#include <exception>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace simxr::probe
{
ConsumerUnavailable::ConsumerUnavailable()
	: std::runtime_error("Consumer display is unavailable; qualification skipped")
{
}

ConsumerProcess::ConsumerProcess(bool verifyPixels, bool disconnect, const std::string& companion,
								 const std::string& screenshot)
	: ConsumerProcess(companion.empty() ? "/proc/self/exe" : companion,
					  Arguments(verifyPixels, disconnect, companion, screenshot))
{
}

ConsumerProcess::ConsumerProcess(const std::string& executable, std::vector<std::string> options)
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
		throw std::system_error(errno, std::generic_category(), "Create sharing channel");
	FileDescriptor parent(sockets[0]);
	FileDescriptor child(sockets[1]);
	posix_spawn_file_actions_t actions;
	if (const auto error = posix_spawn_file_actions_init(&actions))
		throw std::system_error(error, std::generic_category(), "Initialize consumer launch");
	struct Actions
	{
		posix_spawn_file_actions_t* value;
		~Actions()
		{
			posix_spawn_file_actions_destroy(value);
		}
	} actionGuard{&actions};
	if (const auto error = posix_spawn_file_actions_addclose(&actions, sockets[0]))
		throw std::system_error(error, std::generic_category(), "Close consumer parent channel");
	if (const auto error = posix_spawn_file_actions_adddup2(&actions, sockets[1], 3))
		throw std::system_error(error, std::generic_category(), "Duplicate consumer channel");
	if (sockets[1] != 3)
		if (const auto error = posix_spawn_file_actions_addclose(&actions, sockets[1]))
			throw std::system_error(error, std::generic_category(), "Close consumer original channel");
	std::vector<std::string> values{executable};
	for (auto& option : options)
		values.push_back(std::move(option));
	std::vector<char*> arguments;
	for (auto& value : values)
		arguments.push_back(value.data());
	arguments.push_back(nullptr);
	if (const auto error = posix_spawn(&m_pid, executable.c_str(), &actions, nullptr, arguments.data(), environ))
		throw std::system_error(error, std::generic_category(), "Launch Vulkan consumer process");
	m_socket = sockets[0];
	parent.Release();
}

ConsumerProcess::~ConsumerProcess()
{
	if (m_socket >= 0)
		close(m_socket);
	if (m_pid > 0)
	{
		kill(m_pid, SIGKILL);
		int status;
		while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR)
		{
		}
	}
}

int ConsumerProcess::Socket() const
{
	return m_socket;
}

void ConsumerProcess::Receive(void* data, std::size_t size, sharing::Descriptors& descriptors)
{
	try
	{
		sharing::Receive(m_socket, data, size, descriptors);
	}
	catch (...)
	{
		const auto failure = std::current_exception();
		try
		{
			Join();
		}
		catch (const ConsumerUnavailable&)
		{
			throw;
		}
		catch (...)
		{
		}
		std::rethrow_exception(failure);
	}
}

void ConsumerProcess::ObserveClosed()
{
	pollfd connection{m_socket, POLLIN, 0};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
	int status = 0;
	do
	{
		const auto remaining =
			std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		if (remaining.count() <= 0)
		{
			status = 0;
			break;
		}
		status = poll(&connection, 1, static_cast<int>(remaining.count()));
	} while (status < 0 && errno == EINTR);
	if (status < 0)
		throw std::system_error(errno, std::generic_category(), "Observe consumer close");
	char unexpected;
	if (status != 1 || recv(m_socket, &unexpected, 1, MSG_DONTWAIT) != 0)
		throw std::runtime_error("Consumer did not close cleanly at the completed boundary");
}

void ConsumerProcess::Join()
{
	if (m_pid <= 0)
		return;
	int status = 0;
	pid_t result = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
	while (result == 0 && std::chrono::steady_clock::now() < deadline)
	{
		result = waitpid(m_pid, &status, WNOHANG);
		if (result < 0 && errno == EINTR)
			result = 0;
		if (result == 0)
			poll(nullptr, 0, 10);
	}
	if (result < 0)
	{
		const auto error = errno;
		if (error == ECHILD)
			m_pid = -1;
		throw std::system_error(error, std::generic_category(), "Join Vulkan consumer process");
	}
	const bool reaped = result == m_pid;
	if (reaped)
		m_pid = -1;
	if (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 77)
		throw ConsumerUnavailable{};
	if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		throw std::runtime_error("Vulkan consumer process failed");
}

std::vector<std::string> ConsumerProcess::Arguments(bool verifyPixels, bool disconnect, const std::string& companion,
													const std::string& screenshot)
{
	if (companion.empty())
	{
		if (disconnect)
			return {"--consumer-disconnect"};
		return {verifyPixels ? "--consumer-verify-pixels" : "--consumer"};
	}
	if (disconnect)
		throw std::invalid_argument("Companion display and early disconnect are separate proofs");
	std::vector<std::string> result{"--proof-consumer", "--validation"};
	if (verifyPixels)
		result.push_back("--proof-verify-pixels");
	if (!screenshot.empty())
	{
		result.push_back("--screenshot");
		result.push_back(screenshot);
	}
	return result;
}

void RunConsumer(bool verifyPixels, bool disconnect)
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
	if (!physicalDevice)
		throw std::runtime_error("Consumer adapter/driver UUID mismatch");
	std::uint32_t family = 0, queue = 0;
	if (!SupportsSharing(physicalDevice, family, queue, packet.imageUsage) || family != packet.family ||
		queue != packet.consumerQueue)
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
		if (requested != frame)
			throw std::runtime_error("Consumer frame identity mismatch");
		auto retained = consumer.CreateImage(false);
		retainedImages.push_back(retained);
		vkGetImageMemoryRequirements(consumer.handle, retained, &requirements);
		consumer.Allocate(retained, MemoryType(physicalDevice, requirements.memoryTypeBits), requirements.size, false);
		auto commands = consumer.BeginCommands();
		Barrier(commands, imported, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
				VK_QUEUE_FAMILY_EXTERNAL, family);
		Barrier(commands, retained, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = ImageExtent;
		vkCmdCopyImage(commands, imported, VK_IMAGE_LAYOUT_GENERAL, retained, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		Barrier(commands, imported, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
				family, VK_QUEUE_FAMILY_EXTERNAL);
		Barrier(commands, retained, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED);
		consumer.Submit(commands, ready, finished);
		consumer.WaitForCompletion();
		sharing::Send(3, &frame, sizeof(frame));
		if (disconnect)
			return;
	}
	if (verifyPixels)
		for (std::uint32_t frame = 0; frame < retainedImages.size(); ++frame)
			consumer.VerifyImage(physicalDevice, retainedImages[frame], frame);
	if (s_validationErrors.load())
		throw std::runtime_error("Consumer Vulkan validation failed");
	const std::uint32_t complete = 3;
	sharing::Send(3, &complete, sizeof(complete));
}

} // namespace simxr::probe
