#pragma once

#include "graphics/Compositor.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <optional>

namespace simxr
{
struct PreviewRectangle
{
	float x = 0;
	float y = 0;
	float width = 0;
	float height = 0;
};

enum class PreviewEyeSelection
{
	Both,
	Left,
	Right
};

enum class PreviewEyePacking
{
	SideBySide,
	OverUnder,
	Anaglyph
};

enum class PreviewPixelAlignment
{
	Fractional,
	WholePixels
};

struct PreviewEyeViewport
{
	std::uint32_t eye;
	PreviewRectangle rectangle;
};

struct PreviewEyeLayout
{
	std::array<PreviewEyeViewport, 2> eyes{};
	std::size_t count = 0;
};

PreviewEyeLayout PreparePreviewEyeLayout(PreviewRectangle present, PreviewEyeSelection selection,
										 PreviewEyePacking packing,
										 PreviewPixelAlignment alignment = PreviewPixelAlignment::Fractional);

enum class PreviewCropSelection
{
	Submitted,
	WholeImage
};

enum class PreviewColorChannels
{
	All,
	Red,
	Cyan
};

struct PreviewProjectionSource
{
	std::uint32_t view;
	LayerImageDescription image;
	XrExtent2Di textureExtent;
	std::array<float, 2> uvOffset;
	std::array<float, 2> uvScale;
};

struct PreviewProjection
{
	std::array<std::optional<PreviewProjectionSource>, 2> eyes;
	XrExtent2Di displayExtent{};
};

struct PreviewProjectionDraw
{
	std::uint32_t sourceEye;
	PreviewEyeViewport target;
	PreviewColorChannels channels;
};

struct PreviewProjectionDraws
{
	std::array<PreviewProjectionDraw, 2> draws{};
	std::size_t count = 0;
};

// The legacy mirror displays outer views without reprojection. Insets remain
// metadata; this does not advertise a four-view Windows compositor.
std::optional<PreviewProjection> PreparePreviewProjection(const ProjectionLayerDescription& layer,
														  const CompositionExtentResolver& resolve,
														  PreviewCropSelection crop = PreviewCropSelection::Submitted);
PreviewProjectionDraws PreparePreviewProjectionDraws(const PreviewProjection& source, const PreviewEyeLayout& layout,
													 PreviewEyePacking packing);
std::optional<std::array<float, 8>> PreviewProjectionConstants(const PreviewProjectionSource& source,
															   PreviewRectangle viewport, bool encodeSrgb);

using PreviewClipCorners = std::array<std::array<float, 4>, 4>;

struct PreviewQuadEye
{
	// Fan order TL, TR, BR, BL matches the D3D12 shader. D3D11 uses a strip.
	std::array<XrVector3f, 4> viewCorners;
	std::array<float, 4> tangents;
	PreviewClipCorners stripClipCorners;
};

struct PreviewQuadGeometry
{
	LayerImageDescription source;
	std::array<float, 4> uvRectangle;
	std::array<std::optional<PreviewQuadEye>, 2> eyes;
};

// Uses only copied submission metadata and views located at that submission's
// display time. Invalid/unavailable geometry never falls back to current poses.
std::optional<PreviewQuadGeometry> PreparePreviewQuad(const QuadLayerDescription& quad, const LocatedViews& views,
													  XrExtent2Di sourceExtent);
std::array<float, 28> PreviewQuadConstants(const PreviewQuadGeometry& geometry, std::uint32_t eye, bool opaque);
// Union of visible quad pixels per eye, from the exact accepted shader constants.
std::array<XrRect2Di, 2> CompositionQuadBounds(const CompositionPlan& plan);
std::optional<XrRect2Di> PreviewQuadBounds(const PreviewQuadEye& geometry, PreviewRectangle viewport,
										   XrExtent2Di targetExtent);
} // namespace simxr
