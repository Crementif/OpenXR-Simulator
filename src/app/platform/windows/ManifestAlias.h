#pragma once

#include <string>

namespace simxr::app::windows
{
// Keeps a private compatibility manifest available after the launcher exits.
std::wstring PrepareManifestAlias(const std::wstring& manifest);
} // namespace simxr::app::windows
