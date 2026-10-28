#include "Pch.h"
#include "app/platform/PackageDocumentation.h"
#include "app/platform/OpenLocalPath.h"
#include "app/services/Utf8Path.h"

#include <SDL3/SDL.h>

namespace simxr::app
{
std::filesystem::path PackageDocumentPath(const std::filesystem::path& executableDirectory, PackageDocument document)
{
	const auto directory = executableDirectory.has_filename() ? executableDirectory : executableDirectory.parent_path();
	const auto package = directory.parent_path();
	return document == PackageDocument::Guide ? package / "README.txt" : package / "share" / "licenses";
}

std::string OpenPackageDocument(PackageDocument document)
{
	const auto* base = SDL_GetBasePath();
	if (!base)
		return std::string("Cannot find the application directory: ") + SDL_GetError();
	const auto directory = Utf8Path(base);
	if (!directory.is_absolute())
		return "Cannot find an absolute application directory.";
	const auto path = PackageDocumentPath(directory, document);
	std::error_code error;
	const bool exists = document == PackageDocument::Guide ? std::filesystem::is_regular_file(path, error) :
		std::filesystem::is_directory(path, error);
	if (!exists || error)
		return "This build does not include the requested package documentation.";
	return OpenLocalPath(path);
}
} // namespace simxr::app
