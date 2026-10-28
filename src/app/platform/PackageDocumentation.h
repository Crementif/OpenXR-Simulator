#pragma once

#include <filesystem>
#include <string>

namespace simxr::app
{
enum class PackageDocument
{
	Guide,
	Licenses
};

std::filesystem::path PackageDocumentPath(const std::filesystem::path& executableDirectory, PackageDocument document);
std::string OpenPackageDocument(PackageDocument document);
} // namespace simxr::app
