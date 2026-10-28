#include "headset/RuntimeSelection.h"
#include "Pch.h"
#include "json.h"

#include <fstream>
#include <stdexcept>

namespace simxr::headset
{
static std::string LowerAscii(std::string_view text)
{
	std::string result(text);
	for (auto& value : result)
		if (value >= 'A' && value <= 'Z')
			value += 'a' - 'A';
	return result;
}

bool IsSimulatorRuntime(std::string_view name)
{
	const auto lower = LowerAscii(name);
	return lower.find("openxr simulator") != std::string::npos || lower.find("openxr_simulator") != std::string::npos ||
		   lower.find("openxr-simulator") != std::string::npos;
}

std::filesystem::path ValidateRuntimeManifest(const std::filesystem::path& manifest)
{
	if (manifest.empty())
		return {};
	const auto path = std::filesystem::canonical(manifest);
	const auto fileName = path.filename().u8string();
	if (IsSimulatorRuntime({reinterpret_cast<const char*>(fileName.data()), fileName.size()}))
		throw std::invalid_argument("Select a physical OpenXR runtime, not the simulator");
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open the selected runtime manifest");
	std::string text(65537, '\0');
	input.read(text.data(), static_cast<std::streamsize>(text.size()));
	const auto count = input.gcount();
	if (input.bad() || !input.eof() || count > 65536)
		throw std::runtime_error("Cannot read runtime manifest or manifest exceeds 64 KiB");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close runtime manifest");
	text.resize(static_cast<std::size_t>(count));
	if (text.find('\0') != std::string::npos)
		throw std::invalid_argument("Runtime manifest contains a null byte");
	const json::Object document(text.c_str());
	const auto library = document.Child("runtime").String("library_path");
	if (library.empty())
		throw std::invalid_argument("Runtime manifest has no runtime library_path");
	const auto libraryPath = std::filesystem::path(std::u8string(library.begin(), library.end()));
	std::error_code error;
	const auto resolved = std::filesystem::weakly_canonical(path.parent_path() / libraryPath, error);
	const auto libraryName = (error ? libraryPath : resolved).filename().u8string();
	if (IsSimulatorRuntime({reinterpret_cast<const char*>(libraryName.data()), libraryName.size()}))
		throw std::invalid_argument("Selected manifest points to the simulator; select a physical runtime");
	return path;
}
} // namespace simxr::headset
