#include "../TestSupport.h"
#include "Pch.h"
#include "headset/RuntimeSelection.h"

#include <iostream>

namespace simxr::headset::tests
{
class Files
{
  public:
	Files()
	{
		m_path = std::filesystem::temp_directory_path() /
				 ("simxr-headset-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Require(std::filesystem::create_directory(m_path), "Create isolated manifest directory");
	}
	~Files()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}
	std::filesystem::path Write(const std::filesystem::path& name, std::string_view text)
	{
		const auto path = m_path / name;
		std::ofstream output(path, std::ios::binary);
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		output.close();
		Require(static_cast<bool>(output), "Write manifest fixture");
		return path;
	}

  private:
	std::filesystem::path m_path;
};

static void Rejected(const std::filesystem::path& path)
{
	bool rejected = false;
	try
	{
		ValidateRuntimeManifest(path);
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected, "Unsafe or unusable manifest must be rejected");
}

static void Run()
{
	Require(ValidateRuntimeManifest({}).empty(), "System selection remains explicit");
	Require(IsSimulatorRuntime("OpenXR Simulator Linux Vulkan"), "Linux runtime identity");
	Require(IsSimulatorRuntime("OpenXR Simulator Runtime"), "Windows runtime identity");
	Require(!IsSimulatorRuntime("VirtualDesktopXR"), "Physical runtime identity");
	Files files;
	const auto valid =
		files.Write("physical.json", R"({"file_format_version":"1.0.0","runtime":{"library_path":"physical.so"}})");
	Require(ValidateRuntimeManifest(valid) == std::filesystem::canonical(valid), "Canonical physical manifest");
	PrepareHostEnvironment(valid);
	Require(std::getenv("XR_RUNTIME_JSON") != nullptr, "Explicit selection sets a child override");
	const auto unicode = files.Write(std::filesystem::path(u8"headset-\u65e5\u672c-\u00e9.json"),
									 R"({"runtime":{"library_path":"physical.so"}})");
	const auto canonical = ValidateRuntimeManifest(unicode);
	Require(canonical == std::filesystem::canonical(unicode), "Unicode manifest path is preserved");
	PrepareHostEnvironment(canonical);
#ifdef _WIN32
	const auto* selected = _wgetenv(L"XR_RUNTIME_JSON");
	Require(selected && canonical.native() == selected, "Windows selection preserves the wide manifest path");
#else
	const auto* selected = std::getenv("XR_RUNTIME_JSON");
	Require(selected && canonical.native() == selected, "Linux selection preserves the manifest bytes");
#endif
	PrepareHostEnvironment({});
	const auto* inherited = std::getenv("XR_RUNTIME_JSON");
	Require(!inherited || !*inherited, "System selection removes an inherited simulator override");
	Rejected(files.Write("alias.json", R"({"runtime":{"library_path":"./libopenxr_simulator.so"}})"));
	Rejected(files.Write("empty.json", "{}"));
	Rejected(files.Write("too-large.json", std::string(65537, ' ')));
	Rejected(files.Write("nul.json", std::string("{}\0{}", 5)));
	Rejected(valid.parent_path() / "absent.json");
	const auto simulator = files.Write("openxr_simulator.json", R"({"runtime":{"library_path":"renamed.so"}})");
	Rejected(simulator);
#ifndef _WIN32
	const auto alias = valid.parent_path() / "symlink.json";
	std::filesystem::create_symlink(simulator, alias);
	Rejected(alias);
	const auto library = files.Write("libopenxr_simulator.so", "fixture");
	std::filesystem::create_symlink(library, valid.parent_path() / "renamed.so");
	Rejected(files.Write("renamed.json", R"({"runtime":{"library_path":"renamed.so"}})"));
#endif
}
} // namespace simxr::headset::tests

int main()
{
	try
	{
		simxr::headset::tests::Run();
		std::cout << "Headset runtime selection checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
