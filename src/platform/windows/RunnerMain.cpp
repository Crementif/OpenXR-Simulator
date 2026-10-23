#include "automation/RunnerMain.h"
#include "Pch.h"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <windows.h>

static std::string Utf8(std::wstring_view value)
{
	if (value.empty())
		return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		throw std::length_error("Runner argument exceeds the Windows encoding limit");
	const auto length = static_cast<int>(value.size());
	const auto count =
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
	if (!count)
		throw std::runtime_error("Cannot encode runner argument as UTF-8");
	std::string result(static_cast<std::size_t>(count), '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), count, nullptr,
							nullptr) != count)
		throw std::runtime_error("Cannot encode runner argument as UTF-8");
	return result;
}

int wmain(int argc, wchar_t** argv)
{
	try
	{
		std::vector<std::string> values;
		values.reserve(static_cast<std::size_t>(argc));
		for (int index = 0; index < argc; ++index)
			values.push_back(Utf8(argv[index]));
		std::vector<char*> arguments;
		arguments.reserve(values.size() + 1);
		for (auto& value : values)
			arguments.push_back(value.data());
		arguments.push_back(nullptr);
		return simxr::automation::RunRunner(argc, arguments.data());
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Runner startup: %s\n", error.what());
		return 1;
	}
}
