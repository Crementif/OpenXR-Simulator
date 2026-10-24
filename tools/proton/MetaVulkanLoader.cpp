// Standalone x64 PE adapter, built without a Windows SDK or simulator dependencies.
using Address = void*;
using Result = int;
using Flags = unsigned int;
static_assert(sizeof(Address) == 8 && sizeof(Result) == 4 && sizeof(Flags) == 4);

extern "C" __declspec(dllimport) Address vkGetInstanceProcAddr(Address instance, const char* name);
extern "C" __declspec(dllimport) Address vkGetDeviceProcAddr(Address device, const char* name);

static constexpr Result s_extensionNotPresent = -7;

static bool Equal(const char* left, const char* right)
{
	if (!left)
		return false;
	while (*left && *left == *right)
	{
		++left;
		++right;
	}
	return *left == *right;
}

static Result GetMemoryProperties(Address device, Flags type, Address handle, Address properties)
{
	using Function = Result (*)(Address, Flags, Address, Address);
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
	return function ? function(device, type, handle, properties) : s_extensionNotPresent;
}

static Result GetMemoryHandle(Address device, const void* info, Address handle)
{
	using Function = Result (*)(Address, const void*, Address);
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
	return function ? function(device, info, handle) : s_extensionNotPresent;
}

static Result GetSemaphoreHandle(Address device, const void* info, Address handle)
{
	using Function = Result (*)(Address, const void*, Address);
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
	return function ? function(device, info, handle) : s_extensionNotPresent;
}

static Result GetFenceHandle(Address device, const void* info, Address handle)
{
	using Function = Result (*)(Address, const void*, Address);
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, "vkGetFenceWin32HandleKHR"));
	return function ? function(device, info, handle) : s_extensionNotPresent;
}

static Result ImportSemaphore(Address device, const void* info)
{
	using Function = Result (*)(Address, const void*);
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
	return function ? function(device, info) : s_extensionNotPresent;
}

static Result ImportFence(Address device, const void* info)
{
	using Function = Result (*)(Address, const void*);
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, "vkImportFenceWin32HandleKHR"));
	return function ? function(device, info) : s_extensionNotPresent;
}

extern "C" Address MetaGetInstanceProcAddr(Address instance, const char* name)
{
	auto function = vkGetInstanceProcAddr(instance, name);
	if (function || !instance)
		return function;

	// Wine exposes these Win32 commands through device lookup only. Resolve with
	// the caller's real device when invoked; preserve its result and ownership.
	if (Equal(name, "vkGetMemoryWin32HandlePropertiesKHR"))
		return reinterpret_cast<Address>(GetMemoryProperties);
	if (Equal(name, "vkGetMemoryWin32HandleKHR"))
		return reinterpret_cast<Address>(GetMemoryHandle);
	if (Equal(name, "vkGetSemaphoreWin32HandleKHR"))
		return reinterpret_cast<Address>(GetSemaphoreHandle);
	if (Equal(name, "vkGetFenceWin32HandleKHR"))
		return reinterpret_cast<Address>(GetFenceHandle);
	if (Equal(name, "vkImportSemaphoreWin32HandleKHR"))
		return reinterpret_cast<Address>(ImportSemaphore);
	if (Equal(name, "vkImportFenceWin32HandleKHR"))
		return reinterpret_cast<Address>(ImportFence);
	return nullptr;
}
