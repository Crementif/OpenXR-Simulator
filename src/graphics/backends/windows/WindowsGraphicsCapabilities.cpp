#include "graphics/backends/windows/WindowsGraphicsCapabilities.h"
#include "Pch.h"
#include "graphics/backends/vulkan/VulkanDevice.h"
#include "platform/windows/WindowsLibrary.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>

namespace simxr::windows
{
using Microsoft::WRL::ComPtr;

static constexpr std::array<std::int64_t, 11> s_formats{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
														DXGI_FORMAT_R8G8B8A8_UNORM,
														DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
														DXGI_FORMAT_B8G8R8A8_UNORM,
														DXGI_FORMAT_R16G16B16A16_FLOAT,
														DXGI_FORMAT_R32G32B32A32_FLOAT,
														DXGI_FORMAT_R10G10B10A2_UNORM,
														DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
														DXGI_FORMAT_D32_FLOAT,
														DXGI_FORMAT_D24_UNORM_S8_UINT,
														DXGI_FORMAT_D16_UNORM};

std::span<const std::int64_t> Direct3DSwapchainFormats()
{
	return s_formats;
}

static XrResult FindAdapter(const WindowsLibrary& dxgi, ComPtr<IDXGIAdapter1>& output, DXGI_ADAPTER_DESC1& description)
{
	const auto create = std::bit_cast<decltype(&CreateDXGIFactory1)>(dxgi.Procedure("CreateDXGIFactory1"));
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(create(IID_PPV_ARGS(&factory))))
		return XR_ERROR_RUNTIME_FAILURE;

	struct Instance
	{
		VkInstance handle = VK_NULL_HANDLE;

		~Instance()
		{
			if (handle)
				vkDestroyInstance(handle, nullptr);
		}
	} instance;

	VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	info.pApplicationInfo = &application;
	if (CreateRuntimeVulkanInstance(info, &instance.handle) != VK_SUCCESS)
		return XR_ERROR_RUNTIME_FAILURE;
	std::uint32_t count = 0;
	if (vkEnumeratePhysicalDevices(instance.handle, &count, nullptr) != VK_SUCCESS)
		return XR_ERROR_RUNTIME_FAILURE;
	std::vector<VkPhysicalDevice> physical(count);
	if (vkEnumeratePhysicalDevices(instance.handle, &count, physical.data()) != VK_SUCCESS)
		return XR_ERROR_RUNTIME_FAILURE;
	physical.resize(count);
	// DXGI can expose remote/session adapters with the same name as the real GPU.
	// Only a matching Vulkan LUID can support the companion's shared images.
	for (const auto device : physical)
	{
		VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
		vkGetPhysicalDeviceProperties2(device, &properties);
		if (!identity.deviceLUIDValid)
			continue;
		for (UINT index = 0;; ++index)
		{
			ComPtr<IDXGIAdapter1> adapter;
			const auto enumerated = factory->EnumAdapters1(index, &adapter);
			if (enumerated == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(enumerated))
				return XR_ERROR_RUNTIME_FAILURE;
			DXGI_ADAPTER_DESC1 candidate{};
			if (FAILED(adapter->GetDesc1(&candidate)))
				return XR_ERROR_RUNTIME_FAILURE;
			if (!(candidate.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
				!std::memcmp(&candidate.AdapterLuid, identity.deviceLUID, VK_LUID_SIZE))
			{
				output = std::move(adapter);
				description = candidate;
				return XR_SUCCESS;
			}
		}
	}
	return XR_ERROR_SYSTEM_INVALID;
}

XrResult Direct3DAdapterLuid(LUID& output)
{
	const WindowsLibrary dxgi(L"dxgi.dll");
	ComPtr<IDXGIAdapter1> adapter;
	DXGI_ADAPTER_DESC1 description{};
	const auto result = FindAdapter(dxgi, adapter, description);
	if (XR_SUCCEEDED(result))
		output = description.AdapterLuid;
	return result;
}

XrResult Direct3DMaxSampleCount(bool d3d11, bool d3d12, std::uint32_t& output)
{
	if (!d3d11 && !d3d12)
	{
		output = 1;
		return XR_SUCCESS;
	}
	const WindowsLibrary dxgi(L"dxgi.dll");
	std::optional<WindowsLibrary> library11, library12;
	if (d3d11)
		library11.emplace(L"d3d11.dll");
	if (d3d12)
		library12.emplace(L"d3d12.dll");
	ComPtr<IDXGIAdapter1> adapter;
	DXGI_ADAPTER_DESC1 description{};
	const auto result = FindAdapter(dxgi, adapter, description);
	if (XR_FAILED(result))
		return result;
	ComPtr<ID3D11Device> device11;
	ComPtr<ID3D12Device> device12;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (library11)
	{
		const auto create = std::bit_cast<decltype(&D3D11CreateDevice)>(library11->Procedure("D3D11CreateDevice"));
		if (FAILED(create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device11,
						  nullptr, nullptr)))
			return XR_ERROR_RUNTIME_FAILURE;
	}
	if (library12)
	{
		const auto create = std::bit_cast<decltype(&D3D12CreateDevice)>(library12->Procedure("D3D12CreateDevice"));
		if (FAILED(create(adapter.Get(), level, IID_PPV_ARGS(&device12))))
			return XR_ERROR_RUNTIME_FAILURE;
	}
	std::uint32_t maximum = 1;
	for (UINT samples = 2; samples <= 32; samples *= 2)
	{
		bool supported = true;
		for (const auto format : s_formats)
		{
			if (device11)
			{
				UINT qualityLevels = 0;
				if (FAILED(device11->CheckMultisampleQualityLevels(static_cast<DXGI_FORMAT>(format), samples,
																   &qualityLevels)))
					return XR_ERROR_RUNTIME_FAILURE;
				supported &= qualityLevels != 0;
			}
			if (device12)
			{
				D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{static_cast<DXGI_FORMAT>(format), samples,
																	 D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
				if (FAILED(device12->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
														 sizeof(levels))))
					return XR_ERROR_RUNTIME_FAILURE;
				supported &= levels.NumQualityLevels != 0;
			}
		}
		if (supported)
			maximum = samples;
	}
	output = maximum;
	return XR_SUCCESS;
}
} // namespace simxr::windows
