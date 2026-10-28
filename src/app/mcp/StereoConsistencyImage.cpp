#include "app/mcp/StereoConsistencyImage.h"
#include "Pch.h"

#include <cmath>

namespace simxr::app::mcp
{
using Json = nlohmann::json;
using Color = std::array<std::uint8_t, 3>;

static void Pixel(std::vector<std::uint8_t>& image, int width, int height, int x, int y, Color color)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
		return;
	const auto index = (std::size_t(y) * width + x) * 4;
	for (int channel = 0; channel < 3; ++channel)
		image[index + channel] = color[channel];
	image[index + 3] = 255;
}

static void Rectangle(std::vector<std::uint8_t>& image, int width, int height, int x0, int y0, int x1, int y1, Color color)
{
	for (int x = x0; x <= x1; ++x)
	{
		Pixel(image, width, height, x, y0, color);
		Pixel(image, width, height, x, y1, color);
	}
	for (int y = y0; y <= y1; ++y)
	{
		Pixel(image, width, height, x0, y, color);
		Pixel(image, width, height, x1, y, color);
	}
}

static std::array<unsigned, 5> Glyph(char character)
{
	switch (character)
	{
	case 'A': return {2, 5, 7, 5, 5}; case 'B': return {6, 5, 6, 5, 6};
	case 'D': return {6, 5, 5, 5, 6}; case 'E': return {7, 4, 6, 4, 7};
	case 'F': return {7, 4, 6, 4, 4}; case 'G': return {3, 4, 5, 5, 3};
	case 'H': return {5, 5, 7, 5, 5}; case 'I': return {7, 2, 2, 2, 7};
	case 'L': return {4, 4, 4, 4, 7}; case 'P': return {6, 5, 6, 4, 4};
	case 'R': return {6, 5, 6, 5, 5}; case 'S': return {3, 4, 2, 1, 6};
	case 'T': return {7, 2, 2, 2, 2}; case 'U': return {5, 5, 5, 5, 7};
	case 'Y': return {5, 5, 2, 2, 2}; case '0': return {7, 5, 5, 5, 7};
	case '1': return {2, 6, 2, 2, 7}; case '2': return {7, 1, 7, 4, 7};
	case '3': return {7, 1, 7, 1, 7}; case '4': return {5, 5, 7, 1, 1};
	case '5': return {7, 4, 7, 1, 7}; case '6': return {7, 4, 7, 5, 7};
	case '7': return {7, 1, 1, 1, 1}; case '8': return {7, 5, 7, 5, 7};
	case '9': return {7, 5, 7, 1, 7}; default: return {};
	}
}

static void Text(std::vector<std::uint8_t>& image, int width, int height, int x, int y,
	std::string_view text, Color color, int scale = 1)
{
	for (const auto character : text)
	{
		const auto glyph = Glyph(character);
		for (int row = 0; row < 5; ++row)
			for (int column = 0; column < 3; ++column)
				if (glyph[row] & (1u << (2 - column)))
					for (int dy = 0; dy < scale; ++dy)
						for (int dx = 0; dx < scale; ++dx)
							Pixel(image, width, height, x + column * scale + dx, y + row * scale + dy, color);
		x += 4 * scale;
	}
}

std::vector<std::uint8_t> StereoConsistencyImage(std::span<const std::uint8_t> left,
	std::span<const std::uint8_t> right, const std::vector<float>& luminance, const std::vector<float>& residual,
	const std::vector<std::uint8_t>& valid, const std::vector<float>& surface, const Json& regions,
	unsigned width, unsigned height, float threshold, int maximumDisparity)
{
	const int sheetWidth = 2 * width + 4, sheetHeight = 2 * height + 24;
	std::vector<std::uint8_t> sheet(std::size_t(sheetWidth) * sheetHeight * 4, 0);
	for (std::uint32_t y = 0; y < height; ++y)
		for (std::uint32_t x = 0; x < width; ++x)
		{
			const auto i = std::size_t(y) * width + x;
			Pixel(sheet, sheetWidth, sheetHeight, x, y + 10, {left[i * 3], left[i * 3 + 1], left[i * 3 + 2]});
			Pixel(sheet, sheetWidth, sheetHeight, x + width + 4, y + 10, {right[i * 3], right[i * 3 + 1], right[i * 3 + 2]});
			const auto grey = luminance[i] * .45f;
			const auto strength = std::clamp(std::abs(residual[i]) / (threshold * 2.5f), 0.f, 1.f);
			const std::array<float, 3> tint = residual[i] < 0 ? std::array{1.f, .15f, .1f} : std::array{.1f, .45f, 1.f};
			Color heat{};
			for (int c = 0; c < 3; ++c)
			{
				const auto value = valid[i] ? grey * (1 - strength) + tint[c] * strength :
					grey * .35f + (c == 1 ? .12f : 0);
				heat[c] = static_cast<std::uint8_t>(255 * std::clamp(value, 0.f, 1.f));
			}
			Pixel(sheet, sheetWidth, sheetHeight, x, y + height + 24, heat);
			const auto disparity = std::clamp(surface[i] / std::max(1, maximumDisparity), 0.f, 1.f);
			const auto fade = valid[i] ? 1.f : .45f;
			Pixel(sheet, sheetWidth, sheetHeight, x + width + 4, y + height + 24,
				{static_cast<std::uint8_t>(255 * disparity * fade),
				 static_cast<std::uint8_t>(255 * std::sqrt(disparity) * .8f * fade),
				 static_cast<std::uint8_t>(255 * (1 - disparity) * fade)});
		}
	const Color white{230, 230, 230};
	const int scale = width >= 80 ? 2 : 1;
	Text(sheet, sheetWidth, sheetHeight, 2, 0, width >= 80 ? "LEFT" : "L", white, scale);
	Text(sheet, sheetWidth, sheetHeight, width + 6, 0, width >= 80 ? "RIGHT" : "R", white, scale);
	Text(sheet, sheetWidth, sheetHeight, 2, height + 14, width >= 80 ? "RESIDUAL" : "R", white, scale);
	Text(sheet, sheetWidth, sheetHeight, width + 6, height + 14, width >= 80 ? "DISPARITY" : "D", white, scale);
	for (std::size_t index = 0; index < regions.size(); ++index)
	{
		const auto& region = regions[index];
		const auto& box = region.at("box");
		const auto x0 = static_cast<int>(box[0].get<float>() * width), x1 = static_cast<int>(box[2].get<float>() * width);
		const auto y0 = static_cast<int>(box[1].get<float>() * height), y1 = static_cast<int>(box[3].get<float>() * height);
		const auto shift = static_cast<int>(std::round(region.at("surface_disparity_pct").get<float>() * width / 100));
		const auto kind = region.at("kind").get<std::string>();
		const Color color = region.at("nature") == "highlight" ? Color{120, 210, 255} :
			kind == "SCREEN_LOCKED" ? Color{255, 80, 255} : kind == "WRONG_DEPTH" ? Color{255, 190, 40} : Color{255, 60, 60};
		for (const auto [offsetX, offsetY] : {std::pair{0, 10}, {int(width) + 4 - shift, 10}, {0, int(height) + 24}})
		{
			Rectangle(sheet, sheetWidth, sheetHeight, x0 + offsetX, y0 + offsetY, x1 + offsetX, y1 + offsetY, color);
			Text(sheet, sheetWidth, sheetHeight, x0 + offsetX + 2, y0 + offsetY + 2, std::to_string(index + 1), color);
		}
	}
	return sheet;
}
} // namespace simxr::app::mcp
