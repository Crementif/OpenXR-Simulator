#include "control/legacy/LegacyCaptureImage.h"
#include "Pch.h"

namespace simxr
{
static constexpr std::size_t s_maxPixelBytes = 512 * 1024 * 1024;

static void ValidatePixels(const CapturePixels& source)
{
	if ((source.order != CaptureColorOrder::Rgba && source.order != CaptureColorOrder::Bgra) ||
		(source.encoding != CaptureColorEncoding::Linear && source.encoding != CaptureColorEncoding::Srgb) ||
		source.pixels.size() != CapturePixelBytes(source.width, source.height))
		throw std::invalid_argument("Capture source pixels do not match their format and extent");
}

static float DecodeColor(std::uint8_t channel, CaptureColorEncoding encoding)
{
	if (encoding == CaptureColorEncoding::Linear)
		return channel / 255.0f;
	static const auto s_linearColors = []
	{
		std::array<float, 256> colors{};
		for (std::size_t index = 0; index < colors.size(); ++index)
		{
			const auto value = index / 255.0f;
			colors[index] = value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		}
		return colors;
	}();
	return s_linearColors[channel];
}

static std::uint8_t EncodeColor(float linear)
{
	linear = std::clamp(linear, 0.0f, 1.0f);
	const auto srgb = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
	return static_cast<std::uint8_t>(std::clamp(std::lround(srgb * 255), 0L, 255L));
}

static XrColor4f Texel(const CapturePixels& source, std::uint32_t x, std::uint32_t y)
{
	const auto offset = (static_cast<std::size_t>(y) * source.width + x) * 4;
	const auto* pixel = source.pixels.data() + offset;
	const auto red = source.order == CaptureColorOrder::Rgba ? 0u : 2u;
	const auto blue = 2u - red;
	return {DecodeColor(pixel[red], source.encoding), DecodeColor(pixel[1], source.encoding),
			DecodeColor(pixel[blue], source.encoding), pixel[3] / 255.0f};
}

static XrColor4f Interpolate(XrColor4f first, XrColor4f second, float weight)
{
	return {std::lerp(first.r, second.r, weight), std::lerp(first.g, second.g, weight),
			std::lerp(first.b, second.b, weight), std::lerp(first.a, second.a, weight)};
}

static XrColor4f SampleLinear(const CapturePixels& source, XrVector2f coordinates)
{
	const auto x = std::clamp(coordinates.x * source.width - 0.5f, 0.0f, static_cast<float>(source.width - 1));
	const auto y = std::clamp(coordinates.y * source.height - 0.5f, 0.0f, static_cast<float>(source.height - 1));
	const auto left = static_cast<std::uint32_t>(x);
	const auto top = static_cast<std::uint32_t>(y);
	const auto right = std::min(left + 1, source.width - 1);
	const auto bottom = std::min(top + 1, source.height - 1);
	const auto upper = Interpolate(Texel(source, left, top), Texel(source, right, top), x - left);
	const auto lower = Interpolate(Texel(source, left, bottom), Texel(source, right, bottom), x - left);
	return Interpolate(upper, lower, y - top);
}

std::size_t CapturePixelBytes(std::uint32_t width, std::uint32_t height)
{
	const auto pixels = std::uint64_t{width} * height;
	if (!pixels || pixels > s_maxPixelBytes / 4)
		throw std::invalid_argument("Capture pixel extent exceeds the 512 MiB bound");
	return static_cast<std::size_t>(pixels * 4);
}

std::vector<std::uint8_t> ComposeCaptureImage(const CaptureImagePlan& plan, std::span<const CapturePixels> sources)
{
	if (plan.firstEye > 1 || plan.eyeCount == 0 || plan.eyeCount > 2 - plan.firstEye || sources.size() > 64 ||
		plan.draws.size() > 128)
		throw std::invalid_argument("Capture requires one or two valid eye outputs and bounded sources");
	const auto eyeBytes = CapturePixelBytes(plan.width, plan.height);
	if (eyeBytes > s_maxPixelBytes / plan.eyeCount)
		throw std::invalid_argument("Packed capture exceeds the 512 MiB bound");
	std::size_t sourceBytes = 0;
	for (const auto& source : sources)
	{
		ValidatePixels(source);
		if (source.pixels.size() > s_maxPixelBytes - sourceBytes)
			throw std::invalid_argument("Capture sources exceed the 512 MiB bound");
		sourceBytes += source.pixels.size();
	}
	for (const auto& draw : plan.draws)
		if (draw.eye > 1 || draw.source >= sources.size())
			throw std::invalid_argument("Capture draw has no valid eye or source");

	const auto packedWidth = static_cast<std::size_t>(plan.width) * plan.eyeCount;
	std::vector<std::uint8_t> output(eyeBytes * plan.eyeCount);
	for (std::uint32_t y = 0; y < plan.height; ++y)
		for (std::uint32_t eyeOffset = 0; eyeOffset < plan.eyeCount; ++eyeOffset)
			for (std::uint32_t x = 0; x < plan.width; ++x)
			{
				const XrVector2f eyeUV{(x + 0.5f) / plan.width, (y + 0.5f) / plan.height};
				XrColor4f color{0, 0, 0, 1};
				for (const auto& draw : plan.draws)
				{
					if (draw.eye != plan.firstEye + eyeOffset)
						continue;
					if (const auto sourceUV = EvaluateCompositionSample(draw.constants, eyeUV))
						color = CompositeReferenceColor(SampleLinear(sources[draw.source], *sourceUV), color,
														draw.constants.control[1]);
				}
				const auto offset = (static_cast<std::size_t>(y) * packedWidth + eyeOffset * plan.width + x) * 4;
				output[offset] = EncodeColor(color.b);
				output[offset + 1] = EncodeColor(color.g);
				output[offset + 2] = EncodeColor(color.r);
				output[offset + 3] = static_cast<std::uint8_t>(std::clamp(std::lround(color.a * 255), 0L, 255L));
			}
	return output;
}
} // namespace simxr
