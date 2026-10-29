#include "app/mcp/McpPch.h"
#include "app/mcp/McpStereoAnalysis.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;

Json AnalyzeStereo(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right, std::uint32_t width,
				   std::uint32_t height, std::uint32_t sourceWidth, const std::function<void()>& check)
{
	if (width < 8 || height < 8 || width > 640 || height > 640 || sourceWidth < width ||
		left.size() != std::size_t{width} * height || right.size() != left.size())
		throw std::invalid_argument("Stereo analysis requires two matching bounded eye images");
	const auto x0 = width / 4, y0 = height / 4;
	const auto windowWidth = width / 2, windowHeight = height / 2;
	const auto count = double(windowWidth) * windowHeight;
	double sum = 0, square = 0;
	for (std::uint32_t y = y0; y < y0 + windowHeight; ++y)
	{
		check();
		for (std::uint32_t x = x0; x < x0 + windowWidth; ++x)
		{
			const auto value = left[std::size_t{y} * width + x];
			sum += value;
			square += double(value) * value;
		}
	}
	const auto contrast = std::sqrt(std::max(0.0, square / count - (sum / count) * (sum / count)));
	Json result = {
		{"contrast", contrast},
		{"analysis_width", width},
		{"analysis_height", height},
		{"method", "central_window_horizontal_disparity"},
		{"limitation",
		 "Scene depth, occlusion and texture affect this heuristic; it does not verify IPD or stereo consistency."}};
	if (contrast < 2.0)
	{
		result["verdict"] = "INCONCLUSIVE_NO_FEATURES";
		result["diagnosis"] = "The center of the left eye has too little detail to measure disparity.";
		return result;
	}
	const auto maxShift = std::min<int>(60, width / 6);
	std::vector<std::pair<int, double>> scores;
	scores.reserve(2 * maxShift + 1);
	int bestShift = 0;
	double bestScore = std::numeric_limits<double>::infinity();
	for (int distance = 0; distance <= maxShift; ++distance)
	{
		for (int sign : {-1, 1})
		{
			if (!distance && sign == 1)
				continue;
			check();
			const int shift = distance * sign;
			if (int(x0) + shift < 0 || int(x0 + windowWidth) + shift > int(width))
				continue;
			double difference = 0;
			for (std::uint32_t y = y0; y < y0 + windowHeight; ++y)
				for (std::uint32_t x = x0; x < x0 + windowWidth; ++x)
				{
					const auto delta = int(left[std::size_t{y} * width + x]) -
									   int(right[std::size_t{y} * width + std::size_t(int(x) + shift)]);
					difference += delta * delta;
				}
			const auto score = difference / count;
			scores.emplace_back(shift, score);
			if (score < bestScore)
			{
				bestScore = score;
				bestShift = shift;
			}
		}
	}
	const auto disparity = static_cast<int>(std::lround(double(bestShift) * sourceWidth / width));
	const auto minimum = std::max(2u, sourceWidth / 640);
	const auto maximum = std::max(30u, sourceWidth / 40);
	result["horizontal_disparity_px"] = disparity;
	result["expected_range_px"] = Json::array({minimum, maximum});
	result["mean_squared_error"] = bestScore;
	if (bestScore > contrast * contrast * 0.8)
	{
		result["verdict"] = "INCONCLUSIVE_NO_MATCH";
		result["diagnosis"] = "The central eye images did not match reliably.";
		return result;
	}
	for (const auto& [shift, score] : scores)
	{
		if (std::abs(shift - bestShift) > 2 && score <= bestScore + contrast * contrast * 0.005)
		{
			result["verdict"] = "INCONCLUSIVE_AMBIGUOUS";
			result["diagnosis"] = "Repeated features admit multiple disparity matches.";
			return result;
		}
	}
	const auto magnitude = std::abs(disparity);
	result["verdict"] = magnitude < int(minimum)   ? "FAIL_NO_PARALLAX"
						: magnitude > int(maximum) ? "FAIL_EXCESSIVE_PARALLAX"
												   : "PASS";
	result["diagnosis"] = "The measured disparity is compared with a scene-dependent heuristic range.";
	return result;
}
} // namespace simxr::app::mcp
