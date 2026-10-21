#pragma once

#include "runtime/ExtensionRegistry.h"
#include "runtime/extensions/BodyTracking.h"

#include <span>

namespace simxr
{
struct SimulationExtensionCapabilities
{
	bool quadViews = false;
	bool foveatedRendering = false;
};
std::span<const ExtensionDefinition> SimulationExtensions(SimulationExtensionCapabilities capabilities = {});
XrResult FillSimulationProperties(const EnabledExtensions& enabled, XrSystemProperties* properties,
								  const BodyTrackingCapabilities& capabilities = {});
} // namespace simxr
