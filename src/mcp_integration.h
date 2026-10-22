// Explicit native screenshot acquisition. Filesystem publication belongs to the
// instance-owned compatibility worker; no GPU/native readers cross that boundary.
#pragma once

#include "control/legacy/WindowsLegacyCapture.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <string>
#include <windows.h>
#include <wrl/client.h>

namespace mcp
{
inline const std::filesystem::path& GetSimulatorDataDirectory()
{
	static const auto directory = []
	{
		const auto environment = [](const wchar_t* name)
		{
			std::wstring value;
			for (;;)
			{
				const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
				if (count < value.size() || count == 0)
				{
					value.resize(count);
					return value;
				}
				value.resize(count);
			}
		};
		if (const auto explicitPath = environment(L"SIMXR_DATA_DIR"); !explicitPath.empty())
			return std::filesystem::path(explicitPath);
		const auto base = environment(L"LOCALAPPDATA");
		return std::filesystem::path(base.empty() ? L"." : base) / L"OpenXR-Simulator";
	}();
	return directory;
}
inline const char* GetSessionStateName(int state)
{
	switch (state)
	{
	case 0:
		return "UNKNOWN";
	case 1:
		return "IDLE";
	case 2:
		return "READY";
	case 3:
		return "SYNCHRONIZED";
	case 4:
		return "VISIBLE";
	case 5:
		return "FOCUSED";
	case 6:
		return "STOPPING";
	case 7:
		return "LOSS_PENDING";
	case 8:
		return "EXITING";
	default:
		return "UNKNOWN";
	}
}
inline bool CaptureScreenshot(WindowsLegacyCapture& capture, ID3D11Device* device, ID3D11DeviceContext* context,
							  IDXGISwapChain1* swapchain)
{
	if (!device || !context || !swapchain || !capture.ScreenshotRequested())
		return false;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
	if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(source.GetAddressOf()))))
		return false;
	D3D11_TEXTURE2D_DESC description{};
	source->GetDesc(&description);
	auto stagingDescription = description;
	stagingDescription.Usage = D3D11_USAGE_STAGING;
	stagingDescription.BindFlags = 0;
	stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDescription.MiscFlags = 0;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, staging.GetAddressOf())))
		return false;
	context->CopyResource(staging.Get(), source.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
		return false;
	struct Mapping
	{
		ID3D11DeviceContext* context;
		ID3D11Texture2D* image;
		~Mapping()
		{
			context->Unmap(image, 0);
		}
	} mapping{context, staging.Get()};
	const bool bgra =
		description.Format == DXGI_FORMAT_B8G8R8A8_UNORM || description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	return capture.SubmitPixels(static_cast<const std::uint8_t*>(mapped.pData), description.Width, description.Height,
								mapped.RowPitch, bgra);
}
} // namespace mcp
