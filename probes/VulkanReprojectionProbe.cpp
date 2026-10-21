#include "VulkanReprojectionProbe.h"
#include "Math.h"
#include "graphics/RotationalReprojection.h"
#include "graphics/backends/vulkan/VulkanInspection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>

namespace simxr::probe
{
namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}
void Await(const GpuCompletion& output)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		const auto status = output.PollCompletion();
		Require(status != CompletionStatus::Failed, "Owned composition failed");
		if (status == CompletionStatus::Complete)
			return;
		Require(std::chrono::steady_clock::now() < deadline, "Owned composition timed out");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
std::shared_ptr<const VulkanImagePixels> Pixels(const VulkanInspectionFrame& image)
{
	auto read = image.Export();
	Require(read != nullptr, "Completed composition could not be exported");
	Await(*read);
	auto pixels = read->Pixels();
	Require(pixels != nullptr, "Completed composition export omitted pixels");
	return pixels;
}
float Decode(float value)
{
	return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}
float Encode(float value)
{
	value = std::clamp(value, 0.0f, 1.0f);
	return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1 / 2.4f) - 0.055f;
}
XrColor4f Sample(const VulkanImagePixels& image, XrVector2f uv)
{
	const float x = uv.x * image.width - 0.5f, y = uv.y * image.height - 0.5f;
	const std::int32_t left = static_cast<std::int32_t>(std::floor(x)), top = static_cast<std::int32_t>(std::floor(y));
	std::array<float, 4> result{};
	for (std::int32_t row = 0; row < 2; ++row)
		for (std::int32_t column = 0; column < 2; ++column)
		{
			const auto px = std::clamp(left + column, 0, static_cast<std::int32_t>(image.width) - 1);
			const auto py = std::clamp(top + row, 0, static_cast<std::int32_t>(image.height) - 1);
			const float weight = (column ? x - left : 1 - (x - left)) * (row ? y - top : 1 - (y - top));
			for (std::size_t channel = 0; channel < 4; ++channel)
			{
				float value = image.rgba.at((py * image.width + px) * 4 + channel) / 255.0f;
				if (channel != 3 && image.format == VK_FORMAT_R8G8B8A8_SRGB)
					value = Decode(value);
				result[channel] += weight * value;
			}
		}
	return {result[0], result[1], result[2], result[3]};
}
void Verify(const CompositionPlan& plan, const VulkanImagePixels& output,
	const std::vector<std::shared_ptr<const VulkanImagePixels>>& sources)
{
	Require(output.width == plan.width && output.height == plan.height && output.layers == 2,
		"Owned composition output dimensions changed");
	for (std::uint32_t eye = 0; eye < 2; ++eye)
		for (std::uint32_t y = 0; y < plan.height; ++y)
			for (std::uint32_t x = 0; x < plan.width; ++x)
			{
				XrColor4f color{0, 0, 0, 1};
				for (const auto& draw : plan.draws)
				{
					if (draw.eye != eye)
						continue;
					const auto uv = EvaluateCompositionSample(
						draw, {(x + 0.5f) / plan.width, (y + 0.5f) / plan.height});
					if (!uv)
						continue;
					const auto index = reinterpret_cast<std::uintptr_t>(draw.source.source) - 1;
					const auto& source = *sources.at(index);
					const auto& rectangle = draw.constants.imageRectangle;
					const XrVector2f clamped{std::clamp(uv->x, rectangle[0] + 0.5f / source.width,
														rectangle[0] + rectangle[2] - 0.5f / source.width),
											 std::clamp(uv->y, rectangle[1] + 0.5f / source.height,
														rectangle[1] + rectangle[3] - 0.5f / source.height)};
					color = CompositeReferenceColor(Sample(source, clamped), color, draw.constants.control[1]);
					// The SRGB render attachment quantizes after each blended draw.
					color.r = Decode(std::round(Encode(color.r) * 255) / 255);
					color.g = Decode(std::round(Encode(color.g) * 255) / 255);
					color.b = Decode(std::round(Encode(color.b) * 255) / 255);
				}
				const std::array expected{Encode(color.r), Encode(color.g), Encode(color.b), color.a};
				for (std::size_t channel = 0; channel < 4; ++channel)
				{
					const std::int32_t actual = output.rgba.at(((eye * plan.height + y) * plan.width + x) * 4 + channel);
					const auto wanted = static_cast<std::int32_t>(std::round(expected[channel] * 255));
					if (std::abs(actual - wanted) > 3)
						std::fprintf(stderr,
									 "Composition mismatch eye=%u x=%u y=%u channel=%zu actual=%d expected=%d\n", eye,
									 x, y, channel, actual, wanted);
					Require(std::abs(actual - wanted) <= 3,
							"Retained per-layer composition pixels differ from geometry/linear-alpha reference");
				}
			}
}
void Exercise(std::vector<std::shared_ptr<VulkanInspectionFrame>> sources,
	const std::shared_ptr<VulkanInspectionFrame>& foreign, bool verify)
{
	Require(sources.size() == 5, "Expected separate outer/inset/quad retained sources");
	const PoseSample head{IdentityPose(), TrackedPoseFlags, {Duration{1}}};
	const auto fov = [](float tangent)
	{
		const float angle = std::atan(tangent);
		return XrFovf{-angle, angle, angle, -angle};
	};
	CompositionTarget target{48, 32, {{{head, fov(1)}, {head, fov(1)}}}};
	ProjectionLayerDescription projection;
	projection.space = {XR_NULL_HANDLE, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
		XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT, head, XR_REFERENCE_SPACE_TYPE_LOCAL};
	for (std::uint32_t view = 0; view < 4; ++view)
	{
		const LayerImageDescription image{reinterpret_cast<XrSwapchain>(view + 1), {{0, 0}, {4, 5}}, 0};
		projection.views.push_back({IdentityPose(), head, fov(view < 2 ? 1 : 0.35f), image, {}});
	}
	QuadLayerDescription world{projection.space, XR_EYE_VISIBILITY_RIGHT,
		{{0, 0, 0, 1}, {0.25f, 0, -2}}, head, {1.2f, 0.8f},
		{reinterpret_cast<XrSwapchain>(5), {{0, 0}, {4, 5}}, 0}};
	world.worldPose.pose = world.poseInSpace;
	auto hud = world;
	hud.space.referenceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	hud.eyes = XR_EYE_VISIBILITY_BOTH;
	hud.poseInSpace.position = {-0.3f, 0.4f, -2};
	hud.worldPose.pose = hud.poseInSpace;
	hud.size = {0.6f, 0.4f};
	FrameLayers layers{{Duration{1}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, {projection, world, hud}};
	const CompositionExtentResolver extent = [](XrSwapchain) { return XrExtent2Di{4, 5}; };
	CompositionPlan baseline, rotated;
	Require(PrepareComposition(layers, target, extent, baseline) == XR_SUCCESS, "Retained baseline plan");
	auto newer = head;
	newer.pose.orientation = EulerAngles(0.25f, -0.1f, 0.04f);
	Require(PrepareRotationalComposition(layers, target, head, newer, extent, rotated) == XR_SUCCESS,
		"Retained rotation plan");
	std::vector<std::shared_ptr<const VulkanInspectionFrame>> bindings;
	for (const auto& draw : rotated.draws)
		bindings.push_back(sources.at(reinterpret_cast<std::uintptr_t>(draw.source.source) - 1));
	std::vector<std::shared_ptr<const VulkanImagePixels>> reference;
	if (verify)
		for (const auto& source : sources)
			reference.push_back(Pixels(*source));
	auto wrong = bindings;
	wrong[0] = foreign;
	bool rejected = false;
	try
	{
		VulkanInspectionFrame::Compose(rotated, wrong);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Foreign or missing retained source accepted");
	wrong.clear();
	auto invalid = rotated;
	invalid.draws.front().source.arraySlice = 7;
	rejected = false;
	try
	{
		VulkanInspectionFrame::Compose(invalid, bindings);
	}
	catch (const VulkanError&)
	{
		rejected = true;
	}
	Require(rejected, "Invalid retained slice accepted");
	std::array<std::shared_ptr<VulkanInspectionFrame>, 3> outputs;
	outputs[0] = VulkanInspectionFrame::Compose(baseline, bindings);
	outputs[1] = VulkanInspectionFrame::Compose(rotated, bindings);
	outputs[2] = VulkanInspectionFrame::Compose(rotated, bindings);
	for (const auto& output : outputs)
	{
		Require(output != nullptr, "Completed retained sources reported pending");
		Await(*output);
	}
	rejected = false;
	try
	{
		VulkanInspectionFrame::Compose(rotated, bindings);
	}
	catch (const VulkanError& error)
	{
		rejected = error.Result() == VK_ERROR_OUT_OF_POOL_MEMORY;
	}
	Require(rejected, "Owned composition output capacity is not bounded");
	if (verify)
	{
		Verify(baseline, *Pixels(*outputs[0]), reference);
		Verify(rotated, *Pixels(*outputs[1]), reference);
	}
	for (auto& output : outputs)
		output.reset();
	auto sampling = std::async(std::launch::async, [&]
	{
		for (std::uint32_t iteration = 0; iteration < 6; ++iteration)
		{
			auto image = VulkanInspectionFrame::Compose(rotated, bindings);
			Await(*image);
			if (verify)
				Verify(rotated, *Pixels(*image), reference);
		}
	});
	auto exporting = std::async(std::launch::async, [&]
	{
		for (std::uint32_t iteration = 0; iteration < 6; ++iteration)
		{
			if (verify)
				Require(Pixels(*sources[0])->rgba == reference[0]->rgba,
					"Concurrent composition changed source pixels");
			auto delivery = sources[4]->Share();
			Await(*delivery);
		}
	});
	sampling.get();
	exporting.get();
	outputs[0] = VulkanInspectionFrame::Compose(rotated, bindings);
	bindings.clear();
	sources.clear();
	Await(*outputs[0]);
	if (verify)
		Verify(rotated, *Pixels(*outputs[0]), reference);
	auto shared = outputs[0]->Share();
	Require(shared != nullptr, "Owned composition cannot use normal GPU preview delivery");
	Await(*shared);
	std::puts("case=owned-per-layer-composition afterApplicationTeardown=1 boundedOutputs=3 PASS");
}
} // namespace
void VerifyCompositionPixels(const CompositionPlan& plan, const VulkanInspectionFrame& output,
							 const std::vector<std::shared_ptr<const VulkanImagePixels>>& sources)
{
	Verify(plan, *Pixels(output), sources);
}

void VerifyRetainedComposition(std::vector<std::shared_ptr<VulkanInspectionFrame>> linear,
	std::vector<std::shared_ptr<VulkanInspectionFrame>> srgb, bool verifyPixels)
{
	Exercise(std::move(linear), srgb.front(), verifyPixels);
	Exercise(std::move(srgb), {}, verifyPixels);
}
} // namespace simxr::probe
