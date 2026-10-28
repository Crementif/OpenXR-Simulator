#include "Pch.h"
#include "app/mcp/StereoConsistency.h"
#include "../TestSupport.h"

#include <iostream>

int main(int argc, char** argv)
{
	using namespace simxr::app::mcp;
	using Json = nlohmann::json;
	Json views = Json::array({{{"fov", {-0.8, 0.7, 0.8, -0.8}}},
		{{"fov", {-0.8, 0.7, 0.8, -0.8}}}});
	Require(StereoProjectionsMatch(views), "Matching projections are supported");
	views[1]["fov"][1] = 0.6;
	Require(!StereoProjectionsMatch(views), "Unequal horizontal scales are unsupported");
	views[1]["fov"][1] = 0.7;
	views[1]["fov"][2] = 0.7;
	Require(!StereoProjectionsMatch(views), "Unequal vertical projections are unsupported");
	Require(!StereoProjectionsMatch(Json::array()), "Missing eye views are unsupported");
	const std::uint32_t width = argc > 1 ? 480 : 64, height = argc > 1 ? 300 : 48;
	std::vector<std::uint8_t> left(std::size_t(width) * height * 3, 128), right(left);
	if (argc > 1)
	{
		std::ifstream input(argv[1], std::ios::binary);
		Require(bool(input), "Open stereo consistency fixture");
		input.read(reinterpret_cast<char*>(left.data()), static_cast<std::streamsize>(left.size()));
		input.read(reinterpret_cast<char*>(right.data()), static_cast<std::streamsize>(right.size()));
		Require(bool(input) && input.peek() == std::char_traits<char>::eof(), "Read two exact RGB images");
	}
	if (argc > 5 && std::string_view(argv[5]) == "identical")
		right = left;
	const auto frusta = argc > 1 && !(argc > 5 && std::string_view(argv[5]) == "identical")
		? std::optional<StereoFrusta>{{argc > 5 && std::string_view(argv[5]) == "mismatch" ? .15625f : .0625f, .45f, .064f}}
		: std::nullopt;
	const auto result = AnalyzeStereoConsistency(left, right, width, height, height, .16f, frusta, argc > 1, [] {});
	std::cout << result.document.dump() << '\n';
	if (argc > 2)
		Require(result.document.at("verdict").get<std::string>().starts_with(argv[2]), "Expected synthetic verdict");
	else if (argc == 1)
		Require(result.document.at("verdict") == "INCONCLUSIVE_NO_FEATURES", "Flat eyes cannot be analyzed");
	if (argc > 3 && argv[3][0])
		Require(!result.document.at("regions").empty() && result.document.at("regions")[0].at("kind") == argv[3],
				"Expected synthetic region classification");
	if (argc > 1)
	{
		Require(result.sheetWidth == 2 * width + 4 && result.sheetHeight == 2 * height + 24 &&
				result.sheet.size() == std::size_t(result.sheetWidth) * result.sheetHeight * 4,
				"Stereo consistency produces a bounded four-panel sheet");
		if (argc > 4)
		{
			std::ofstream output(argv[4], std::ios::binary);
			output.write(reinterpret_cast<const char*>(result.sheet.data()), static_cast<std::streamsize>(result.sheet.size()));
			Require(bool(output), "Write stereo sheet fixture");
		}
	}
	return 0;
}
