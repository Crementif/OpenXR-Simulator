#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace mcp
{
std::uint64_t WindowsLegacyProcessId();

class WindowsLegacyOutput
{
  public:
	explicit WindowsLegacyOutput(std::filesystem::path destination);
	~WindowsLegacyOutput();
	WindowsLegacyOutput(const WindowsLegacyOutput&) = delete;
	WindowsLegacyOutput& operator=(const WindowsLegacyOutput&) = delete;
	void Write(std::span<const std::byte> bytes);
	void Commit();

  private:
	void Abort() noexcept;
	std::filesystem::path m_destination;
	std::filesystem::path m_temporary;
	std::ofstream m_stream;
	bool m_committed = false;
};
} // namespace mcp
