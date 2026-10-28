#include "../TestSupport.h"
#include "app/services/LibraryCatalog.h"
#include "app/services/Utf8Path.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

namespace app = simxr::app;

static void Write(const std::filesystem::path& path, const std::string& bytes)
{
	std::ofstream file(path, std::ios::binary);
	file << bytes;
	file.close();
	Require(bool(file), "Write library fixture");
}
static std::string Read(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}
static app::LibraryResult Finished(app::LibraryCatalog& catalog)
{
	std::optional<app::LibraryResult> result;
	Eventually(
		[&]
		{
			result = catalog.TakeResult();
			return result.has_value();
		},
		"Library worker did not finish");
	return std::move(*result);
}
static void CheckCatalog(const std::filesystem::path& root)
{
	std::filesystem::create_directories(root / "recordings");
	std::filesystem::create_directories(root / "scripts");
	const auto take = root / "recordings" / app::Utf8Path("目-take.luau");
	Write(take, "-- OpenXR Simulator recorded inputs, schema 2.\n-- Launch simxr_runner with --seed 73\n");
	Write(root / "recordings/broken.luau", "-- OpenXR Simulator recorded inputs, schema 2.\n-- bad seed\n");
	Write(root / "scripts/plain.luau", "print('hello')");
	app::LibraryCatalog catalog(root);
	catalog.Refresh();
	auto result = Finished(catalog);
	Require(!result.error && result.entries && result.entries->size() == 3, "Library did not load both folders");
	for (const auto& entry : *result.entries)
	{
		if (entry.path == app::PathText(take))
			Require(entry.take && entry.seed == 73 && entry.error.empty(), "Saved take lost classification or seed");
		else if (entry.name == "broken")
			Require(entry.take && !entry.error.empty(), "Malformed take silently became an executable script");
		else
			Require(!entry.take && entry.error.empty(), "Ordinary script was rejected as a recording");
	}
	const auto destination = root / "recordings/existing.luau";
	Write(destination, "existing content");
	Require(catalog.Rename(app::PathText(take), "existing"), "Admit rename conflict");
	result = Finished(catalog);
	Require(result.error && !result.changed && Read(destination) == "existing content" && std::filesystem::exists(take),
			"Rename overwrote an existing destination or lost its source");
#if defined(__linux__)
	std::filesystem::create_symlink(root / "missing", root / "recordings/dangling.luau");
	Require(catalog.Rename(app::PathText(take), "dangling"), "Admit dangling-link conflict");
	result = Finished(catalog);
	Require(result.error && !result.changed && std::filesystem::is_symlink(root / "recordings/dangling.luau"),
			"Rename replaced a dangling destination link");
	std::filesystem::remove(root / "recordings/dangling.luau");
#endif
	Require(catalog.Rename(app::PathText(take), "目-étude"), "Admit Unicode rename");
	catalog.Refresh();
	result = Finished(catalog);
	const auto renamed = root / "recordings" / app::Utf8Path("目-étude.luau");
	Require(result.action == app::LibraryAction::Rename && result.changed && !result.error &&
				result.renamed == app::PathText(renamed) && std::filesystem::exists(renamed) &&
				!std::filesystem::exists(take),
			"Refresh overwrote a rename result or Unicode path changed");
	while (catalog.Busy())
		Finished(catalog);
	Require(catalog.Delete(app::PathText(renamed)), "Admit delete");
	result = Finished(catalog);
	Require(result.changed && !result.error && !std::filesystem::exists(renamed), "Library delete failed");
	Require(catalog.Delete(app::PathText(root / "scripts")), "Admit invalid directory deletion");
	result = Finished(catalog);
	Require(result.error && !result.changed && std::filesystem::is_directory(root / "scripts"),
			"Library delete accepted a directory");
	std::filesystem::rename(root / "scripts", root / "scripts-held");
	Write(root / "scripts", "not a directory");
	catalog.Refresh();
	result = Finished(catalog);
	Require(result.error && !result.entries, "Failed scan silently replaced the last usable listing");
	std::filesystem::remove(root / "scripts");
	std::filesystem::rename(root / "scripts-held", root / "scripts");
	for (int i = 0; i < 510; ++i)
		Write(root / "recordings" / (std::to_string(i) + ".luau"), "print('bounded')");
	catalog.Refresh();
	result = Finished(catalog);
	Require(!result.error && result.entries && result.entries->size() == 512 && result.limited,
			"Library capacity was exceeded across folders or truncation was hidden");
	Require(!app::LibraryCatalog::ValidName("../escape") && !app::LibraryCatalog::ValidName("a:b") &&
				!app::LibraryCatalog::ValidName(std::string("bad\0name", 8)),
			"Library accepted a path or embedded NUL name");
	const auto closing = root / "closing.luau";
	Write(closing, "print('close')");
	{
		app::LibraryCatalog shuttingDown(root);
		Require(shuttingDown.Delete(app::PathText(closing)), "Admit deletion before shutdown");
	}
	Require(!std::filesystem::exists(closing), "Worker shutdown abandoned an accepted file operation");
}
int main()
{
	const auto root = std::filesystem::temp_directory_path() /
					  ("simxr-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup
	{
		std::filesystem::path path;
		~Cleanup()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	} cleanup{root};
	try
	{
		CheckCatalog(root);
		std::cout << "Library catalog tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
