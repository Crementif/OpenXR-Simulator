#pragma once

#include "control/ControlPacket.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

namespace simxr::control
{
#ifdef _WIN32
inline constexpr auto VulkanTransferKind = TransferHandleKind::OpaqueWin32;
#else
inline constexpr auto VulkanTransferKind = TransferHandleKind::OpaqueFd;
#endif

std::shared_ptr<TransferHandle> OwnVulkanControlHandle(VulkanExternalHandle handle);
VulkanExternalHandle DuplicateVulkanControlHandle(const std::shared_ptr<TransferHandle>& handle);
} // namespace simxr::control
