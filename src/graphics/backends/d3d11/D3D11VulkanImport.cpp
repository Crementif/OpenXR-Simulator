#include "graphics/backends/d3d11/D3D11VulkanImport.h"
#include "Pch.h"

namespace simxr
{
std::shared_ptr<WindowsVulkanImport> ImportD3D11Frame(std::shared_ptr<VulkanDevice> device,
													  std::shared_ptr<D3D11Frame> frame)
{
	if (!frame)
		throw std::invalid_argument("D3D11 import requires a frame");
	Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
	frame->Texture()->GetDevice(&sourceDevice);
	Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC adapterDescription{};
	if (FAILED(sourceDevice.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
		FAILED(adapter->GetDesc(&adapterDescription)))
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Cannot identify D3D11 adapter");
	D3D11_TEXTURE2D_DESC description{};
	frame->Texture()->GetDesc(&description);
	const WindowsSharedImage source{adapterDescription.AdapterLuid,
									frame->TextureHandle(),
									frame->FenceHandle(),
									VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
									frame->Format(),
									description.Width,
									description.Height,
									description.ArraySize,
									description.MipLevels};
	return WindowsVulkanImport::Create(std::move(device), source, std::move(frame));
}
} // namespace simxr
