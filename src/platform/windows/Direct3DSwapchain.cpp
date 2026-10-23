#include "platform/windows/Direct3DSwapchain.h"
#include "Pch.h"

#include "graphics/backends/windows/WindowsGraphicsCapabilities.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr_platform.h>

#include <bit>

namespace simxr::windows
{
static XrResult Validate(const XrSwapchainCreateInfo& description) noexcept
{
	if (description.type != XR_TYPE_SWAPCHAIN_CREATE_INFO || !description.width || !description.height ||
		!description.arraySize || !description.sampleCount || !description.mipCount || !description.usageFlags ||
		(description.faceCount != 1 && description.faceCount != 6))
		return XR_ERROR_VALIDATION_FAILURE;
	constexpr XrSwapchainUsageFlags supportedUsage =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
		XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	if ((description.createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
		(description.usageFlags & ~supportedUsage) || description.faceCount != 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	if (!std::has_single_bit(description.sampleCount) || description.sampleCount > 32 ||
		(description.sampleCount != 1 && description.mipCount != 1) ||
		description.mipCount >
			static_cast<std::uint32_t>(std::bit_width(std::max(description.width, description.height))) ||
		description.arraySize > 2048)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto formats = Direct3DSwapchainFormats();
	if (std::find(formats.begin(), formats.end(), description.format) == formats.end())
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	return XR_SUCCESS;
}

static bool IsDepth(DXGI_FORMAT format) noexcept
{
	return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
		   format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D16_UNORM;
}

static DXGI_FORMAT Typeless(DXGI_FORMAT format) noexcept
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return DXGI_FORMAT_R32G32B32A32_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_TYPELESS;
	default:
		return format;
	}
}

Direct3DSwapchain::Direct3DSwapchain(DXGI_FORMAT format) : m_format(format) {}

XrResult Direct3DSwapchain::Create(ID3D11Device* device, const XrSwapchainCreateInfo& description,
								   std::unique_ptr<Direct3DSwapchain>& output)
{
	const auto validation = Validate(description);
	if (XR_FAILED(validation))
		return validation;
	if (output)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (!device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	auto prepared =
		std::unique_ptr<Direct3DSwapchain>(new Direct3DSwapchain(static_cast<DXGI_FORMAT>(description.format)));
	const auto result = prepared->Allocate(device, description);
	if (XR_FAILED(result))
		return result;
	output.swap(prepared);
	return XR_SUCCESS;
}

XrResult Direct3DSwapchain::Create(ID3D12Device* device, const XrSwapchainCreateInfo& description,
								   std::unique_ptr<Direct3DSwapchain>& output)
{
	const auto validation = Validate(description);
	if (XR_FAILED(validation))
		return validation;
	if (output)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (!device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	auto prepared =
		std::unique_ptr<Direct3DSwapchain>(new Direct3DSwapchain(static_cast<DXGI_FORMAT>(description.format)));
	const auto result = prepared->Allocate(device, description);
	if (XR_FAILED(result))
		return result;
	output.swap(prepared);
	return XR_SUCCESS;
}

XrResult Direct3DSwapchain::Allocate(ID3D11Device* device, const XrSwapchainCreateInfo& description)
{
	const bool depth = IsDepth(m_format);
	const bool sampledDepth = depth && (description.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
	D3D11_TEXTURE2D_DESC texture{};
	texture.Format = !depth || sampledDepth ? Typeless(m_format) : m_format;
	texture.Width = description.width;
	texture.Height = description.height;
	texture.ArraySize = description.arraySize;
	texture.MipLevels = description.mipCount;
	texture.SampleDesc.Count = description.sampleCount;
	texture.Usage = D3D11_USAGE_DEFAULT;
	if (depth)
	{
		texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (sampledDepth)
			texture.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
	}
	else
	{
		// Keep typed UNORM and sRGB views usable for the compatibility path.
		texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (description.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
			texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
	}
	const auto count = (description.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1u : 3u;
	m_images11.resize(count);
	for (auto& image : m_images11)
		if (FAILED(device->CreateTexture2D(&texture, nullptr, image.GetAddressOf())))
			return XR_ERROR_RUNTIME_FAILURE;
	return XR_SUCCESS;
}

XrResult Direct3DSwapchain::Allocate(ID3D12Device* device, const XrSwapchainCreateInfo& description)
{
	D3D12_RESOURCE_DESC texture{};
	texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texture.Width = description.width;
	texture.Height = description.height;
	texture.DepthOrArraySize = static_cast<UINT16>(description.arraySize);
	texture.MipLevels = static_cast<UINT16>(description.mipCount);
	texture.Format = m_format;
	texture.SampleDesc.Count = description.sampleCount;
	texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	if (IsDepth(m_format))
	{
		texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		texture.Format = Typeless(m_format);
	}
	else
	{
		if (description.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
			texture.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (description.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
			texture.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (description.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
			texture.Format = Typeless(m_format);
	}
	D3D12_CLEAR_VALUE clear{};
	const D3D12_CLEAR_VALUE* clearValue = nullptr;
	if (texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
	{
		m_releaseState12 = D3D12_RESOURCE_STATE_RENDER_TARGET;
		clear.Format = m_format;
		clear.Color[3] = 1;
		clearValue = &clear;
	}
	else if (texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
	{
		m_releaseState12 = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		clear.Format = m_format;
		clear.DepthStencil.Depth = 1;
		clearValue = &clear;
	}
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	const auto count = (description.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1u : 3u;
	m_images12.resize(count);
	for (auto& image : m_images12)
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, m_releaseState12, clearValue,
												   IID_PPV_ARGS(image.GetAddressOf()))))
			return XR_ERROR_RUNTIME_FAILURE;
	return XR_SUCCESS;
}

XrResult Direct3DSwapchain::EnumerateImages(std::uint32_t capacity, std::uint32_t* count,
											XrSwapchainImageBaseHeader* images) const noexcept
{
	if (!count || (capacity && !images))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto size = ImageCount();
	*count = size;
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < size)
		return XR_ERROR_SIZE_INSUFFICIENT;
	if (!m_images12.empty())
	{
		auto* output = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
		for (std::uint32_t index = 0; index < size; ++index)
			if (output[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)
				return XR_ERROR_VALIDATION_FAILURE;
		for (std::uint32_t index = 0; index < size; ++index)
			output[index].texture = m_images12[index].Get();
	}
	else
	{
		auto* output = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
		for (std::uint32_t index = 0; index < size; ++index)
			if (output[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)
				return XR_ERROR_VALIDATION_FAILURE;
		for (std::uint32_t index = 0; index < size; ++index)
			output[index].texture = m_images11[index].Get();
	}
	return XR_SUCCESS;
}

std::uint32_t Direct3DSwapchain::ImageCount() const noexcept
{
	return static_cast<std::uint32_t>(m_images11.size() + m_images12.size());
}

DXGI_FORMAT Direct3DSwapchain::Format() const noexcept
{
	return m_format;
}

ID3D11Texture2D* Direct3DSwapchain::Image11(std::uint32_t index) const noexcept
{
	return index < m_images11.size() ? m_images11[index].Get() : nullptr;
}

ID3D12Resource* Direct3DSwapchain::Image12(std::uint32_t index) const noexcept
{
	return index < m_images12.size() ? m_images12[index].Get() : nullptr;
}

D3D12_RESOURCE_STATES Direct3DSwapchain::ReleaseState12() const noexcept
{
	return m_releaseState12;
}
} // namespace simxr::windows
