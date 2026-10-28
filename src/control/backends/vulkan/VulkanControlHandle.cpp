#include "control/backends/vulkan/VulkanControlHandle.h"
#include "Pch.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace simxr::control
{
class VulkanControlHandle final : public TransferHandle
{
  public:
	explicit VulkanControlHandle(VulkanExternalHandle handle) : m_handle(std::move(handle)) {}
	TransferHandleKind Kind() const noexcept override
	{
		return VulkanTransferKind;
	}
	std::intptr_t Value() const noexcept override
	{
#ifdef _WIN32
		return std::bit_cast<std::intptr_t>(m_handle.Get());
#else
		return m_handle.Get();
#endif
	}
	std::shared_ptr<TransferHandle> Duplicate() const override
	{
		return OwnVulkanControlHandle(m_handle.Duplicate());
	}

  private:
	VulkanExternalHandle m_handle;
};
std::shared_ptr<TransferHandle> OwnVulkanControlHandle(VulkanExternalHandle handle)
{
	return std::make_shared<VulkanControlHandle>(std::move(handle));
}
VulkanExternalHandle DuplicateVulkanControlHandle(const std::shared_ptr<TransferHandle>& handle)
{
	if (!handle || handle->Kind() != VulkanTransferKind)
		throw std::invalid_argument("Incompatible native Vulkan transfer handle");
#ifdef _WIN32
	if (handle->Value() <= 0)
		throw std::invalid_argument("Invalid Windows Vulkan transfer handle");
	HANDLE copy = nullptr;
	if (!DuplicateHandle(GetCurrentProcess(), std::bit_cast<HANDLE>(handle->Value()), GetCurrentProcess(), &copy, 0,
						 FALSE, DUPLICATE_SAME_ACCESS))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
								"Duplicate Vulkan transfer handle");
#else
	if (handle->Value() < 0 || handle->Value() > std::numeric_limits<int>::max())
		throw std::invalid_argument("Invalid Vulkan transfer descriptor");
	int copy;
	do
	{
		copy = fcntl(static_cast<int>(handle->Value()), F_DUPFD_CLOEXEC, 0);
	} while (copy < 0 && errno == EINTR);
	if (copy < 0)
		throw std::system_error(errno, std::generic_category(), "Duplicate Vulkan transfer descriptor");
#endif
	return VulkanExternalHandle(copy);
}
} // namespace simxr::control
