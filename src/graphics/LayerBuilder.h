#pragma once

#include "runtime/validation/LayerValidation.h"
#include "simulation/TrackingTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace simxr
{
struct LayerImageDescription
{
	XrSwapchain source = XR_NULL_HANDLE;
	XrRect2Di rectangle{};
	std::uint32_t arraySlice = 0;
};

struct LayerDepthDescription
{
	LayerImageDescription image;
	float minDepth;
	float maxDepth;
	float nearZ;
	float farZ;
};

struct LayerSpaceDescription
{
	XrSpace source = XR_NULL_HANDLE;
	XrCompositionLayerFlags flags = 0;
	PoseSample worldPose;
	XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
	XrPosef poseOffset{{0, 0, 0, 1}, {0, 0, 0}};
};

struct ProjectionViewDescription
{
	XrPosef poseInSpace;
	PoseSample worldPose;
	XrFovf fov;
	LayerImageDescription color;
	std::optional<LayerDepthDescription> depth;
};

struct ProjectionLayerDescription
{
	LayerSpaceDescription space;
	std::vector<ProjectionViewDescription> views;
};

struct QuadLayerDescription
{
	LayerSpaceDescription space;
	XrEyeVisibility eyes;
	XrPosef poseInSpace;
	PoseSample worldPose;
	XrExtent2Df size;
	LayerImageDescription color;
};

using LayerDescription = std::variant<ProjectionLayerDescription, QuadLayerDescription>;

// All data is copied. Source handles are diagnostic identities, not owners of
// swapchain images, spaces, native resources, or application memory.
struct FrameLayers
{
	FrameLayers(RuntimeTime time, XrEnvironmentBlendMode blend, std::vector<LayerDescription> descriptions)
		: displayTime(time), blendMode(blend), layers(std::move(descriptions))
	{
	}
	const RuntimeTime displayTime;
	const XrEnvironmentBlendMode blendMode;
	const std::vector<LayerDescription> layers;
};

using LayerSpaceResolver = std::function<XrResult(XrSpace, RuntimeTime, SpaceSample&)>;
XrResult BuildLayers(const XrFrameEndInfo& info, const LayerValidationContext& validation,
					 const LayerSpaceResolver& resolve, std::shared_ptr<const FrameLayers>& output);
std::optional<LayerImageDescription> FindLayerImage(const FrameLayers& frame, std::uint32_t layer, std::uint32_t view);
const LayerDepthDescription* FindLayerDepth(const FrameLayers& frame, std::uint32_t layer, std::uint32_t view);
} // namespace simxr
