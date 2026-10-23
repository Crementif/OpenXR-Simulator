#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace simxr::windows
{
// Owns the native textures. Acquisition and exact source completion stay with
// the session's SwapchainAccess; submitted frames retain the resources they use.
class Direct3DSwapchain final
{
  public:
	// Output must be empty; allocation failure leaves it unchanged.
	static XrResult Create(ID3D11Device* device, const XrSwapchainCreateInfo& description,
						   std::unique_ptr<Direct3DSwapchain>& output);
	static XrResult Create(ID3D12Device* device, const XrSwapchainCreateInfo& description,
						   std::unique_ptr<Direct3DSwapchain>& output);
	Direct3DSwapchain(const Direct3DSwapchain&) = delete;
	Direct3DSwapchain& operator=(const Direct3DSwapchain&) = delete;

	XrResult EnumerateImages(std::uint32_t capacity, std::uint32_t* count,
							 XrSwapchainImageBaseHeader* images) const noexcept;
	std::uint32_t ImageCount() const noexcept;
	DXGI_FORMAT Format() const noexcept;
	ID3D11Texture2D* Image11(std::uint32_t index) const noexcept;
	ID3D12Resource* Image12(std::uint32_t index) const noexcept;
	// Native submission restores the state declared when these images were allocated.
	D3D12_RESOURCE_STATES ReleaseState12() const noexcept;

  private:
	explicit Direct3DSwapchain(DXGI_FORMAT format);
	XrResult Allocate(ID3D11Device* device, const XrSwapchainCreateInfo& description);
	XrResult Allocate(ID3D12Device* device, const XrSwapchainCreateInfo& description);
	const DXGI_FORMAT m_format;
	std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_images11;
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_images12;
	D3D12_RESOURCE_STATES m_releaseState12 = D3D12_RESOURCE_STATE_COMMON;
};
} // namespace simxr::windows
