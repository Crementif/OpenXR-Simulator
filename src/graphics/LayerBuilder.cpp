#include "graphics/LayerBuilder.h"
#include "Math.h"
#include "runtime/validation/StructureChain.h"

#include <utility>

namespace simxr
{
namespace
{
LayerImageDescription Image(const XrSwapchainSubImage& image)
{
	return {image.swapchain, image.imageRect, image.imageArrayIndex};
}

PoseSample Offset(PoseSample world, const XrPosef& offset)
{
	const bool translated = offset.position.x != 0 || offset.position.y != 0 || offset.position.z != 0;
	if (translated && !(world.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		world.flags &= ~(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
	if (translated && !(world.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		world.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	world.pose = Compose(world.pose, offset);
	return world;
}
} // namespace

XrResult BuildLayers(const XrFrameEndInfo& info, const LayerValidationContext& validation,
					 const LayerSpaceResolver& resolve, std::shared_ptr<const FrameLayers>& output)
{
	auto result = ValidateLayers(info, validation);
	if (XR_FAILED(result))
		return result;
	if (info.layerCount && !resolve)
		return XR_ERROR_VALIDATION_FAILURE;
	const RuntimeTime time{Duration{info.displayTime}};
	std::vector<LayerDescription> descriptions;
	descriptions.reserve(info.layerCount);
	for (std::uint32_t index = 0; index < info.layerCount; ++index)
	{
		const auto& layer = *info.layers[index];
		SpaceSample sample;
		result = resolve(layer.space, time, sample);
		if (XR_FAILED(result))
			return result;
		LayerSpaceDescription space{
			layer.space, layer.layerFlags, sample.worldPose, sample.referenceType, sample.poseOffset};
		if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
		{
			const auto& projection = reinterpret_cast<const XrCompositionLayerProjection&>(layer);
			ProjectionLayerDescription description{space, {}};
			description.views.reserve(projection.viewCount);
			for (std::uint32_t viewIndex = 0; viewIndex < projection.viewCount; ++viewIndex)
			{
				const auto& view = projection.views[viewIndex];
				ProjectionViewDescription copied{
					view.pose, Offset(space.worldPose, view.pose), view.fov, Image(view.subImage), {}};
				const auto depth = StructureChain(view.next).Find<XrCompositionLayerDepthInfoKHR>(
					XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR);
				if (depth)
					copied.depth = LayerDepthDescription{Image(depth->subImage), depth->minDepth, depth->maxDepth,
														 depth->nearZ, depth->farZ};
				description.views.push_back(std::move(copied));
			}
			descriptions.emplace_back(std::move(description));
		}
		else
		{
			const auto& quad = reinterpret_cast<const XrCompositionLayerQuad&>(layer);
			descriptions.emplace_back(QuadLayerDescription{space, quad.eyeVisibility, quad.pose,
														   Offset(space.worldPose, quad.pose), quad.size,
														   Image(quad.subImage)});
		}
	}
	auto prepared = std::make_shared<const FrameLayers>(time, info.environmentBlendMode, std::move(descriptions));
	output = std::move(prepared);
	return XR_SUCCESS;
}

std::optional<LayerImageDescription> FindLayerImage(const FrameLayers& frame, std::uint32_t layer, std::uint32_t view)
{
	if (layer >= frame.layers.size())
		return {};
	if (const auto* projection = std::get_if<ProjectionLayerDescription>(&frame.layers[layer]))
	{
		if (view < projection->views.size())
			return projection->views[view].color;
		return {};
	}
	if (const auto* quad = std::get_if<QuadLayerDescription>(&frame.layers[layer]); quad && !view)
		return quad->color;
	return {};
}
const LayerDepthDescription* FindLayerDepth(const FrameLayers& frame, std::uint32_t layer, std::uint32_t view)
{
	if (layer < frame.layers.size())
		if (const auto* projection = std::get_if<ProjectionLayerDescription>(&frame.layers[layer]);
			projection && view < projection->views.size() && projection->views[view].depth)
			return &*projection->views[view].depth;
	return nullptr;
}
} // namespace simxr
