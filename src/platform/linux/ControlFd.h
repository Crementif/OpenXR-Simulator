#pragma once

#include "control/ControlPacket.h"

#include <memory>

namespace simxr::control
{
// Takes ownership immediately, including every exceptional path.
std::shared_ptr<TransferHandle> OwnControlFd(int fd);
// Does not consume the caller's descriptor; the duplicate is close-on-exec.
std::shared_ptr<TransferHandle> DuplicateControlFd(int fd);
} // namespace simxr::control
