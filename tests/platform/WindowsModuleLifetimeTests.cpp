#include "Pch.h"
#include "../TestSupport.h"

#include <bit>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <windows.h>

namespace simxr::test
{
static HMODULE Loaded(const std::filesystem::path& path)
{
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.c_str(), &module))
		return module;
	const auto error = GetLastError();
	if (error != ERROR_MOD_NOT_FOUND)
		throw std::system_error(static_cast<int>(error), std::system_category(), "Query fixture module");
	return nullptr;
}

static HMODULE Load(const std::filesystem::path& path)
{
	const auto module = LoadLibraryW(path.c_str());
	if (!module)
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Load fixture module");
	return module;
}

static void Unload(HMODULE& module)
{
	if (!FreeLibrary(module))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Unload fixture module");
	module = nullptr;
}

template <typename Function> static Function Procedure(HMODULE module, const char* name)
{
	const auto procedure = GetProcAddress(module, name);
	if (!procedure)
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), name);
	return std::bit_cast<Function>(procedure);
}

static void Run(const std::filesystem::path& path)
{
	using AllocateIdentity = std::uint64_t (*)() noexcept;
	using ReferenceOperation = BOOL (*)() noexcept;
	Require(!Loaded(path), "Fixture was already loaded before the test");
	auto module = Load(path);
	const auto first = Procedure<AllocateIdentity>(module, "allocateProcessIdentity")();
	Require(first != 0, "Initial identity allocation failed");
	Unload(module);
	Require(!Loaded(path), "Fixture did not unload after its caller reference was released");

	module = Load(path);
	const auto allocate = Procedure<AllocateIdentity>(module, "allocateProcessIdentity");
	const auto hold = Procedure<ReferenceOperation>(module, "holdModuleReference");
	const auto release = Procedure<ReferenceOperation>(module, "releaseModuleReference");
	const auto second = allocate();
	Require(second != 0 && second != first, "DLL reload reused a process identity");
	Require(hold(), "Fixture could not retain its module reference");
	const auto retained = module;
	Unload(module);
	Require(Loaded(path) == retained, "Retained module unloaded with the caller reference");

	module = Load(path);
	Require(module == retained, "Reacquiring the retained module loaded a different module");
	Require(release(), "Fixture did not release its retained reference");
	Require(Loaded(path) == module, "Releasing the fixture reference lost the caller reference");
	Unload(module);
	Require(!Loaded(path), "Fixture reference prevented final module unload");
}
} // namespace simxr::test

int wmain(int argc, wchar_t** argv)
{
	try
	{
		Require(argc == 2, "Expected the fixture DLL path");
		simxr::test::Run(std::filesystem::canonical(argv[1]));
		std::puts("PASS Windows module reload identities and retained module lifetime");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
