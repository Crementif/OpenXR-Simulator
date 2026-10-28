#include "app/mcp/StereoSurface.h"
#include "app/mcp/McpPch.h"
#include "app/mcp/StereoImageOps.h"

namespace simxr::app::mcp
{
struct SurfaceFeatures
{
	float texture, red, blue;
};

static std::vector<SurfaceFeatures> Features(std::span<const std::uint8_t> rgb, int width, int height)
{
	const auto count = std::size_t(width) * height;
	std::vector<float> luminance(count), squared(count);
	for (std::size_t i = 0; i < count; ++i)
	{
		luminance[i] = (rgb[i * 3] * .299f + rgb[i * 3 + 1] * .587f + rgb[i * 3 + 2] * .114f) / 255;
		squared[i] = luminance[i] * luminance[i];
	}
	const auto mean = StereoBoxMean(luminance, width, height, 3, 3);
	const auto meanSquared = StereoBoxMean(squared, width, height, 3, 3);
	std::vector<SurfaceFeatures> features(count);
	for (std::size_t i = 0; i < count; ++i)
	{
		const auto variance = std::max(0.0f, meanSquared[i] - mean[i] * mean[i]);
		const auto total = (rgb[i * 3] + rgb[i * 3 + 1] + rgb[i * 3 + 2]) / 255.0f + .06f;
		features[i] = {(luminance[i] - mean[i]) / (std::sqrt(variance) + .02f) * .35f,
					   rgb[i * 3] / 255.0f / total * 4, rgb[i * 3 + 2] / 255.0f / total * 4};
	}
	return features;
}

static std::vector<float> CostVolume(const std::vector<SurfaceFeatures>& left,
									 const std::vector<SurfaceFeatures>& right, int width, int height,
									 int minimum, int maximum, bool coarse, const std::function<void()>& check)
{
	const int disparities = maximum - minimum + 1;
	const auto count = std::size_t(width) * height;
	std::vector<float> volume(count * disparities), difference(count), rows(count);
	for (int d = minimum; d <= maximum; ++d)
	{
		check();
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const auto i = std::size_t(y) * width + x;
				const auto rightX = x - d;
				if (rightX < 0 || rightX >= width)
					difference[i] = 2.5f;
				else
				{
					const auto& a = left[i];
					const auto& b = right[std::size_t(y) * width + rightX];
					difference[i] = std::min(2.5f, std::abs(a.texture - b.texture) +
						std::abs(a.red - b.red) + std::abs(a.blue - b.blue));
				}
			}
		if (coarse)
			rows = StereoBoxMean(difference, width, height, 2, 2);
		else
		{
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					const auto i = std::size_t(y) * width + x;
					rows[i] = difference[i] + (y ? difference[i - width] : 0) +
						(y + 1 < height ? difference[i + width] : 0);
				}
		}
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const auto i = std::size_t(y) * width + x;
				const auto score = coarse ? rows[i] : (rows[i] + (x ? rows[i - 1] : 0) +
					(x + 1 < width ? rows[i + 1] : 0)) / 9;
				volume[i * disparities + (d - minimum)] = score;
			}
	}
	return volume;
}

static std::vector<float> Aggregate(const std::vector<float>& costs, int width, int height, int disparities,
									 const std::function<void()>& check)
{
	std::vector<float> result(costs.size(), 0), previous(disparities), current(disparities);
	for (int axis = 0; axis < 2; ++axis)
		for (int reverse = 0; reverse < 2; ++reverse)
		{
			const auto lines = axis == 0 ? height : width;
			const auto length = axis == 0 ? width : height;
			for (int line = 0; line < lines; ++line)
			{
				check();
				for (int step = 0; step < length; ++step)
				{
					const auto position = reverse ? length - step - 1 : step;
					const auto x = axis == 0 ? position : line;
					const auto y = axis == 0 ? line : position;
					const auto base = (std::size_t(y) * width + x) * disparities;
					const auto floor = step ? *std::min_element(previous.begin(), previous.end()) : 0;
					for (int d = 0; d < disparities; ++d)
					{
						auto value = costs[base + d];
						if (step)
						{
							auto best = std::min(previous[d], floor + 1.0f);
							if (d > 0)
								best = std::min(best, previous[d - 1] + .1f);
							if (d + 1 < disparities)
								best = std::min(best, previous[d + 1] + .1f);
							value += best - floor;
						}
						current[d] = value;
						result[base + d] += value * .25f;
					}
					previous.swap(current);
				}
			}
		}
	return result;
}

struct MatchPlanes
{
	std::vector<float> disparity, uniqueness, right;
};

static MatchPlanes Select(const std::vector<float>& volume, int width, int height, int minimum, int maximum,
						  const std::function<void()>& check)
{
	const auto count = std::size_t(width) * height;
	const int disparities = maximum - minimum + 1;
	MatchPlanes result{std::vector<float>(count), std::vector<float>(count), std::vector<float>(count)};
	for (int y = 0; y < height; ++y)
	{
		check();
		for (int x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			const auto base = i * disparities;
			const auto first = volume.begin() + base;
			const auto selected = static_cast<int>(std::min_element(first, first + disparities) - first);
			const auto best = volume[base + selected];
			const auto low = volume[base + std::max(0, selected - 1)];
			const auto high = volume[base + std::min(disparities - 1, selected + 1)];
			const auto denominator = low + high - 2 * best;
			const auto subpixel = denominator > 1e-6f ? std::clamp(.5f * (low - high) / denominator, -.5f, .5f) : 0;
			result.disparity[i] = minimum + selected + subpixel;
			const auto away = std::min(volume[base + std::max(0, selected - 4)],
				volume[base + std::min(disparities - 1, selected + 4)]);
			result.uniqueness[i] = selected == 0 || selected == disparities - 1 ? 0 : (away - best) / (away + .001f);
			float bestRight = std::numeric_limits<float>::infinity();
			int rightDisparity = 0;
			for (int d = minimum; d <= maximum; ++d)
			{
				const auto leftX = x + d;
				const auto cost = leftX < 0 || leftX >= width ? 99.0f :
					volume[(std::size_t(y) * width + leftX) * disparities + d - minimum];
				if (cost < bestRight)
				{
					bestRight = cost;
					rightDisparity = d;
				}
			}
			result.right[i] = static_cast<float>(rightDisparity);
		}
	}
	return result;
}

static float SampleRow(const std::vector<float>& values, int width, int y, float x)
{
	x = std::clamp(x, 0.0f, width - 1.001f);
	const auto column = static_cast<int>(x);
	const auto fraction = x - column;
	const auto index = std::size_t(y) * width + column;
	return values[index] * (1 - fraction) + values[index + 1] * fraction;
}

static float Percentile(std::vector<float> values, double fraction)
{
	std::sort(values.begin(), values.end());
	const auto position = fraction * (values.size() - 1);
	const auto index = static_cast<std::size_t>(position);
	const auto weight = static_cast<float>(position - index);
	return values[index] * (1 - weight) + values[std::min(index + 1, values.size() - 1)] * weight;
}

static std::pair<int, int> Bounds(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
								  int width, int height, const std::function<void()>& check)
{
	const int scale = std::max(1, width / 120);
	const int smallWidth = (width + scale - 1) / scale, smallHeight = (height + scale - 1) / scale;
	std::vector<std::uint8_t> a(std::size_t(smallWidth) * smallHeight * 3), b(a.size());
	for (int y = 0; y < smallHeight; ++y)
		for (int x = 0; x < smallWidth; ++x)
			for (int c = 0; c < 3; ++c)
			{
				const auto source = (std::size_t(y * scale) * width + x * scale) * 3 + c;
				const auto target = (std::size_t(y) * smallWidth + x) * 3 + c;
				a[target] = left[source];
				b[target] = right[source];
			}
	const auto minimum = -(smallWidth + 7) / 8, maximum = static_cast<int>(smallWidth * .62);
	const auto featuresA = Features(a, smallWidth, smallHeight), featuresB = Features(b, smallWidth, smallHeight);
	const auto costs = CostVolume(featuresA, featuresB, smallWidth, smallHeight, minimum, maximum, true, check);
	const auto aggregate = Aggregate(costs, smallWidth, smallHeight, maximum - minimum + 1, check);
	const auto matches = Select(aggregate, smallWidth, smallHeight, minimum, maximum, check);
	std::vector<float> good;
	for (int y = 0; y < smallHeight; ++y)
		for (int x = 0; x < smallWidth; ++x)
		{
			const auto i = std::size_t(y) * smallWidth + x;
			if (matches.uniqueness[i] > .05f &&
				std::abs(SampleRow(matches.right, smallWidth, y, x - matches.disparity[i]) - matches.disparity[i]) <= 1)
				good.push_back(matches.disparity[i]);
		}
	if (good.size() < 50)
		good = matches.disparity;
	return {static_cast<int>(std::floor(Percentile(good, .01) * scale)),
			static_cast<int>(std::ceil(Percentile(good, .995) * scale))};
}

StereoSurface MatchStereoSurface(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
	std::uint32_t width, std::uint32_t height, std::optional<float> infinity, const std::function<void()>& check)
{
	if (width < 16 || height < 16 || width > 480 || height > 480 ||
		left.size() != std::size_t(width) * height * 3 || right.size() != left.size() ||
		(infinity && (!std::isfinite(*infinity) || std::abs(*infinity) > 4)))
		throw std::invalid_argument("Stereo surface inputs exceed the analysis bounds");
	check();
	const auto [lowest, highest] = Bounds(left, right, width, height, check);
	int minimum = lowest - 10;
	if (infinity)
		minimum = std::min(minimum, static_cast<int>(*infinity * width) - 5);
	const int maximum = std::min(std::max(highest + static_cast<int>(std::nearbyint(width * .04)),
		minimum + static_cast<int>(std::nearbyint(width * .30))), minimum + static_cast<int>(std::nearbyint(width * .45)));
	const auto featuresA = Features(left, width, height), featuresB = Features(right, width, height);
	const auto costs = CostVolume(featuresA, featuresB, width, height, minimum, maximum, false, check);
	const auto aggregate = Aggregate(costs, width, height, maximum - minimum + 1, check);
	auto matches = Select(aggregate, width, height, minimum, maximum, check);
	StereoSurface result{width, height, minimum, maximum, std::move(matches.disparity),
		std::move(matches.uniqueness), std::vector<std::uint8_t>(std::size_t(width) * height)};
	for (std::uint32_t y = 0; y < height; ++y)
		for (std::uint32_t x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			const auto target = x - result.disparity[i];
			result.trusted[i] = result.uniqueness[i] > .04f && target >= 1 && target <= width - 2 &&
				std::abs(SampleRow(matches.right, width, y, target) - result.disparity[i]) <= 1.5f;
		}
	return result;
}
} // namespace simxr::app::mcp
