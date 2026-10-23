#include "graphics/backends/d3d12/D3D12VulkanImport.h"

namespace simxr
{
std::shared_ptr<WindowsVulkanImport> ImportD3D12Frame(std::shared_ptr<VulkanDevice> device,
	std::shared_ptr<D3D12Frame> frame)
{
	if (!frame)
		throw std::invalid_argument("D3D12 import requires a frame");
	Microsoft::WRL::ComPtr<ID3D12Device> sourceDevice;
	if (FAILED(frame->Texture()->GetDevice(IID_PPV_ARGS(&sourceDevice))))
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Cannot identify D3D12 adapter");
	const auto description = frame->Texture()->GetDesc();
	const WindowsSharedImage source{sourceDevice->GetAdapterLuid(), frame->TextureHandle(), frame->FenceHandle(),
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, description.Format, static_cast<std::uint32_t>(description.Width),
		description.Height, description.DepthOrArraySize, description.MipLevels};
	return WindowsVulkanImport::Create(std::move(device), source, std::move(frame));
}
}
