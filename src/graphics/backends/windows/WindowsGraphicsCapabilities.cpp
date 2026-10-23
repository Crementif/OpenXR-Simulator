#include "Pch.h"
#include "graphics/backends/windows/WindowsGraphicsCapabilities.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace simxr::windows
{
using Microsoft::WRL::ComPtr;

static constexpr std::array<std::int64_t, 11> s_formats{
	DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
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

XrResult Direct3DMaxSampleCount(bool d3d11, bool d3d12, std::uint32_t& output)
{
	if (!d3d11 && !d3d12)
	{
		output = 1;
		return XR_SUCCESS;
	}
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return XR_ERROR_RUNTIME_FAILURE;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0;; ++index)
	{
		const auto enumerated = factory->EnumAdapters1(index, &adapter);
		if (enumerated == DXGI_ERROR_NOT_FOUND)
			return XR_ERROR_SYSTEM_INVALID;
		if (FAILED(enumerated))
			return XR_ERROR_RUNTIME_FAILURE;
		DXGI_ADAPTER_DESC1 description{};
		if (FAILED(adapter->GetDesc1(&description)))
			return XR_ERROR_RUNTIME_FAILURE;
		if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			break;
		adapter.Reset();
	}
	ComPtr<ID3D11Device> device11;
	ComPtr<ID3D12Device> device12;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (d3d11 && FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
		&level, 1, D3D11_SDK_VERSION, &device11, nullptr, nullptr)))
		return XR_ERROR_RUNTIME_FAILURE;
	if (d3d12 && FAILED(D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device12))))
		return XR_ERROR_RUNTIME_FAILURE;
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
				D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{
					static_cast<DXGI_FORMAT>(format), samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
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
}
