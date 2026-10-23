#pragma once

#include "graphics/FrameOutput.h"
#include "graphics/LayerBuilder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace simxr
{
struct alignas(16) LayerConstants
{
	std::array<float, 4> eyeTangents;
	std::array<float, 4> sourceTangents;
	std::array<float, 4> sourceFromEyeRotation;
	std::array<float, 4> sourceFromEyePosition;
	std::array<float, 4> imageRectangle;
	std::array<float, 4> quadParameters;
	std::array<std::uint32_t, 4> control;
};
static_assert(sizeof(LayerConstants) == 112 && alignof(LayerConstants) == 16);
static_assert(offsetof(LayerConstants, control) == 96 && offsetof(LayerConstants, imageRectangle) == 64);

struct CompositionEye
{
	PoseSample worldPose;
	XrFovf fov;
};
// Width/height describe each eye output; the backend owns stereo packing.
struct CompositionTarget
{
	std::uint32_t width;
	std::uint32_t height;
	std::array<CompositionEye, 2> eyes;
};
struct CompositionDraw
{
	LayerImageDescription source;
	std::uint32_t eye;
	LayerConstants constants;
};
struct CompositionPlan
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<CompositionDraw> draws;
	std::vector<FrameView> views;
};
using CompositionExtentResolver = std::function<std::optional<XrExtent2Di>(XrSwapchain)>;
XrResult PrepareComposition(const FrameLayers& frame, const CompositionTarget& target,
							const CompositionExtentResolver& resolve, CompositionPlan& output);

XrResult PrepareDepthComposition(const LayerDepthDescription& depth, bool linear, XrExtent2Di extent, bool multisampled,
								 CompositionPlan& output);

// CPU reference for shader ray/crop equations. It evaluates geometry only and
// returns canonical top-left texture coordinates; it performs no pixel sampling.
std::optional<XrVector2f> EvaluateCompositionSample(const CompositionDraw& draw, XrVector2f eyeUV);
std::optional<XrVector2f> EvaluateCompositionSample(const LayerConstants& constants, XrVector2f eyeUV);
// Linear-color CPU reference; source alpha flags are constants.control[1].
XrColor4f CompositeReferenceColor(XrColor4f source, XrColor4f premultipliedDestination, std::uint32_t alphaFlags);
// Union of visible quad pixels per eye, from the accepted shader constants.
std::array<XrRect2Di, 2> CompositionQuadBounds(const CompositionPlan& plan);
} // namespace simxr
