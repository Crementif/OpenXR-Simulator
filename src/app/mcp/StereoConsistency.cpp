#include "app/mcp/StereoConsistency.h"
#include "app/mcp/McpPch.h"
#include "app/mcp/StereoImageOps.h"
#include "app/mcp/StereoConsistencyImage.h"
#include "app/mcp/StereoSurface.h"

#include <numeric>

namespace simxr::app::mcp
{
using Json = nlohmann::json;
static constexpr float LogFloor = .03f;

bool StereoProjectionsMatch(const Json& views)
{
	try
	{
		if (!views.is_array() || views.size() != 2)
			return false;
		const auto& left = views[0].at("fov");
		const auto& right = views[1].at("fov");
		float l[4], r[4];
		for (std::size_t i = 0; i < 4; ++i)
		{
			l[i] = std::tan(left.at(i).get<float>());
			r[i] = std::tan(right.at(i).get<float>());
			if (!std::isfinite(l[i]) || !std::isfinite(r[i]))
				return false;
		}
		const auto close = [](float a, float b)
		{
			return std::abs(a - b) <= .0001f * std::max({1.0f, std::abs(a), std::abs(b)});
		};
		return l[1] > l[0] && r[1] > r[0] && l[2] > l[3] && r[2] > r[3] &&
			close(l[1] - l[0], r[1] - r[0]) && close(l[2], r[2]) && close(l[3], r[3]);
	}
	catch (const Json::exception&)
	{
		return false;
	}
}

static std::vector<float> Luminance(std::span<const std::uint8_t> image)
{
	std::vector<float> result(image.size() / 3);
	for (std::size_t i = 0; i < result.size(); ++i)
		result[i] = (image[i * 3] * .299f + image[i * 3 + 1] * .587f + image[i * 3 + 2] * .114f) / 255;
	return result;
}

static float Percentile(std::vector<float> values, double fraction)
{
	if (values.empty())
		throw std::invalid_argument("Cannot measure an empty stereo sample");
	std::sort(values.begin(), values.end());
	const auto position = fraction * (values.size() - 1);
	const auto index = static_cast<std::size_t>(position);
	const auto weight = static_cast<float>(position - index);
	return values[index] * (1 - weight) + values[std::min(index + 1, values.size() - 1)] * weight;
}

static float Sample(const std::vector<float>& data, int width, int y, float x)
{
	x = std::clamp(x, 0.0f, width - 1.001f);
	const auto column = static_cast<int>(x);
	const auto weight = x - column;
	const auto index = std::size_t(y) * width + column;
	return data[index] * (1 - weight) + data[index + 1] * weight;
}

static std::vector<float> Extreme(const std::vector<float>& input, int width, int height, int radius, bool largest)
{
	std::vector<float> intermediate(input.size()), result(input.size());
	const auto pick = [largest](float a, float b) { return largest ? std::max(a, b) : std::min(a, b); };
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			auto value = input[std::size_t(y) * width + x];
			for (int offset = -radius; offset <= radius; ++offset)
				if (y + offset >= 0 && y + offset < height)
					value = pick(value, input[std::size_t(y + offset) * width + x]);
			intermediate[std::size_t(y) * width + x] = value;
		}
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			auto value = intermediate[std::size_t(y) * width + x];
			for (int offset = -radius; offset <= radius; ++offset)
				if (x + offset >= 0 && x + offset < width)
					value = pick(value, intermediate[std::size_t(y) * width + x + offset]);
			result[std::size_t(y) * width + x] = value;
		}
	return result;
}

static std::vector<std::uint8_t> Dilate(const std::vector<std::uint8_t>& input, int width, int height, int radius)
{
	std::vector<float> mask(input.begin(), input.end());
	const auto expanded = StereoBoxMean(mask, width, height, radius, radius);
	std::vector<std::uint8_t> result(mask.size());
	for (std::size_t i = 0; i < result.size(); ++i)
		result[i] = expanded[i] > .0001f;
	return result;
}

static std::vector<float> FillSurface(const StereoSurface& matches)
{
	const int width = matches.width, height = matches.height;
	auto output = matches.disparity;
	std::vector<float> have(matches.trusted.begin(), matches.trusted.end());
	for (const auto& [radiusY, radiusX] : {std::pair{2, 12}, {5, 40}, {12, 120}, {40, 400}})
	{
		const auto weights = StereoBoxMean(have, width, height, radiusY, radiusX);
		std::vector<float> weighted(have.size());
		for (std::size_t i = 0; i < have.size(); ++i)
			weighted[i] = output[i] * have[i];
		const auto spread = StereoBoxMean(weighted, width, height, radiusY, radiusX);
		bool complete = true;
		for (std::size_t i = 0; i < have.size(); ++i)
		{
			if (have[i] < .5f && weights[i] > .02f)
			{
				output[i] = spread[i] / std::max(weights[i], 1e-6f);
				have[i] = 1;
			}
			complete &= have[i] >= .5f;
		}
		if (complete)
			break;
	}
	return output;
}

static float VerticalOffset(const std::vector<float>& left, const std::vector<float>& right,
	int width, int height, std::uint32_t sourceHeight)
{
	std::vector<float> rowsLeft(height), rowsRight(height);
	const auto start = width / 4, end = width * 3 / 4;
	for (int y = 0; y < height; ++y)
		for (int x = start; x < end; ++x)
		{
			rowsLeft[y] += left[std::size_t(y) * width + x] / (end - start);
			rowsRight[y] += right[std::size_t(y) * width + x] / (end - start);
		}
	const auto meanLeft = StereoBoxMean(rowsLeft, 1, height, 8, 0);
	const auto meanRight = StereoBoxMean(rowsRight, 1, height, 8, 0);
	for (int y = 0; y < height; ++y)
	{
		rowsLeft[y] -= meanLeft[y];
		rowsRight[y] -= meanRight[y];
	}
	float best = std::numeric_limits<float>::infinity();
	int bestOffset = 0;
	for (int offset = -8; offset <= 8; ++offset)
	{
		double error = 0;
		const auto startLeft = std::max(0, offset), startRight = std::max(0, -offset);
		const auto count = height - std::abs(offset);
		for (int y = 0; y < count; ++y)
		{
			const auto difference = rowsLeft[startLeft + y] - rowsRight[startRight + y];
			error += difference * difference;
		}
		const auto mean = static_cast<float>(error / count);
		if (mean < best - 1e-9f || (std::abs(mean - best) <= 1e-9f && std::abs(offset) < std::abs(bestOffset)))
		{
			best = mean;
			bestOffset = offset;
		}
	}
	return float(bestOffset) * sourceHeight / height;
}

static float StandardDeviation(const std::vector<float>& values)
{
	double sum = 0, squared = 0;
	for (const auto value : values)
	{
		sum += value;
		squared += value * value;
	}
	const auto mean = sum / values.size();
	return static_cast<float>(std::sqrt(std::max(0.0, squared / values.size() - mean * mean)));
}

struct StereoRegion
{
	std::string kind, nature, darkerEye;
	int x0, y0, x1, y1;
	float area, ratio, surface, shading = 0, cancel, grey;
	bool hasShading = false;
};

static std::optional<float> Distance(std::optional<StereoFrusta> frusta, float disparity)
{
	if (!frusta || disparity - frusta->infinity <= .0001f)
		return {};
	return frusta->ipd * frusta->focal / (disparity - frusta->infinity);
}

static Json RegionJson(const StereoRegion& region, int width, int height, std::optional<StereoFrusta> frusta)
{
	const auto surfaceDistance = Distance(frusta, region.surface / width);
	const auto shadingDistance = region.hasShading ? Distance(frusta, region.shading / width) : std::nullopt;
	return {{"kind", region.kind}, {"nature", region.nature}, {"darker_eye", region.darkerEye},
		{"box", {float(region.x0) / width, float(region.y0) / height, float(region.x1) / width, float(region.y1) / height}},
		{"area_pct", region.area}, {"mean_ratio_pct", region.ratio}, {"surface_disparity_pct", 100 * region.surface / width},
		{"shading_disparity_pct", region.hasShading ? Json(100 * region.shading / width) : Json(nullptr)},
		{"cancel_fraction", region.cancel}, {"grey", region.grey},
		{"surface_metres", surfaceDistance ? Json(*surfaceDistance) : Json(nullptr)},
		{"shading_metres", shadingDistance ? Json(*shadingDistance) : Json(nullptr)}};
}

StereoConsistencyReport AnalyzeStereoConsistency(std::span<const std::uint8_t> left,
	std::span<const std::uint8_t> right, std::uint32_t inputWidth, std::uint32_t inputHeight,
	std::uint32_t sourceHeight, float threshold, std::optional<StereoFrusta> frusta,
	bool includeImage, const std::function<void()>& check)
{
	if (inputWidth < 16 || inputHeight < 16 || inputWidth > 480 || inputHeight > 480 || sourceHeight < inputHeight ||
		left.size() != std::size_t(inputWidth) * inputHeight * 3 || right.size() != left.size() ||
		!std::isfinite(threshold) || threshold <= 0 ||
		(frusta && (!std::isfinite(frusta->infinity) || std::abs(frusta->infinity) > 4 ||
			!std::isfinite(frusta->focal) || frusta->focal <= 0 || !std::isfinite(frusta->ipd) || frusta->ipd < 0)))
		throw std::invalid_argument("Invalid stereo consistency inputs");
	const int width = inputWidth, height = inputHeight;
	const auto count = std::size_t(width) * height;
	auto leftY = Luminance(left), rightY = Luminance(right);
	StereoConsistencyReport result;
	result.document = {{"verdict", "INCONCLUSIVE_NO_FEATURES"}, {"score", 0}, {"highlight_score", 0},
		{"valid_pct", 0}, {"median_disparity_pct", 0}, {"max_disparity_pct", 0}, {"behind_infinity_pct", 0},
		{"vertical_offset_px", 0}, {"exposure_offset_pct", 0}, {"far_disparity_pct", 0},
		{"infinity_disparity_pct", frusta ? Json(100 * frusta->infinity) : Json(nullptr)},
		{"regions", Json::array()}, {"notes", Json::array()}};
	if (std::max(StandardDeviation(leftY), StandardDeviation(rightY)) < .012f)
	{
		result.document["notes"].push_back("Both eyes are flat colour; nothing to match.");
		return result;
	}
	const auto vertical = VerticalOffset(leftY, rightY, width, height, sourceHeight);
	auto matches = MatchStereoSurface(left, right, width, height,
		frusta ? std::optional<float>(frusta->infinity) : std::nullopt, check);
	std::vector<float> matched;
	for (std::size_t i = 0; i < count; ++i)
		if (matches.trusted[i])
			matched.push_back(matches.disparity[i]);
	if (matched.size() < count * .03)
	{
		result.document["verdict"] = "INCONCLUSIVE_NO_MATCH";
		result.document["valid_pct"] = 100.0 * matched.size() / count;
		result.document["vertical_offset_px"] = vertical;
		result.document["notes"].push_back("Under 3% of the image could be matched between the eyes.");
		return result;
	}
	check();
	const auto surface = FillSurface(matches);
	std::vector<float> nearest(count, -std::numeric_limits<float>::infinity());
	std::vector<std::size_t> targets(count);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			const auto column = std::clamp(static_cast<int>(std::nearbyint(x - surface[i])), 0, width - 1);
			targets[i] = std::size_t(y) * width + column;
			nearest[targets[i]] = std::max(nearest[targets[i]], surface[i]);
		}
	std::vector<std::uint8_t> occluded(count), excluded(count);
	std::vector<float> occlusionFloat(count), rowNear(height, -std::numeric_limits<float>::infinity());
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			occluded[i] = surface[i] < nearest[targets[i]] - 2.5f;
			occlusionFloat[i] = occluded[i];
			if (matches.trusted[i])
				rowNear[y] = std::max(rowNear[y], matches.disparity[i]);
		}
	const auto occlusionMean = StereoBoxMean(occlusionFloat, width, height, 2, 2);
	for (std::size_t i = 0; i < count; ++i)
		occluded[i] &= occlusionMean[i] > .35f;
	const auto grown = Dilate(occluded, width, height, 1);
	const auto median = Percentile(matched, .5);
	for (auto& value : rowNear)
		if (!std::isfinite(value))
			value = median;
	rowNear = Extreme(rowNear, 1, height, 12, true);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			const auto target = x - surface[i];
			excluded[i] = grown[i] || target < 1 || target > width - 2 ||
				(!matches.trusted[i] && x < rowNear[y] + 2);
		}
	const auto blurLeft = StereoBoxMean(leftY, width, height, 2, 2);
	const auto blurRight = StereoBoxMean(rightY, width, height, 2, 2);
	std::vector<float> logLeft(count), logWarped(count), residual(count), warpedY(count);
	std::vector<float> exposureSamples;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			logLeft[i] = std::log(blurLeft[i] + LogFloor);
			logWarped[i] = std::log(Sample(blurRight, width, y, x - surface[i]) + LogFloor);
			warpedY[i] = Sample(rightY, width, y, x - surface[i]);
			if (!excluded[i])
				exposureSamples.push_back(logLeft[i] - logWarped[i]);
		}
	const auto exposure = exposureSamples.empty() ? 0 : Percentile(exposureSamples, .5);
	const auto highSurface = Extreme(surface, width, height, 4, true);
	const auto lowSurface = Extreme(surface, width, height, 4, false);
	std::fill(residual.begin(), residual.end(), std::numeric_limits<float>::infinity());
	for (const auto* candidate : {&surface, &highSurface, &lowSurface})
	{
		check();
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const auto i = std::size_t(y) * width + x;
				logWarped[i] = std::log(Sample(blurRight, width, y, x - (*candidate)[i]) + LogFloor);
			}
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -2; dx <= 2; ++dx)
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						const auto i = std::size_t(y) * width + x;
						const auto shiftedX = x - dx;
						const auto shiftedY = (y - dy + height) % height;
						const auto shifted = shiftedX < 0 || shiftedX >= width ? 0 :
							logWarped[std::size_t(shiftedY) * width + shiftedX];
						const auto error = logLeft[i] - shifted - exposure;
						if (std::abs(error) < std::abs(residual[i]))
							residual[i] = error;
					}
	}
	std::vector<std::uint8_t> valid(count), mask(count);
	std::vector<float> rawMask(count);
	std::size_t validCount = 0;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			valid[i] = !excluded[i] && x >= 6 && y >= 6 && x < width - 6 && y < height - 6;
			validCount += valid[i];
			if (!valid[i])
				residual[i] = 0;
			rawMask[i] = std::abs(residual[i]) > threshold;
		}
	const auto support = StereoBoxMean(rawMask, width, height, 2, 2);
	for (std::size_t i = 0; i < count; ++i)
		mask[i] = rawMask[i] && support[i] > .45f;

	std::vector<int> labels(count, 0);
	std::vector<StereoRegion> regions;
	const auto warpedBlur = StereoBoxMean(warpedY, width, height, 2, 2);
	const auto far = Percentile(matched, .02);
	const auto infinity = frusta ? frusta->infinity * width : far;
	int label = 0;
	std::uint64_t regionWork = 0;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto start = std::size_t(y) * width + x;
			if (!mask[start] || labels[start])
				continue;
			check();
			++label;
			std::vector<std::size_t> pixels{start};
			labels[start] = label;
			int x0 = x, x1 = x + 1, y0 = y, y1 = y + 1;
			for (std::size_t current = 0; current < pixels.size(); ++current)
			{
				if (current % 4096 == 0)
					check();
				const auto px = static_cast<int>(pixels[current] % width), py = static_cast<int>(pixels[current] / width);
				x0 = std::min(x0, px); x1 = std::max(x1, px + 1);
				y0 = std::min(y0, py); y1 = std::max(y1, py + 1);
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const auto nx = px + dx, ny = py + dy;
						if (nx < 0 || ny < 0 || nx >= width || ny >= height)
							continue;
						const auto i = std::size_t(ny) * width + nx;
						if (mask[i] && !labels[i])
						{
							labels[i] = label;
							pixels.push_back(i);
						}
					}
			}
			if (pixels.size() < .0004 * count)
				continue;
			double meanResidual = 0, greyDifference = 0, meanLeft = 0;
			std::vector<float> surfaceValues;
			for (const auto i : pixels)
			{
				meanResidual += residual[i];
				meanLeft += logLeft[i];
				surfaceValues.push_back(surface[i]);
				const auto px = static_cast<int>(i % width), py = static_cast<int>(i / width);
				const auto target = std::clamp(px - surface[i], 0.0f, width - 1.001f);
				const auto column = static_cast<int>(target);
				const auto fraction = target - column;
				float sumLeft = .06f, sumRight = .06f, channelsRight[3];
				for (int channel = 0; channel < 3; ++channel)
				{
					const auto j = (std::size_t(py) * width + column) * 3 + channel;
					channelsRight[channel] = (right[j] * (1 - fraction) + right[j + 3] * fraction) / 255;
					sumRight += channelsRight[channel];
					sumLeft += left[i * 3 + channel] / 255.0f;
				}
				for (int channel = 0; channel < 3; ++channel)
					greyDifference += std::abs(left[i * 3 + channel] / 255.0f / sumLeft - channelsRight[channel] / sumRight);
			}
			meanResidual /= pixels.size(); meanLeft /= pixels.size();
			const auto surfaceMedian = Percentile(surfaceValues, .5);
			const auto mx0 = std::max(0, x0 - 6), mx1 = std::min(width, x1 + 6);
			const auto my0 = std::max(0, y0 - 3), my1 = std::min(height, y1 + 3);
			regionWork += std::uint64_t(mx1 - mx0) * (my1 - my0) *
				(matches.maximumDisparity - matches.minimumDisparity + 1);
			if (regions.size() >= 128 || regionWork > 80'000'000)
				throw std::runtime_error("Stereo consistency exceeds the bounded region analysis budget");
			double base = 0;
			for (int py = my0; py < my1; ++py)
				for (int px = mx0; px < mx1; ++px)
				{
					const auto i = std::size_t(py) * width + px;
					const auto difference = logLeft[i] - std::log(warpedBlur[i] + LogFloor) - exposure;
					base += difference * difference;
				}
			double best = base;
			int bestShift = 0;
			bool foundShift = false;
			for (int d = matches.minimumDisparity; d <= matches.maximumDisparity; ++d)
			{
				check();
				if (mx0 - d < 0 || mx1 - d > width)
					continue;
				double error = 0;
				for (int py = my0; py < my1; ++py)
					for (int px = mx0; px < mx1; ++px)
					{
						const auto difference = logLeft[std::size_t(py) * width + px] -
							std::log(blurRight[std::size_t(py) * width + px - d] + LogFloor) - exposure;
						error += difference * difference;
					}
				if (error < best)
				{
					best = error; bestShift = d; foundShift = true;
				}
			}
			const auto cancel = base <= 1e-9 ? 0.f : static_cast<float>(1 - best / base);
			const bool shifted = foundShift && cancel > .75f && std::abs(bestShift - surfaceMedian) > 2.5f;
			std::string kind = "ONE_EYE_ONLY";
			if (shifted)
				kind = std::abs(bestShift - infinity) <= 1.5f && surfaceMedian - infinity > 4 ? "SCREEN_LOCKED" : "WRONG_DEPTH";
			const int rx0 = std::max(0, x0 - 8), rx1 = std::min(width, x1 + 8);
			const int ry0 = std::max(0, y0 - 8), ry1 = std::min(height, y1 + 8);
			const int rw = rx1 - rx0, rh = ry1 - ry0;
			std::vector<std::uint8_t> inside(std::size_t(rw) * rh);
			for (int py = ry0; py < ry1; ++py)
				for (int px = rx0; px < rx1; ++px)
					inside[std::size_t(py - ry0) * rw + px - rx0] = labels[std::size_t(py) * width + px] == label;
			const auto ringOuter = Dilate(inside, rw, rh, 6), ringInner = Dilate(inside, rw, rh, 1);
			std::vector<float> around;
			for (int py = ry0; py < ry1; ++py)
				for (int px = rx0; px < rx1; ++px)
				{
					const auto i = std::size_t(py) * width + px, r = std::size_t(py - ry0) * rw + px - rx0;
					if (ringOuter[r] && !ringInner[r] && valid[i] && !mask[i])
						around.push_back(logLeft[i]);
				}
			std::string nature = "shadow";
			if (around.size() >= 12)
			{
				const auto surrounding = Percentile(around, .5);
				const auto meanRight = meanLeft - meanResidual;
				const auto odd = std::abs(meanLeft - surrounding) >= std::abs(meanRight - surrounding) ? meanLeft : meanRight;
				if (odd > surrounding)
					nature = "highlight";
			}
			regions.push_back({kind, nature, meanResidual < 0 ? "left" : "right", x0, y0, x1, y1,
				static_cast<float>(100.0 * pixels.size() / count), static_cast<float>(100 * (1 - std::exp(-std::abs(meanResidual)))),
				surfaceMedian, static_cast<float>(bestShift), std::max(0.f, cancel),
				static_cast<float>(1 - std::min(1.0, greyDifference / pixels.size() / .12)), shifted});
		}
	std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) { return a.area * a.ratio > b.area * b.ratio; });
	const auto validFraction = double(validCount) / count;
	double shadowWeight = 0, highlightWeight = 0;
	bool largeShadow = false, shiftedShadow = false, largeRegion = false;
	for (const auto& region : regions)
	{
		const auto weight = region.area * -std::log(std::max(1e-6f, 1 - region.ratio / 100)) / threshold;
		if (region.nature == "shadow")
		{
			shadowWeight += weight;
			largeShadow |= region.area >= .5f;
			shiftedShadow |= region.hasShading && region.area >= .1f;
		}
		else
			highlightWeight += weight;
		largeRegion |= region.area >= .5f;
	}
	const auto score = shadowWeight / std::max(validFraction, .001);
	const auto highlightScore = highlightWeight / std::max(validFraction, .001);
	std::string verdict = validFraction < .2 ? "INCONCLUSIVE_LOW_COVERAGE" :
		(score >= 1 || largeShadow || shiftedShadow) ? "FAIL_RIVALRY" :
		(highlightScore >= 1 || largeRegion) ? "WARN_HIGHLIGHT_RIVALRY" :
		!regions.empty() ? "WARN_MINOR_RIVALRY" : "PASS";
	const auto behind = frusta ? std::count_if(matched.begin(), matched.end(),
		[&](float value) { return value < infinity - 2; }) / double(matched.size()) : 0;
	Json notes = Json::array();
	if (frusta && far < infinity - .02f * width)
		notes.push_back("Farthest content is behind the declared frusta's infinity; check the capture crop and projection offset.");
	if (behind > .05)
		notes.push_back("More than 5% of matched pixels lie behind the declared infinity; check eye order and separation.");
	if (!frusta && far < -2)
		notes.push_back("Some disparity is negative; asymmetric frusta are needed to determine whether it is behind infinity.");
	if (std::abs(vertical) >= 2)
		notes.push_back("The eyes have a vertical offset; parallel frusta should give zero.");
	if (std::abs(exposure) > .05)
		notes.push_back("The eyes have different global brightness.");
	if (Percentile(matched, .98) - far < 1 && (!frusta || std::abs(far - infinity) < 1.5f))
		notes.push_back("Every matched pixel has the same disparity: both eyes may show the same image (no stereo).");
	if (validFraction < .2)
		notes.push_back("Less than 20% of the eye could be compared.");
	Json regionValues = Json::array();
	for (std::size_t i = 0; i < std::min<std::size_t>(12, regions.size()); ++i)
		regionValues.push_back(RegionJson(regions[i], width, height, frusta));
	result.document = {{"verdict", verdict}, {"score", score}, {"highlight_score", highlightScore},
		{"valid_pct", validFraction * 100}, {"median_disparity_pct", 100 * median / width},
		{"max_disparity_pct", 100 * Percentile(matched, .99) / width}, {"behind_infinity_pct", behind * 100},
		{"vertical_offset_px", vertical}, {"exposure_offset_pct", 100 * (std::exp(std::abs(exposure)) - 1)},
		{"far_disparity_pct", 100 * far / width},
		{"infinity_disparity_pct", frusta ? Json(100 * frusta->infinity) : Json(nullptr)},
		{"regions", std::move(regionValues)}, {"notes", std::move(notes)}};
	if (includeImage)
	{
		check();
		result.sheetWidth = width * 2 + 4;
		result.sheetHeight = height * 2 + 24;
		result.sheet = StereoConsistencyImage(left, right, leftY, residual, valid, surface,
			result.document.at("regions"), width, height, threshold, matches.maximumDisparity);
		result.document["sheet"] = {{"top_left", "left eye"}, {"top_right", "right eye"},
			{"bottom_left", "residual: red left darker, blue right darker, green excluded"},
			{"bottom_right", "surface disparity: blue far, yellow near"},
			{"boxes", "red one eye only, amber wrong depth, magenta screen locked, pale blue highlight"}};
	}
	return result;
}
} // namespace simxr::app::mcp
