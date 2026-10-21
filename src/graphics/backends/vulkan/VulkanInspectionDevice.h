#pragma once

#include "graphics/backends/vulkan/VulkanExternal.h"

#include <memory>
#include <span>

namespace simxr
{
struct VulkanInspectionValidation;

// Acquires an independent inspection instance/device matching both UUIDs. The
// returned VulkanDevice retains its native owner through every image/work reader.
class VulkanInspectionDevice
{
  public:
	VulkanInspectionDevice(const VulkanExternalIdentity& identity,
						   std::shared_ptr<VulkanInspectionValidation> validation = {});
	const std::shared_ptr<VulkanDevice>& Device() const noexcept;
	std::span<const char* const> EnabledExtensions() const noexcept;
	bool ConsumerFencesEnabled() const noexcept;

  private:
	struct Native;
	std::shared_ptr<VulkanDevice> m_device;
	bool m_consumerFences = false;
};
} // namespace simxr
