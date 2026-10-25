#include "../TestSupport.h"
#include "math/Math.h"
#include "control/legacy/LegacyCaptureImage.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
using namespace simxr;

constexpr XrFovf OuterFov{-0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f};

CapturePixels Solid(std::uint32_t width, std::uint32_t height, std::array<std::uint8_t, 4> color,
					CaptureColorOrder order = CaptureColorOrder::Bgra,
					CaptureColorEncoding encoding = CaptureColorEncoding::Srgb)
{
	CapturePixels source{width, height, order, encoding, std::vector<std::uint8_t>(CapturePixelBytes(width, height))};
	for (std::size_t index = 0; index < source.pixels.size(); index += 4)
		std::copy(color.begin(), color.end(), source.pixels.begin() + static_cast<std::ptrdiff_t>(index));
	return source;
}

LayerImageDescription Image(std::size_t source)
{
	return {reinterpret_cast<XrSwapchain>(source + 1), {{3, 4}, {2, 2}}, 0};
}

ProjectionLayerDescription Projection(std::size_t left = 0, std::size_t right = 1)
{
	ProjectionLayerDescription projection;
	for (const auto source : {left, right})
		projection.views.push_back({IdentityPose(), {IdentityPose(), ValidPoseFlags}, OuterFov, Image(source), {}});
	return projection;
}

QuadLayerDescription Quad(std::size_t source = 1)
{
	auto pose = IdentityPose();
	pose.position.z = -1;
	return {{}, XR_EYE_VISIBILITY_BOTH, pose, {pose, ValidPoseFlags}, {2, 2}, Image(source)};
}

CompositionTarget Target(std::uint32_t width = 2, std::uint32_t height = 2)
{
	const CompositionEye eye{{IdentityPose(), ValidPoseFlags}, OuterFov};
	return {width, height, {eye, eye}};
}

CaptureImagePlan Plan(std::vector<LayerDescription> layers, CompositionTarget target = Target())
{
	CompositionPlan composition;
	const FrameLayers frame{{Duration{1}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, std::move(layers)};
	Require(PrepareComposition(
				frame, target,
				[](XrSwapchain)
				{
					return XrExtent2Di{8, 8};
				},
				composition) == XR_SUCCESS,
			"Shared compositor accepts capture geometry");
	CaptureImagePlan capture{composition.width, composition.height, 0, 2, {}};
	for (const auto& draw : composition.draws)
	{
		CaptureImageDraw copied{draw.constants, draw.eye, reinterpret_cast<std::size_t>(draw.source.source) - 1};
		copied.constants.imageRectangle = {0, 0, 1, 1};
		capture.draws.push_back(copied);
	}
	return capture;
}

std::array<std::uint8_t, 4> Pixel(const std::vector<std::uint8_t>& image, std::size_t offset)
{
	return {image.at(offset), image.at(offset + 1), image.at(offset + 2), image.at(offset + 3)};
}

void ProjectionPackingAndEncoding()
{
	const std::array sources{Solid(2, 2, {10, 20, 30, 40}, CaptureColorOrder::Rgba), Solid(2, 2, {50, 60, 70, 80})};
	auto plan = Plan({Projection()});
	const auto image = ComposeCaptureImage(plan, sources);
	Require(image.size() == 32, "Stereo capture packs both eye rows");
	for (std::size_t row = 0; row < 2; ++row)
	{
		Require(Pixel(image, row * 16) == std::array<std::uint8_t, 4>{30, 20, 10, 255},
				"RGBA sRGB source is packed BGRA with canonical opaque alpha");
		Require(Pixel(image, row * 16 + 8) == std::array<std::uint8_t, 4>{50, 60, 70, 255},
				"BGRA sRGB source preserves color");
	}
	plan.firstEye = 1;
	plan.eyeCount = 1;
	Require(Pixel(ComposeCaptureImage(plan, sources), 0) == std::array<std::uint8_t, 4>{50, 60, 70, 255},
			"Right-eye selection excludes left pixels");
	const std::array linear{Solid(2, 2, {128, 128, 128, 0}, CaptureColorOrder::Bgra, CaptureColorEncoding::Linear)};
	plan = Plan({Projection(0, 0)});
	Require(Pixel(ComposeCaptureImage(plan, linear), 0) == std::array<std::uint8_t, 4>{188, 188, 188, 255},
			"UNORM sources are linear and the capture output is sRGB encoded");
}

void AlphaAndVisibility()
{
	std::array sources{Solid(2, 2, {255, 0, 0, 255}), Solid(2, 2, {255, 0, 0, 128}, CaptureColorOrder::Rgba)};
	auto quad = Quad();
	quad.space.flags =
		XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	auto plan = Plan({Projection(0, 0), quad});
	const auto blended = ComposeCaptureImage(plan, sources);
	Require(Pixel(blended, 0) == std::array<std::uint8_t, 4>{187, 0, 188, 255},
			"Straight alpha blends in linear color before sRGB output encoding");
	sources[1] = Solid(2, 2, {128, 0, 0, 128}, CaptureColorOrder::Rgba, CaptureColorEncoding::Linear);
	quad.space.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	plan = Plan({Projection(0, 0), quad});
	Require(ComposeCaptureImage(plan, sources) == blended, "Premultiplied linear source retains its contribution");
	quad.eyes = XR_EYE_VISIBILITY_RIGHT;
	plan = Plan({Projection(0, 0), quad});
	const auto selected = ComposeCaptureImage(plan, sources);
	Require(Pixel(selected, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255} &&
				Pixel(selected, 8) == Pixel(blended, 0),
			"Quad visibility follows the actual eye");
	plan.firstEye = 1;
	plan.eyeCount = 1;
	Require(Pixel(ComposeCaptureImage(plan, sources), 0) == Pixel(blended, 0),
			"Mono right capture retains right-eye visibility");
	quad.space.flags = 0;
	quad.eyes = XR_EYE_VISIBILITY_BOTH;
	sources[1] = Solid(2, 2, {255, 0, 0, 0}, CaptureColorOrder::Rgba);
	Require(Pixel(ComposeCaptureImage(Plan({Projection(0, 0), quad}), sources), 0) ==
				std::array<std::uint8_t, 4>{0, 0, 255, 255},
			"Opaque quad ignores the source alpha byte");
}

void FilteringAndCroppedOrientation()
{
	const std::array sources{CapturePixels{2,
										   2,
										   CaptureColorOrder::Bgra,
										   CaptureColorEncoding::Srgb,
										   {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255}}};
	const auto filtered = ComposeCaptureImage(Plan({Projection(0, 0)}, Target(1, 1)), sources);
	Require(Pixel(filtered, 0) == std::array<std::uint8_t, 4>{188, 188, 188, 255},
			"Bilinear sampling decodes sRGB before filtering");
	const std::array cropped{CapturePixels{2,
										   2,
										   CaptureColorOrder::Bgra,
										   CaptureColorEncoding::Srgb,
										   {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}}};
	const auto quad = Quad(0);
	const auto expected = ComposeCaptureImage(Plan({quad}), cropped);
	Require(Pixel(expected, 0) == Pixel(cropped[0].pixels, 0) && Pixel(expected, 20) == Pixel(cropped[0].pixels, 12),
			"Rebased cropped quad samples preserve top-left orientation without outside pixels");
	const XrPosef parent{EulerAngles(.3f, -.2f, .4f), {1, 2, 3}};
	auto transformed = quad;
	transformed.worldPose.pose = Compose(parent, quad.worldPose.pose);
	auto target = Target();
	for (auto& eye : target.eyes)
		eye.worldPose.pose = parent;
	Require(ComposeCaptureImage(Plan({transformed}, target), cropped) == expected,
			"Common world transform preserves relative quad geometry");
	transformed.worldPose.pose = IdentityPose();
	transformed.worldPose.pose.position.z = 1;
	Require(Pixel(ComposeCaptureImage(Plan({transformed}), cropped), 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255},
			"Quad back face leaves the canonical opaque black clear");
}

void FourViewsAndSubmissionOrder()
{
	const std::array sources{Solid(2, 2, {255, 0, 0, 255}), Solid(2, 2, {0, 255, 0, 255}),
							 Solid(2, 2, {0, 0, 255, 255}), Solid(2, 2, {0, 255, 255, 255})};
	auto projection = Projection();
	for (std::size_t source = 2; source < 4; ++source)
	{
		auto inset = projection.views[source - 2];
		inset.fov = {-std::atan(.5f), std::atan(.5f), std::atan(.5f), -std::atan(.5f)};
		inset.color = Image(source);
		projection.views.push_back(inset);
	}
	const auto image = ComposeCaptureImage(Plan({projection}, Target(4, 4)), sources);
	Require(Pixel(image, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255} &&
				Pixel(image, 16) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
			"Outer views cover both eye outputs");
	Require(Pixel(image, 36) == std::array<std::uint8_t, 4>{0, 0, 255, 255} &&
				Pixel(image, 52) == std::array<std::uint8_t, 4>{0, 255, 255, 255},
			"Four-view insets cover only their FOV");
	const auto quad = Quad(2);
	Require(Pixel(ComposeCaptureImage(Plan({quad, Projection(0, 0)}), sources), 0) ==
				std::array<std::uint8_t, 4>{255, 0, 0, 255},
			"Later projection preserves submitted layer order");
	Require(Pixel(ComposeCaptureImage(Plan({Projection(0, 0), quad}), sources), 0) ==
				std::array<std::uint8_t, 4>{0, 0, 255, 255},
			"Later quad preserves submitted layer order");
}

void InvalidDataDoesNotReachPixels()
{
	bool overflow = false;
	try
	{
		CapturePixelBytes(UINT32_MAX, UINT32_MAX);
	}
	catch (const std::invalid_argument&)
	{
		overflow = true;
	}
	Require(overflow, "Pixel bounds are checked before allocation");
	auto plan = Plan({Projection(0, 0)});
	std::array sources{Solid(2, 2, {0, 0, 0, 0})};
	sources[0].pixels.pop_back();
	bool truncated = false;
	try
	{
		ComposeCaptureImage(plan, sources);
	}
	catch (const std::invalid_argument&)
	{
		truncated = true;
	}
	Require(truncated, "Truncated source pixels are rejected before sampling");
	sources[0] = Solid(2, 2, {0, 0, 0, 0});
	plan.draws[0].source = 1;
	bool missing = false;
	try
	{
		ComposeCaptureImage(plan, sources);
	}
	catch (const std::invalid_argument&)
	{
		missing = true;
	}
	Require(missing, "Missing captured sources are rejected");
	plan.draws[0].source = 0;
	plan.firstEye = 2;
	bool invalidEye = false;
	try
	{
		ComposeCaptureImage(plan, sources);
	}
	catch (const std::invalid_argument&)
	{
		invalidEye = true;
	}
	Require(invalidEye, "Invalid eye selection cannot index output storage");
}
} // namespace

int main()
{
	try
	{
		ProjectionPackingAndEncoding();
		AlphaAndVisibility();
		FilteringAndCroppedOrientation();
		FourViewsAndSubmissionOrder();
		InvalidDataDoesNotReachPixels();
		std::cout << "Legacy capture image checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
