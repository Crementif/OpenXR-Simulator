#pragma once

#include <array>
#include <string_view>

namespace simxr
{
// Fixed storage keeps snapshot copies allocation-free after GPU admission.
class TrackerSourcePath
{
  public:
	TrackerSourcePath() = default;
	explicit TrackerSourcePath(std::string_view path);
	std::string_view View() const;
	bool operator==(const TrackerSourcePath&) const = default;

  private:
	std::array<char, 288> m_path{};
};
} // namespace simxr
