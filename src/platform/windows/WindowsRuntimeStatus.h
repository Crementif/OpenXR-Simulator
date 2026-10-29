#pragma once

#include "platform/windows/WindowsRuntimeFiles.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace simxr::windows
{
struct FrameStatus
{
	std::uint64_t frame = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::string format;
	std::string state;
	float yaw = 0;
	float pitch = 0;
	float roll = 0;
	float x = 0;
	float y = 1.7f;
	float z = 0;
};
class WindowsRuntimeStatus
{
  public:
	explicit WindowsRuntimeStatus(std::shared_ptr<WindowsRuntimeFiles> files);
	void PublishStatus(FrameStatus status);

  private:
	std::shared_ptr<WindowsRuntimeFiles> m_files;
	std::mutex m_mutex;
	std::chrono::steady_clock::time_point m_lastStatus;
};
} // namespace simxr::windows
