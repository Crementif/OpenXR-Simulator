#include "platform/windows/D3D12PreviewPipelines.h"

#include "platform/windows/D3D12Preview.h"

#include <d3dcompiler.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace simxr::windows
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr char BlitShader[] = R"HLSL(
cbuffer Blit : register(b0)
{
    float2 uvOffset;
    float2 uvScale;
    float2 uvTap;
    float srgbEncode;
    float padding;
};
Texture2DArray<float4> sourceImage : register(t0);
SamplerState sourceSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 triangleCoordinate = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.uv = uvOffset + triangleCoordinate * uvScale;
    output.position = float4(triangleCoordinate.x * 2.0 - 1.0, 1.0 - triangleCoordinate.y * 2.0, 0.0, 1.0);
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target
{
    float3 color = sourceImage.Sample(sourceSampler, float3(input.uv + float2(-uvTap.x, -uvTap.y), 0)).rgb
                 + sourceImage.Sample(sourceSampler, float3(input.uv + float2( uvTap.x, -uvTap.y), 0)).rgb
                 + sourceImage.Sample(sourceSampler, float3(input.uv + float2(-uvTap.x,  uvTap.y), 0)).rgb
                 + sourceImage.Sample(sourceSampler, float3(input.uv + float2( uvTap.x,  uvTap.y), 0)).rgb;
    color *= 0.25;
    if (srgbEncode > 0.5)
    {
        color = saturate(color);
        color = (color <= 0.0031308) ? color * 12.92 : 1.055 * pow(color, 1.0 / 2.4) - 0.055;
    }
    return float4(color, 1.0);
}
)HLSL";

constexpr char QuadShader[] = R"HLSL(
cbuffer Quad : register(b0)
{
    float4 corner0;
    float4 corner1;
    float4 corner2;
    float4 corner3;
    float4 tangents;
    float4 uvRectangle;
    float4 options;
};
Texture2DArray<float4> sourceImage : register(t0);
SamplerState sourceSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    const uint triangleCorners[6] = {0, 1, 2, 0, 2, 3};
    const float3 corners[4] = {corner0.xyz, corner1.xyz, corner2.xyz, corner3.xyz};
    const float2 coordinates[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
    uint cornerIndex = triangleCorners[vertexId];
    float3 viewPosition = corners[cornerIndex];
    VertexOutput output;
    output.position.x = (2.0 * viewPosition.x + (tangents.y + tangents.x) * viewPosition.z)
                      / (tangents.y - tangents.x);
    output.position.y = (2.0 * viewPosition.y + (tangents.z + tangents.w) * viewPosition.z)
                      / (tangents.z - tangents.w);
    output.position.z = 0.0;
    output.position.w = -viewPosition.z;
    output.uv = lerp(uvRectangle.xy, uvRectangle.zw, coordinates[cornerIndex]);
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target
{
    float4 color = sourceImage.Sample(sourceSampler, float3(input.uv, 0));
    if (options.x > 0.5)
        color.a = 1.0;
    return color;
}
)HLSL";

void Check(HRESULT result, std::string_view operation, ID3DBlob* errors = nullptr)
{
	if (SUCCEEDED(result))
		return;
	std::string message(operation);
	message += " failed (HRESULT ";
	message += std::to_string(static_cast<unsigned long>(result));
	message += ')';
	if (errors && errors->GetBufferSize())
	{
		message += ": ";
		message.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
	}
	throw std::runtime_error(message);
}

ComPtr<ID3DBlob> Compile(std::string_view source, const char* name, const char* entry, const char* target)
{
	ComPtr<ID3DBlob> bytecode;
	ComPtr<ID3DBlob> errors;
	const auto result = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, entry, target,
								   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.GetAddressOf());
	Check(result, std::string(name) + ' ' + entry, errors.Get());
	return bytecode;
}

ComPtr<ID3D12RootSignature> CreateSignature(ID3D12Device& device, UINT constantCount)
{
	D3D12_DESCRIPTOR_RANGE sourceRange{};
	sourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	sourceRange.NumDescriptors = 1;
	sourceRange.BaseShaderRegister = 0;
	sourceRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER parameters[2]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[0].DescriptorTable = {1, &sourceRange};
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[1].Constants = {0, 0, constantCount};
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC description{};
	description.NumParameters = 2;
	description.pParameters = parameters;
	description.NumStaticSamplers = 1;
	description.pStaticSamplers = &sampler;
	description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	const auto result = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
													serialized.GetAddressOf(), errors.GetAddressOf());
	Check(result, "Serialize preview root signature", errors.Get());
	ComPtr<ID3D12RootSignature> signature;
	Check(device.CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
									 IID_PPV_ARGS(signature.GetAddressOf())),
		  "Create preview root signature");
	return signature;
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
	throw std::invalid_argument("Unknown preview channel selection");
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
	throw std::invalid_argument("Unknown preview alpha selection");
}

UINT8 WriteMask(PreviewChannels channels, bool quad)
{
	switch (channels)
	{
	case PreviewChannels::All:
		return D3D12_COLOR_WRITE_ENABLE_ALL;
	case PreviewChannels::Red:
		if (quad)
			return D3D12_COLOR_WRITE_ENABLE_RED;
		return D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_ALPHA;
	case PreviewChannels::Cyan:
		return D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
	}
	throw std::invalid_argument("Unknown preview channel selection");
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC PipelineDescription(ID3D12RootSignature* signature, ID3DBlob& vertex,
													   ID3DBlob& pixel, bool quad)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
	description.pRootSignature = signature;
	description.VS = {vertex.GetBufferPointer(), vertex.GetBufferSize()};
	description.PS = {pixel.GetBufferPointer(), pixel.GetBufferSize()};
	for (auto& target : description.BlendState.RenderTarget)
	{
		target.SrcBlend = D3D12_BLEND_ONE;
		target.DestBlend = D3D12_BLEND_ZERO;
		target.BlendOp = D3D12_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D12_BLEND_ONE;
		target.DestBlendAlpha = D3D12_BLEND_ZERO;
		target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		target.LogicOp = D3D12_LOGIC_OP_NOOP;
	}
	description.SampleMask = (std::numeric_limits<UINT>::max)();
	description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	description.RasterizerState.DepthClipEnable = TRUE;
	description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	description.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
											   D3D12_COMPARISON_FUNC_ALWAYS};
	description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
	description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	description.NumRenderTargets = 1;
	description.RTVFormats[0] = quad ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	return description;
}

DXGI_FORMAT SamplingFormat(DXGI_FORMAT format, bool quad) noexcept
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return quad ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return quad ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return quad ? format : DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return quad ? format : DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:
		return format;
	}
}
} // namespace

PreviewPipelineBinding::PreviewPipelineBinding(ComPtr<ID3D12RootSignature> signature,
											   ComPtr<ID3D12PipelineState> pipeline, Pass pass)
	: m_signature(std::move(signature)), m_pipeline(std::move(pipeline)), m_pass(pass)
{
}

std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> PreviewPipelineBinding::SourceView(const D3D12_RESOURCE_DESC& source,
																				  std::uint32_t slice) const noexcept
{
	if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || source.SampleDesc.Count != 1 ||
		source.MipLevels == 0 || slice >= source.DepthOrArraySize)
		return std::nullopt;
	D3D12_SHADER_RESOURCE_VIEW_DESC description{};
	description.Format = SamplingFormat(source.Format, m_pass == Pass::Quad);
	description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	description.Texture2DArray.FirstArraySlice = slice;
	description.Texture2DArray.ArraySize = 1;
	description.Texture2DArray.MipLevels = 1;
	return description;
}

bool PreviewPipelineBinding::Retain(D3D12Preview& preview) const
{
	return preview.RetainPipeline(m_signature.Get(), m_pipeline.Get());
}

void PreviewPipelineBinding::Bind(ID3D12GraphicsCommandList& commands) const noexcept
{
	commands.SetGraphicsRootSignature(m_signature.Get());
	commands.SetPipelineState(m_pipeline.Get());
}

D3D12PreviewPipelines::D3D12PreviewPipelines(ID3D12Device* device)
{
	if (!device)
		throw std::invalid_argument("Preview pipelines require a D3D12 device");
	const auto blitVertex = Compile(BlitShader, "PreviewBlit", "VSMain", "vs_5_0");
	const auto blitPixel = Compile(BlitShader, "PreviewBlit", "PSMain", "ps_5_0");
	const auto quadVertex = Compile(QuadShader, "PreviewQuad", "VSMain", "vs_5_0");
	const auto quadPixel = Compile(QuadShader, "PreviewQuad", "PSMain", "ps_5_0");
	auto blitSignature = CreateSignature(*device, BlitConstantCount);
	auto quadSignature = CreateSignature(*device, QuadConstantCount);
	std::array<Pipeline, 3> blitPipelines;
	std::array<std::array<Pipeline, 3>, 3> quadPipelines;
	constexpr std::array channels{PreviewChannels::All, PreviewChannels::Red, PreviewChannels::Cyan};
	constexpr std::array alphas{PreviewAlpha::Opaque, PreviewAlpha::Premultiplied, PreviewAlpha::Straight};

	for (const auto selection : channels)
	{
		auto description = PipelineDescription(blitSignature.Get(), *blitVertex.Get(), *blitPixel.Get(), false);
		description.BlendState.RenderTarget[0].RenderTargetWriteMask = WriteMask(selection, false);
		Check(device->CreateGraphicsPipelineState(&description,
												  IID_PPV_ARGS(blitPipelines[ChannelIndex(selection)].GetAddressOf())),
			  "Create preview projection pipeline");
		for (const auto alpha : alphas)
		{
			description = PipelineDescription(quadSignature.Get(), *quadVertex.Get(), *quadPixel.Get(), true);
			auto& blending = description.BlendState.RenderTarget[0];
			blending.RenderTargetWriteMask = WriteMask(selection, true);
			if (alpha != PreviewAlpha::Opaque)
			{
				blending.BlendEnable = TRUE;
				blending.SrcBlend = D3D12_BLEND_ONE;
				if (alpha == PreviewAlpha::Straight)
					blending.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				blending.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				blending.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			}
			Check(device->CreateGraphicsPipelineState(
					  &description,
					  IID_PPV_ARGS(quadPipelines[AlphaIndex(alpha)][ChannelIndex(selection)].GetAddressOf())),
				  "Create preview quad pipeline");
		}
	}
	m_blitSignature = std::move(blitSignature);
	m_quadSignature = std::move(quadSignature);
	m_blitPipelines = std::move(blitPipelines);
	m_quadPipelines = std::move(quadPipelines);
}

PreviewPipelineBinding D3D12PreviewPipelines::Blit(PreviewChannels channels) const
{
	return {m_blitSignature, m_blitPipelines[ChannelIndex(channels)], PreviewPipelineBinding::Pass::Projection};
}

PreviewPipelineBinding D3D12PreviewPipelines::Quad(PreviewAlpha alpha, PreviewChannels channels) const
{
	return {m_quadSignature, m_quadPipelines[AlphaIndex(alpha)][ChannelIndex(channels)],
			PreviewPipelineBinding::Pass::Quad};
}
} // namespace simxr::windows
