#pragma once

#include "control/ImageInspection.h"

#include <functional>
#include <memory>

namespace simxr::control
{
using ImageCapabilityQuery = std::function<ImageCapabilities(XrSession)>;
std::shared_ptr<ImageInspectionProvider> CreateVulkanImageInspectionProvider(ImageCapabilityQuery query = {});
} // namespace simxr::control
