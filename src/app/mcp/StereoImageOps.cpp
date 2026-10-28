#include "app/mcp/StereoImageOps.h"
#include "Pch.h"

namespace simxr::app::mcp
{
std::vector<float> StereoBoxMean(const std::vector<float>& input, int width, int height, int radiusY, int radiusX)
{
	const auto paddedWidth = width + radiusX * 2;
	const auto paddedHeight = height + radiusY * 2;
	std::vector<double> integral(std::size_t(paddedWidth + 1) * (paddedHeight + 1), 0);
	for (int y = 0; y < paddedHeight; ++y)
	{
		double row = 0;
		for (int x = 0; x < paddedWidth; ++x)
		{
			row += input[std::size_t(std::clamp(y - radiusY, 0, height - 1)) * width +
						 std::clamp(x - radiusX, 0, width - 1)];
			integral[std::size_t(y + 1) * (paddedWidth + 1) + x + 1] =
				integral[std::size_t(y) * (paddedWidth + 1) + x + 1] + row;
		}
	}
	std::vector<float> result(std::size_t(width) * height);
	const auto area = double(2 * radiusY + 1) * (2 * radiusX + 1);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
		{
			const auto top = std::size_t(y) * (paddedWidth + 1);
			const auto bottom = std::size_t(y + 2 * radiusY + 1) * (paddedWidth + 1);
			const auto right = x + 2 * radiusX + 1;
			result[std::size_t(y) * width + x] = static_cast<float>(
				(integral[bottom + right] - integral[top + right] - integral[bottom + x] + integral[top + x]) / area);
		}
	return result;
}
} // namespace simxr::app::mcp
