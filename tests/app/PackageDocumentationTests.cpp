#include "Pch.h"
#include "app/platform/PackageDocumentation.h"
#include "app/services/Utf8Path.h"
#ifndef _WIN32
#include "app/platform/OpenLocalPath.h"
#endif
#include "../TestSupport.h"
#include <iostream>

int main(int argc, char** argv)
{
	using namespace simxr::app;
	if (argc == 2 && (std::string_view(argv[1]) == "--open-guide" || std::string_view(argv[1]) == "--open-licenses"))
	{
		const auto document =
			std::string_view(argv[1]) == "--open-guide" ? PackageDocument::Guide : PackageDocument::Licenses;
		const auto error = OpenPackageDocument(document);
		if (!error.empty())
		{
			std::cerr << error << '\n';
			return 1;
		}
		return 0;
	}
	const auto root = std::filesystem::temp_directory_path() / Utf8Path("simxr package Ω #");
	const auto binary = root / "bin";
	Require(PackageDocumentPath(binary, PackageDocument::Guide) == root / "README.txt",
			"Guide follows the relocated package");
	Require(PackageDocumentPath(binary / "", PackageDocument::Licenses) == root / "share" / "licenses",
			"A trailing separator still resolves the package license directory");
#ifndef _WIN32
	const auto uri = LocalFileUri(root / "README.txt");
	Require(uri.starts_with("file:///") && uri.find("%20") != std::string::npos &&
				uri.find("%23") != std::string::npos && uri.find("%CE%A9") != std::string::npos,
			"Local file URI encodes spaces, special characters and UTF-8 bytes");
#endif
	return 0;
}
