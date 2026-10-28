#include "Pch.h"
#include "app/mcp/StereoSurface.h"
#include "../TestSupport.h"

using simxr::app::mcp::MatchStereoSurface;

static float Median(std::vector<float> values)
{
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

int main(int argc, char** argv)
{
	const std::uint32_t width = argc > 1 ? 480 : 128;
	const std::uint32_t height = argc > 1 ? 300 : 96;
	const auto count = std::size_t(width) * height * 3;
	std::vector<std::uint8_t> left(count), right(count);
	if (argc > 1)
	{
		std::ifstream input(argv[1], std::ios::binary);
		Require(bool(input), "Open stereo surface fixture");
		input.read(reinterpret_cast<char*>(left.data()), left.size());
		input.read(reinterpret_cast<char*>(right.data()), right.size());
		Require(bool(input) && input.peek() == std::char_traits<char>::eof(), "Read two exact RGB images");
	}
	else
	{
		std::uint32_t random = 1;
		for (auto& value : left)
		{
			random = random * 1664525 + 1013904223;
			value = static_cast<std::uint8_t>(random >> 24);
		}
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
				for (std::uint32_t channel = 0; channel < 3; ++channel)
					right[(std::size_t(y) * width + x) * 3 + channel] =
						left[(std::size_t(y) * width + std::min(width - 1, x + 12)) * 3 + channel];
	}
	const auto result = MatchStereoSurface(left, right, width, height, argc > 1 ? std::optional<float>(.0625f) : std::nullopt,
		[] {});
	std::vector<float> matched;
	for (std::size_t i = 0; i < result.trusted.size(); ++i)
		if (result.trusted[i])
			matched.push_back(result.disparity[i]);
	Require(matched.size() > result.trusted.size() / 5, "Textured pair has reliable bidirectional matches");
	const auto expected = argc > 1 ? 35.f : 12.f;
	Require(std::abs(Median(matched) - expected) < (argc > 1 ? 8.f : 1.f), "Recover known surface disparity");
	bool interrupted = false;
	try { (void)MatchStereoSurface(left, right, width, height, {}, [] { throw std::runtime_error("cancelled"); }); }
	catch (const std::runtime_error&) { interrupted = true; }
	Require(interrupted, "Stereo surface matching checks cancellation");
	return 0;
}
