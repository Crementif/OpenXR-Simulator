#include "Pch.h"
#include "app/platform/windows/ManifestAlias.h"

#include <iostream>

int wmain(int argc, wchar_t** argv)
{
	if (argc != 2)
		return 2;
	const auto alias = simxr::app::windows::PrepareManifestAlias(argv[1]);
	if (alias == argv[1])
		return 1;
	std::wcout << alias;
	return std::wcout ? 0 : 1;
}
