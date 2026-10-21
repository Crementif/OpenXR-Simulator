#include "platform/windows/D3D11Preview.h"
#include "platform/windows/D3D11PreviewContext.h"

#include <d3dcompiler.h>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace simxr::windows
{
static_assert(sizeof(PreviewQuadCorners) == 64);
namespace
{
using Microsoft::WRL::ComPtr;

constexpr char PreviewShader[] = R"HLSL(
Texture2D<float4> sourceImage : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer Quad : register(b0)
{
    float4 corners[4];
};
struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 coordinate = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(coordinate.x - 1.0, 1.0 - coordinate.y, 0.0, 1.0);
    output.uv = coordinate * 0.5;
    return output;
}
VertexOutput QuadVSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.position = corners[vertexId];
    output.uv = float2(vertexId & 1, (vertexId >> 1) & 1);
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target
{
    return sourceImage.Sample(sourceSampler, input.uv);
}
)HLSL";

void Check(HRESULT result, std::string_view operation, ID3DBlob* diagnostics = nullptr)
{
	if (SUCCEEDED(result))
		return;
	std::string message(operation);
	message += " failed (HRESULT ";
	message += std::to_string(static_cast<unsigned long>(result));
	message += ')';
	if (diagnostics && diagnostics->GetBufferSize())
	{
		message += ": ";
		message.append(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
	}
	throw std::runtime_error(message);
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* profile)
{
	ComPtr<ID3DBlob> shader;
	ComPtr<ID3DBlob> diagnostics;
	const auto result =
		D3DCompile(PreviewShader, sizeof(PreviewShader) - 1, "D3D11Preview", nullptr, nullptr, entry, profile,
				   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.GetAddressOf(), diagnostics.GetAddressOf());
	Check(result, entry, diagnostics.Get());
	return shader;
}

std::size_t ChannelIndex(PreviewChannels channels)
{
	switch (channels)
	{
	case PreviewChannels::All:
		return 0;
	case PreviewChannels::Red:
		return 1;
	case PreviewChannels::Cyan:
		return 2;
	}
	throw std::invalid_argument("Unknown D3D11 preview channel selection");
}

std::size_t AlphaIndex(PreviewAlpha alpha)
{
	switch (alpha)
	{
	case PreviewAlpha::Opaque:
		return 0;
	case PreviewAlpha::Premultiplied:
		return 1;
	case PreviewAlpha::Straight:
		return 2;
	}
	throw std::invalid_argument("Unknown D3D11 preview alpha selection");
}

UINT8 WriteMask(PreviewChannels channels)
{
	switch (channels)
	{
	case PreviewChannels::All:
		return D3D11_COLOR_WRITE_ENABLE_ALL;
	case PreviewChannels::Red:
		return D3D11_COLOR_WRITE_ENABLE_RED;
	case PreviewChannels::Cyan:
		return D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	}
	throw std::invalid_argument("Unknown D3D11 preview channel selection");
}

D3D11_BLEND_DESC BlendDescription(PreviewAlpha alpha, PreviewChannels channels, bool quad)
{
	D3D11_BLEND_DESC description{};
	for (auto& target : description.RenderTarget)
	{
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_ZERO;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ONE;
		target.DestBlendAlpha = D3D11_BLEND_ZERO;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	}
	auto& target = description.RenderTarget[0];
	target.RenderTargetWriteMask = WriteMask(channels);
	if (quad)
		target.RenderTargetWriteMask &= ~D3D11_COLOR_WRITE_ENABLE_ALPHA;
	if (alpha != PreviewAlpha::Opaque)
	{
		target.BlendEnable = TRUE;
		if (alpha == PreviewAlpha::Straight)
			target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	}
	return description;
}

DXGI_FORMAT SamplingFormat(DXGI_FORMAT resource, DXGI_FORMAT declared) noexcept
{
	switch (resource)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		if (declared == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
			return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		if (declared == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
			return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:
		return resource;
	}
}

} // namespace

D3D11PreviewImage::operator bool() const noexcept
{
	return m_texture && m_view;
}

bool D3D11PreviewImage::Matches(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) const noexcept
{
	if (!*this)
		return false;
	D3D11_TEXTURE2D_DESC description{};
	m_texture->GetDesc(&description);
	return description.Width == width && description.Height == height && description.Format == format;
}

D3D11Preview::D3D11Preview(ID3D11Device* device)
{
	if (!device)
		throw std::invalid_argument("D3D11 preview requires a device");
	ComPtr<ID3D11DeviceContext> context;
	device->GetImmediateContext(context.GetAddressOf());
	if (!context)
		throw std::runtime_error("D3D11 preview requires an immediate context");
	const auto projectionCode = Compile("VSMain", "vs_5_0");
	const auto quadCode = Compile("QuadVSMain", "vs_5_0");
	const auto pixelCode = Compile("PSMain", "ps_5_0");
	ComPtr<ID3D11VertexShader> projectionVertex;
	ComPtr<ID3D11VertexShader> quadVertex;
	ComPtr<ID3D11PixelShader> pixel;
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11RasterizerState> rasterizer;
	std::array<BlendState, 3> projectionBlends;
	std::array<std::array<BlendState, 3>, 3> quadBlends;
	Check(device->CreateVertexShader(projectionCode->GetBufferPointer(), projectionCode->GetBufferSize(), nullptr,
									 projectionVertex.GetAddressOf()),
		  "Create D3D11 projection shader");
	Check(device->CreateVertexShader(quadCode->GetBufferPointer(), quadCode->GetBufferSize(), nullptr,
									 quadVertex.GetAddressOf()),
		  "Create D3D11 quad shader");
	Check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
									pixel.GetAddressOf()),
		  "Create D3D11 preview pixel shader");
	D3D11_BUFFER_DESC bufferDescription{};
	bufferDescription.ByteWidth = sizeof(PreviewQuadCorners);
	bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
	bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	Check(device->CreateBuffer(&bufferDescription, nullptr, constants.GetAddressOf()), "Create D3D11 quad constants");
	D3D11_SAMPLER_DESC samplerDescription{};
	samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDescription.MaxAnisotropy = 1;
	samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
	Check(device->CreateSamplerState(&samplerDescription, sampler.GetAddressOf()), "Create D3D11 preview sampler");
	D3D11_RASTERIZER_DESC rasterizerDescription{};
	rasterizerDescription.FillMode = D3D11_FILL_SOLID;
	rasterizerDescription.CullMode = D3D11_CULL_NONE;
	rasterizerDescription.DepthClipEnable = TRUE;
	Check(device->CreateRasterizerState(&rasterizerDescription, rasterizer.GetAddressOf()),
		  "Create D3D11 preview rasterizer");
	constexpr std::array channels{PreviewChannels::All, PreviewChannels::Red, PreviewChannels::Cyan};
	constexpr std::array alphas{PreviewAlpha::Opaque, PreviewAlpha::Premultiplied, PreviewAlpha::Straight};
	for (const auto channel : channels)
	{
		auto description = BlendDescription(PreviewAlpha::Opaque, channel, false);
		Check(device->CreateBlendState(&description, projectionBlends[ChannelIndex(channel)].GetAddressOf()),
			  "Create D3D11 projection channels");
		for (const auto alpha : alphas)
		{
			description = BlendDescription(alpha, channel, true);
			Check(device->CreateBlendState(&description,
										   quadBlends[AlphaIndex(alpha)][ChannelIndex(channel)].GetAddressOf()),
				  "Create D3D11 quad blend");
		}
	}
	m_device = device;
	m_context = std::move(context);
	m_projectionVertex = std::move(projectionVertex);
	m_quadVertex = std::move(quadVertex);
	m_pixel = std::move(pixel);
	m_quadConstants = std::move(constants);
	m_sampler = std::move(sampler);
	m_rasterizer = std::move(rasterizer);
	m_projectionBlends = std::move(projectionBlends);
	m_quadBlends = std::move(quadBlends);
}

bool D3D11Preview::IsFor(ID3D11Device* device) const noexcept
{
	return m_device.Get() == device;
}

D3D11PreviewImage D3D11Preview::CreateImage(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format)
{
	if (width == 0 || height == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		throw std::invalid_argument("D3D11 preview image extent is unsupported");
	D3D11_TEXTURE2D_DESC description{};
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11PreviewImage image;
	Check(m_device->CreateTexture2D(&description, nullptr, image.m_texture.GetAddressOf()),
		  "Create D3D11 preview image");
	Check(m_device->CreateShaderResourceView(image.m_texture.Get(), nullptr, image.m_view.GetAddressOf()),
		  "Create D3D11 preview image view");
	return image;
}

D3D11PreviewImage D3D11Preview::Acquire(Cache& cache, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format)
{
	auto& entry = cache.images[cache.next];
	if (!entry.Matches(width, height, format))
	{
		auto replacement = CreateImage(width, height, format);
		entry = std::move(replacement);
	}
	cache.next = (cache.next + 1) % cache.images.size();
	return entry;
}

D3D11Preview::Cache& D3D11Preview::SelectCache(PreviewSurface surface)
{
	switch (surface)
	{
	case PreviewSurface::Projection:
		return m_projectionCache;
	case PreviewSurface::Quad:
		return m_quadCache;
	}
	throw std::invalid_argument("Unknown D3D11 preview source surface");
}

bool D3D11Preview::Owns(const D3D11PreviewImage& image) const noexcept
{
	if (!image)
		return false;
	ComPtr<ID3D11Device> device;
	image.m_texture->GetDevice(device.GetAddressOf());
	return device.Get() == m_device.Get();
}

void D3D11Preview::ValidateScope(const D3D11PreviewContext& scope) const
{
	if (!scope.IsFor(m_context.Get()))
		throw std::invalid_argument("D3D11 preview scope belongs to another context");
}

D3D11PreviewImage D3D11Preview::Copy(D3D11PreviewContext& scope, PreviewSurface surface, ID3D11Texture2D* source,
									 std::uint32_t slice, DXGI_FORMAT declaredFormat,
									 std::optional<PreviewRegion> region)
{
	ValidateScope(scope);
	if (!source)
		return {};
	ComPtr<ID3D11Device> sourceDevice;
	source->GetDevice(sourceDevice.GetAddressOf());
	if (!IsFor(sourceDevice.Get()))
		throw std::invalid_argument("D3D11 preview source belongs to another device");
	D3D11_TEXTURE2D_DESC description{};
	source->GetDesc(&description);
	if (slice >= description.ArraySize || description.MipLevels == 0)
		throw std::invalid_argument("D3D11 preview source slice is invalid");
	PreviewRegion selected{0, 0, description.Width, description.Height};
	if (region)
		selected = *region;
	if (selected.width == 0 || selected.height == 0 || selected.x > description.Width ||
		selected.y > description.Height || selected.width > description.Width - selected.x ||
		selected.height > description.Height - selected.y)
		throw std::invalid_argument("D3D11 preview source rectangle is invalid");
	const auto format = SamplingFormat(description.Format, declaredFormat);
	if (description.SampleDesc.Count > 1 && region && !m_resolve.Matches(description.Width, description.Height, format))
		m_resolve = CreateImage(description.Width, description.Height, format);
	auto image = Acquire(SelectCache(surface), selected.width, selected.height, format);
	const auto subresource = D3D11CalcSubresource(0, slice, description.MipLevels);
	if (description.SampleDesc.Count > 1 && !region)
	{
		m_context->ResolveSubresource(image.m_texture.Get(), 0, source, subresource, format);
		return image;
	}
	ID3D11Texture2D* copySource = source;
	UINT copySubresource = subresource;
	if (description.SampleDesc.Count > 1)
	{
		m_context->ResolveSubresource(m_resolve.m_texture.Get(), 0, source, subresource, format);
		copySource = m_resolve.m_texture.Get();
		copySubresource = 0;
	}
	D3D11_BOX sourceBox{selected.x, selected.y, 0, selected.x + selected.width, selected.y + selected.height, 1};
	m_context->CopySubresourceRegion(image.m_texture.Get(), 0, 0, 0, 0, copySource, copySubresource, &sourceBox);
	return image;
}

D3D11PreviewImage D3D11Preview::Upload(D3D11PreviewContext& scope, PreviewSurface surface, std::uint32_t width,
									   std::uint32_t height, DXGI_FORMAT format, std::span<const std::uint8_t> pixels,
									   std::uint32_t rowPitch)
{
	ValidateScope(scope);
	if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
		format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
		throw std::invalid_argument("D3D11 preview upload requires RGBA8 or BGRA8 pixels");
	if (width == 0 || height == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		throw std::invalid_argument("D3D11 preview upload extent is unsupported");
	const auto rowBytes = static_cast<std::uint64_t>(width) * 4;
	const auto required = height == 0 ? 0 : static_cast<std::uint64_t>(height - 1) * rowPitch + rowBytes;
	if (rowBytes > rowPitch || required > pixels.size())
		throw std::invalid_argument("D3D11 preview upload row layout is invalid");
	auto image = Acquire(SelectCache(surface), width, height, format);
	m_context->UpdateSubresource(image.m_texture.Get(), 0, nullptr, pixels.data(), rowPitch, 0);
	return image;
}

void D3D11Preview::Bind(const D3D11PreviewImage& image, ID3D11RenderTargetView& target, const D3D11_VIEWPORT& viewport,
						ID3D11VertexShader& vertex, ID3D11BlendState& blend)
{
	m_context->GSSetShader(nullptr, nullptr, 0);
	m_context->HSSetShader(nullptr, nullptr, 0);
	m_context->DSSetShader(nullptr, nullptr, 0);
	m_context->VSSetShader(&vertex, nullptr, 0);
	m_context->PSSetShader(m_pixel.Get(), nullptr, 0);
	auto* view = image.m_view.Get();
	auto* sampler = m_sampler.Get();
	m_context->PSSetShaderResources(0, 1, &view);
	m_context->PSSetSamplers(0, 1, &sampler);
	m_context->IASetInputLayout(nullptr);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_context->OMSetBlendState(&blend, nullptr, (std::numeric_limits<UINT>::max)());
	m_context->OMSetDepthStencilState(nullptr, 0);
	m_context->RSSetState(m_rasterizer.Get());
	ID3D11RenderTargetView* targets[]{&target};
	m_context->OMSetRenderTargets(1, targets, nullptr);
	m_context->RSSetViewports(1, &viewport);
}

void D3D11Preview::DrawAndUnbind()
{
	m_context->Draw(4, 0);
	ID3D11ShaderResourceView* empty = nullptr;
	m_context->PSSetShaderResources(0, 1, &empty);
}

bool D3D11Preview::DrawProjection(D3D11PreviewContext& scope, const D3D11PreviewImage& image,
								  ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport,
								  PreviewChannels channels)
{
	ValidateScope(scope);
	if (!Owns(image) || !target)
		return false;
	Bind(image, *target, viewport, *m_projectionVertex.Get(), *m_projectionBlends[ChannelIndex(channels)].Get());
	DrawAndUnbind();
	return true;
}

bool D3D11Preview::DrawQuad(D3D11PreviewContext& scope, const D3D11PreviewImage& image, ID3D11RenderTargetView* target,
							const D3D11_VIEWPORT& viewport, const PreviewQuadCorners& corners, PreviewAlpha alpha,
							PreviewChannels channels)
{
	ValidateScope(scope);
	if (!Owns(image) || !target)
		return false;
	const auto& blend = m_quadBlends[AlphaIndex(alpha)][ChannelIndex(channels)];
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(m_context->Map(m_quadConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	std::memcpy(mapped.pData, corners.data(), sizeof(corners));
	m_context->Unmap(m_quadConstants.Get(), 0);
	auto* constants = m_quadConstants.Get();
	m_context->VSSetConstantBuffers(0, 1, &constants);
	Bind(image, *target, viewport, *m_quadVertex.Get(), *blend.Get());
	DrawAndUnbind();
	return true;
}
} // namespace simxr::windows
