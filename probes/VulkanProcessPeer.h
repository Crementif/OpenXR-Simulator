#pragma once

#include "LinuxFdChannel.h"
#include "VulkanProbeSupport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>

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
	ConsumerUnavailable();
};

class ConsumerProcess
{
  public:
	explicit ConsumerProcess(bool verifyPixels, bool disconnect = false, const std::string& companion = {},
							 const std::string& screenshot = {});
	ConsumerProcess(const std::string& executable, std::vector<std::string> options);
	ConsumerProcess(const ConsumerProcess&) = delete;
	ConsumerProcess& operator=(const ConsumerProcess&) = delete;
	~ConsumerProcess();
	int Socket() const;
	void Receive(void* data, std::size_t size, sharing::Descriptors& descriptors);
	void ObserveClosed();
	void Join();

  private:
	static std::vector<std::string> Arguments(bool verifyPixels, bool disconnect, const std::string& companion,
											  const std::string& screenshot);
	int m_socket = -1;
	pid_t m_pid = -1;
};

void RunConsumer(bool verifyPixels, bool disconnect);
} // namespace simxr::probe
