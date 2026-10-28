#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <httplib.h>

namespace simxr::app::windows
{
httplib::Result CancelablePost(std::uint16_t port, const httplib::Headers& headers, const std::string& body,
								  const std::atomic<bool>& stopped, std::chrono::seconds timeout);
} // namespace simxr::app::windows
