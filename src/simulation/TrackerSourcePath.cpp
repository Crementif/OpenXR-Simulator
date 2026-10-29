#include "simulation/TrackerSourcePath.h"
#include "Pch.h"

namespace simxr
{
TrackerSourcePath::TrackerSourcePath(std::string_view path)
{
	if (path.empty())
		return;
	const auto separator = path.find(":/");
	if (path.size() >= m_path.size() || separator == std::string_view::npos || separator == 0 ||
		separator + 2 == path.size() ||
		!std::all_of(path.begin(), path.end(), [](unsigned char value) { return value >= 33 && value <= 126; }) ||
		!std::all_of(path.begin(), path.begin() + separator, [](unsigned char value) {
			return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-';
		}))
		throw std::invalid_argument("Invalid namespaced physical tracker path");
	std::copy(path.begin(), path.end(), m_path.begin());
}

std::string_view TrackerSourcePath::View() const
{
	return {m_path.data()};
}
} // namespace simxr
