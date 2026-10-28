#include "control/legacy/WindowsLegacyFormat.h"
#include "Pch.h"

#include <ctime>
#include <iomanip>

namespace mcp
{
std::string LocalTimestamp()
{
	const auto now = std::chrono::system_clock::now();
	const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
	const auto value = std::chrono::system_clock::to_time_t(seconds);
	std::tm calendar{};
#ifdef _WIN32
	if (localtime_s(&calendar, &value) != 0)
		return {};
#else
	if (!localtime_r(&value, &calendar))
		return {};
#endif
	std::ostringstream timestamp;
	timestamp.imbue(std::locale::classic());
	timestamp << std::put_time(&calendar, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
			  << milliseconds;
	return timestamp.str();
}
} // namespace mcp
