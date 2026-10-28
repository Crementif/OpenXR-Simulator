#include "Pch.h"
#include "app/platform/StartupFailure.h"
#include "../TestSupport.h"

int main()
{
	using simxr::app::FormatStartupFailure;
	Require(FormatStartupFailure("First line\n\nSecond line") == "First line\n\nSecond line",
			"Existing paragraphs are preserved");
	const auto wrapped = FormatStartupFailure(std::string(80, 'x'));
	Require(wrapped == std::string(48, 'x') + "\n" + std::string(32, 'x'),
			"Long paths wrap without dropping characters");
	const std::string unicode = std::string(47, 'a') + "\xe6\x97\xa5" + std::string(12, 'b');
	const auto formatted = FormatStartupFailure(unicode);
	Require(formatted == std::string(47, 'a') + "\n" + "\xe6\x97\xa5" + std::string(12, 'b'),
			"Wrapping does not split a UTF-8 character");
	const auto invalid = FormatStartupFailure(std::string(80, '\x80'));
	Require(invalid.size() == 81 && invalid[48] == '\n', "Malformed input still makes progress");
	return 0;
}
