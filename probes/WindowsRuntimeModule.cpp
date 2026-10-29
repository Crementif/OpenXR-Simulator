#include "WindowsRuntimeModule.h"
#include "Pch.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <openxr/openxr_loader_negotiation.h>
#include <stdexcept>
#include <system_error>
#include <windows.h>

namespace simxr::probe
{
std::vector<std::string> WindowsArguments(int count, wchar_t** arguments)
{
	std::vector<std::string> result;
	result.reserve(static_cast<std::size_t>(count));
	for (int index = 0; index < count; ++index)
	{
		const auto size =
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arguments[index], -1, nullptr, 0, nullptr, nullptr);
		if (!size)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Read probe argument");
		std::string argument(static_cast<std::size_t>(size), '\0');
		if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arguments[index], -1, argument.data(), size, nullptr,
								 nullptr))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Read probe argument");
		argument.pop_back();
		result.push_back(std::move(argument));
	}
	return result;
}

std::filesystem::path RuntimeModulePath(std::string_view path)
{
	if (path.empty())
		throw std::runtime_error("Expected the runtime DLL path after --verify-unload");
	return std::filesystem::canonical(std::u8string(path.begin(), path.end()));
}

static HMODULE LoadedModule(const std::filesystem::path& path)
{
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.c_str(), &module))
		return module;
	const auto error = GetLastError();
	if (error != ERROR_MOD_NOT_FOUND)
		throw std::system_error(static_cast<int>(error), std::system_category(), "Query runtime module");
	return nullptr;
}

void RequireRuntimeModule(const std::filesystem::path& path, bool loaded)
{
	if (static_cast<bool>(LoadedModule(path)) != loaded)
		throw std::runtime_error(loaded ? "The requested runtime DLL is not loaded"
										: "Runtime DLL remained loaded after complete retirement");
}

static void Check(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

template <typename Function>
static Function RuntimeFunction(PFN_xrGetInstanceProcAddr getProcedure, XrInstance instance, const char* name)
{
	PFN_xrVoidFunction procedure = nullptr;
	Check(getProcedure(instance, name, &procedure), name);
	if (!procedure)
		throw std::runtime_error(std::string("Missing runtime function: ") + name);
	return std::bit_cast<Function>(procedure);
}

void VerifyRuntimeReload(const std::filesystem::path& path)
{
	RequireRuntimeModule(path, false);
	XrInstance retired = XR_NULL_HANDLE;
	for (unsigned cycle = 0; cycle < 2; ++cycle)
	{
		const auto module = LoadLibraryW(path.c_str());
		if (!module)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Load runtime module");
		try
		{
			const auto procedure = GetProcAddress(module, "xrNegotiateLoaderRuntimeInterface");
			if (!procedure)
				throw std::runtime_error("Missing runtime negotiation export");
			const auto negotiate = std::bit_cast<PFN_xrNegotiateLoaderRuntimeInterface>(procedure);
			const XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
											   XR_LOADER_INFO_STRUCT_VERSION,
											   sizeof(XrNegotiateLoaderInfo),
											   XR_CURRENT_LOADER_RUNTIME_VERSION,
											   XR_CURRENT_LOADER_RUNTIME_VERSION,
											   XR_MAKE_VERSION(1, 0, 0),
											   XR_API_VERSION_1_0};
			XrNegotiateRuntimeRequest runtime{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
											  XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(XrNegotiateRuntimeRequest)};
			Check(negotiate(&loader, &runtime), "Negotiate reloaded runtime");
			if (!runtime.getInstanceProcAddr)
				throw std::runtime_error("Reloaded runtime did not return its dispatch function");
			const auto create =
				RuntimeFunction<PFN_xrCreateInstance>(runtime.getInstanceProcAddr, XR_NULL_HANDLE, "xrCreateInstance");
			XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
			info.applicationInfo = {"Runtime reload probe", 1, "", 0, XR_API_VERSION_1_0};
			XrInstance instance = XR_NULL_HANDLE;
			Check(create(&info, &instance), "Create raw runtime instance");
			const auto properties = RuntimeFunction<PFN_xrGetInstanceProperties>(runtime.getInstanceProcAddr, instance,
																				 "xrGetInstanceProperties");
			const auto destroy =
				RuntimeFunction<PFN_xrDestroyInstance>(runtime.getInstanceProcAddr, instance, "xrDestroyInstance");
			XrInstanceProperties output{XR_TYPE_INSTANCE_PROPERTIES};
			Check(properties(instance, &output), "Query raw runtime instance");
			if (retired && (instance == retired || properties(retired, &output) != XR_ERROR_HANDLE_INVALID ||
							destroy(retired) != XR_ERROR_HANDLE_INVALID))
				throw std::runtime_error("Runtime reload accepted a retired runtime identity");
			Check(properties(instance, &output), "Query live runtime instance after stale rejection");
			Check(destroy(instance), "Destroy raw runtime instance");
			retired = instance;
			if (!FreeLibrary(module))
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
										"Unload runtime module");
			RequireRuntimeModule(path, false);
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "FAIL: %s; preserving runtime owners until process exit\n", error.what());
			std::fflush(nullptr);
			TerminateProcess(GetCurrentProcess(), 1);
			std::_Exit(1);
		}
	}
	std::puts("PASS runtime DLL unloaded twice and rejected its retired runtime identity after reload");
}
} // namespace simxr::probe
