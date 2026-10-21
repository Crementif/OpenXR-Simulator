#include "../TestSupport.h"
#include "platform/windows/D3D11Preview.h"
#include "platform/windows/D3D11PreviewContext.h"

#include <d3dcompiler.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
using Microsoft::WRL::ComPtr;
using simxr::windows::D3D11PreviewContext;

ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* profile)
{
	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> errors;
	const auto result = D3DCompile(source, std::strlen(source), "PreviewContextTest", nullptr, nullptr, entry, profile,
								   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
	if (FAILED(result) && errors)
		std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
	Require(SUCCEEDED(result), "Compile state-isolation fixture shader");
	return code;
}

void RoundTrip(ID3D11Device& device, ID3D11DeviceContext& context)
{
	struct ClearOnExit
	{
		ID3D11DeviceContext& context;
		~ClearOnExit()
		{
			context.ClearState();
		}
	} clearOnExit{context};
	constexpr const char* shader = R"HLSL(
struct Vertex
{
    float4 position : SV_POSITION;
};
Vertex VSMain(uint vertexId : SV_VertexID)
{
    Vertex result;
    result.position = float4(vertexId, 0, 0, 1);
    return result;
}
float4 PSMain(Vertex input) : SV_Target
{
    return input.position;
}
[maxvertexcount(3)]
void GSMain(triangle Vertex input[3], inout TriangleStream<Vertex> stream)
{
    stream.Append(input[0]);
    stream.Append(input[1]);
    stream.Append(input[2]);
}
struct PatchFactors
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};
PatchFactors PatchConstants(InputPatch<Vertex, 3> patch)
{
    PatchFactors result;
    result.edges[0] = 1;
    result.edges[1] = 1;
    result.edges[2] = 1;
    result.inside = 1;
    return result;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")]
Vertex HSMain(InputPatch<Vertex, 3> patch, uint controlPointIndex : SV_OutputControlPointID)
{
    return patch[controlPointIndex];
}
[domain("tri")]
float4 DSMain(PatchFactors factors, float3 coordinate : SV_DomainLocation,
              const OutputPatch<Vertex, 3> patch) : SV_POSITION
{
    return patch[0].position * coordinate.x + patch[1].position * coordinate.y + patch[2].position * coordinate.z;
}
)HLSL";
	const auto vertexCode = Compile(shader, "VSMain", "vs_5_0");
	const auto pixelCode = Compile(shader, "PSMain", "ps_5_0");
	const auto geometryCode = Compile(shader, "GSMain", "gs_5_0");
	const auto hullCode = Compile(shader, "HSMain", "hs_5_0");
	const auto domainCode = Compile(shader, "DSMain", "ds_5_0");
	ComPtr<ID3D11VertexShader> vertex;
	ComPtr<ID3D11PixelShader> pixel;
	ComPtr<ID3D11GeometryShader> geometry;
	ComPtr<ID3D11HullShader> hull;
	ComPtr<ID3D11DomainShader> domain;
	Require(SUCCEEDED(device.CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr,
												vertex.GetAddressOf())),
			"Create fixture vertex shader");
	Require(SUCCEEDED(device.CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr,
											   pixel.GetAddressOf())),
			"Create fixture pixel shader");
	Require(SUCCEEDED(device.CreateGeometryShader(geometryCode->GetBufferPointer(), geometryCode->GetBufferSize(),
												  nullptr, geometry.GetAddressOf())),
			"Create fixture geometry shader");
	Require(SUCCEEDED(device.CreateHullShader(hullCode->GetBufferPointer(), hullCode->GetBufferSize(), nullptr,
											  hull.GetAddressOf())),
			"Create fixture hull shader");
	Require(SUCCEEDED(device.CreateDomainShader(domainCode->GetBufferPointer(), domainCode->GetBufferSize(), nullptr,
												domain.GetAddressOf())),
			"Create fixture domain shader");
	context.VSSetShader(vertex.Get(), nullptr, 0);
	context.PSSetShader(pixel.Get(), nullptr, 0);
	context.GSSetShader(geometry.Get(), nullptr, 0);
	context.HSSetShader(hull.Get(), nullptr, 0);
	context.DSSetShader(domain.Get(), nullptr, 0);
	context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
	const D3D11_VIEWPORT originalViewport{3, 5, 19, 23, 0.25f, 0.75f};
	context.RSSetViewports(1, &originalViewport);
	const D3D11_RECT scissor{7, 9, 21, 29};
	context.RSSetScissorRects(1, &scissor);

	D3D11_BUFFER_DESC bufferDescription{};
	bufferDescription.ByteWidth = 1024;
	bufferDescription.Usage = D3D11_USAGE_DEFAULT;
	bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ComPtr<ID3D11Buffer> constants;
	Require(SUCCEEDED(device.CreateBuffer(&bufferDescription, nullptr, constants.GetAddressOf())),
			"Create fixture constants");
	auto* constantReader = constants.Get();
	context.VSSetConstantBuffers(0, 1, &constantReader);
	context.VSSetConstantBuffers(1, 1, &constantReader);
	ComPtr<ID3D11DeviceContext1> context1;
	context.QueryInterface(IID_PPV_ARGS(context1.GetAddressOf()));
	D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
	const bool ranged = context1 &&
						SUCCEEDED(device.CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
						options.ConstantBufferOffsetting;
	UINT firstConstant = 16;
	UINT constantCount = 32;
	if (ranged)
		context1->VSSetConstantBuffers1(0, 1, &constantReader, &firstConstant, &constantCount);

	bufferDescription.BindFlags = D3D11_BIND_STREAM_OUTPUT;
	ComPtr<ID3D11Buffer> stream;
	Require(SUCCEEDED(device.CreateBuffer(&bufferDescription, nullptr, stream.GetAddressOf())),
			"Create fixture stream output");
	auto* streamReader = stream.Get();
	const UINT streamOffset = 16;
	context.SOSetTargets(1, &streamReader, &streamOffset);

	bufferDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	bufferDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDescription.StructureByteStride = 16;
	ComPtr<ID3D11Buffer> unorderedBuffer;
	Require(SUCCEEDED(device.CreateBuffer(&bufferDescription, nullptr, unorderedBuffer.GetAddressOf())),
			"Create fixture UAV buffer");
	D3D11_UNORDERED_ACCESS_VIEW_DESC unorderedDescription{};
	unorderedDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	unorderedDescription.Buffer.NumElements = bufferDescription.ByteWidth / bufferDescription.StructureByteStride;
	unorderedDescription.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
	ComPtr<ID3D11UnorderedAccessView> unordered;
	Require(SUCCEEDED(device.CreateUnorderedAccessView(unorderedBuffer.Get(), &unorderedDescription,
													   unordered.GetAddressOf())),
			"Create fixture OM UAV");
	auto* unorderedReader = unordered.Get();
	const UINT counter = 7;
	D3D11_TEXTURE2D_DESC targetDescription{};
	targetDescription.Width = 16;
	targetDescription.Height = 16;
	targetDescription.MipLevels = 1;
	targetDescription.ArraySize = 1;
	targetDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	targetDescription.SampleDesc.Count = 1;
	targetDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D> targetImage;
	Require(SUCCEEDED(device.CreateTexture2D(&targetDescription, nullptr, targetImage.GetAddressOf())),
			"Create fixture render target");
	ComPtr<ID3D11RenderTargetView> target;
	Require(SUCCEEDED(device.CreateRenderTargetView(targetImage.Get(), nullptr, target.GetAddressOf())),
			"Create fixture target view");
	auto* targetReader = target.Get();
	context.OMSetRenderTargetsAndUnorderedAccessViews(1, &targetReader, nullptr, 1, 1, &unorderedReader, &counter);

	ComPtr<ID3D11Buffer> readOnlyBuffer;
	bufferDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	Require(SUCCEEDED(device.CreateBuffer(&bufferDescription, nullptr, readOnlyBuffer.GetAddressOf())),
			"Create fixture shader input");
	ComPtr<ID3D11ShaderResourceView> source;
	Require(SUCCEEDED(device.CreateShaderResourceView(readOnlyBuffer.Get(), nullptr, source.GetAddressOf())),
			"Create fixture source view");
	std::array<ID3D11ShaderResourceView*, 2> sources{source.Get(), source.Get()};
	context.PSSetShaderResources(0, static_cast<UINT>(sources.size()), sources.data());
	ComPtr<ID3D11Buffer> computeBuffer;
	bufferDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	Require(SUCCEEDED(device.CreateBuffer(&bufferDescription, nullptr, computeBuffer.GetAddressOf())),
			"Create fixture compute buffer");
	ComPtr<ID3D11UnorderedAccessView> computeView;
	Require(SUCCEEDED(device.CreateUnorderedAccessView(computeBuffer.Get(), &unorderedDescription,
													   computeView.GetAddressOf())),
			"Create fixture compute UAV");
	auto* computeReader = computeView.Get();
	context.CSSetUnorderedAccessViews(0, 1, &computeReader, &counter);

	D3D11_QUERY_DESC predicateDescription{D3D11_QUERY_OCCLUSION_PREDICATE, D3D11_QUERY_MISC_PREDICATEHINT};
	ComPtr<ID3D11Predicate> predicate;
	Require(SUCCEEDED(device.CreatePredicate(&predicateDescription, predicate.GetAddressOf())),
			"Create fixture predication");
	context.SetPredication(predicate.Get(), TRUE);

	try
	{
		D3D11PreviewContext isolated(&context);
		ComPtr<ID3D11GeometryShader> currentGeometry;
		context.GSGetShader(currentGeometry.GetAddressOf(), nullptr, nullptr);
		ComPtr<ID3D11HullShader> currentHull;
		ComPtr<ID3D11DomainShader> currentDomain;
		context.HSGetShader(currentHull.GetAddressOf(), nullptr, nullptr);
		context.DSGetShader(currentDomain.GetAddressOf(), nullptr, nullptr);
		Require(!currentGeometry && !currentHull && !currentDomain, "Preview inherited application graphics stages");
		ComPtr<ID3D11Buffer> currentStream;
		context.SOGetTargets(1, currentStream.GetAddressOf());
		Require(!currentStream, "Preview inherited application stream output");
		ComPtr<ID3D11UnorderedAccessView> currentUnordered;
		context.OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, currentUnordered.GetAddressOf());
		Require(!currentUnordered, "Preview inherited application OM UAV");
		ComPtr<ID3D11Predicate> currentPredicate;
		BOOL predicateValue = TRUE;
		context.GetPredication(currentPredicate.GetAddressOf(), &predicateValue);
		Require(!currentPredicate && !predicateValue, "Preview inherited application predication");
		context.VSSetShader(nullptr, nullptr, 0);
		context.PSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* emptyConstants = nullptr;
		context.VSSetConstantBuffers(0, 1, &emptyConstants);
		ID3D11ShaderResourceView* emptySource = nullptr;
		context.PSSetShaderResources(0, 1, &emptySource);
		context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		const D3D11_VIEWPORT changedViewport{0, 0, 64, 64, 0, 1};
		context.RSSetViewports(1, &changedViewport);
		throw 42;
	}
	catch (int value)
	{
		Require(value == 42, "Unexpected unwind fixture");
	}
	ComPtr<ID3D11VertexShader> restoredVertex;
	ComPtr<ID3D11PixelShader> restoredPixel;
	ComPtr<ID3D11GeometryShader> restoredGeometry;
	context.VSGetShader(restoredVertex.GetAddressOf(), nullptr, nullptr);
	context.PSGetShader(restoredPixel.GetAddressOf(), nullptr, nullptr);
	context.GSGetShader(restoredGeometry.GetAddressOf(), nullptr, nullptr);
	ComPtr<ID3D11HullShader> restoredHull;
	ComPtr<ID3D11DomainShader> restoredDomain;
	context.HSGetShader(restoredHull.GetAddressOf(), nullptr, nullptr);
	context.DSGetShader(restoredDomain.GetAddressOf(), nullptr, nullptr);
	Require(restoredVertex == vertex && restoredPixel == pixel && restoredGeometry == geometry &&
				restoredHull == hull && restoredDomain == domain,
			"Scoped preview did not restore graphics shaders during unwind");
	ComPtr<ID3D11Buffer> restoredConstants;
	if (ranged)
	{
		UINT restoredFirst = 0;
		UINT restoredCount = 0;
		context1->VSGetConstantBuffers1(0, 1, restoredConstants.GetAddressOf(), &restoredFirst, &restoredCount);
		Require(restoredFirst == firstConstant && restoredCount == constantCount, "VS b0 range was lost");
	}
	else
		context.VSGetConstantBuffers(0, 1, restoredConstants.GetAddressOf());
	Require(restoredConstants == constants, "VS b0 was lost");
	ComPtr<ID3D11Buffer> untouchedConstants;
	context.VSGetConstantBuffers(1, 1, untouchedConstants.GetAddressOf());
	Require(untouchedConstants == constants, "Preview changed an untouched VS buffer slot");
	ComPtr<ID3D11Buffer> restoredStream;
	context.SOGetTargets(1, restoredStream.GetAddressOf());
	Require(restoredStream == stream, "SO binding was lost");
	ComPtr<ID3D11UnorderedAccessView> restoredUnordered;
	ComPtr<ID3D11RenderTargetView> restoredTarget;
	context.OMGetRenderTargetsAndUnorderedAccessViews(1, restoredTarget.GetAddressOf(), nullptr, 1, 1,
													  restoredUnordered.GetAddressOf());
	Require(restoredUnordered == unordered && restoredTarget == target, "Mixed RTV/OM UAV bindings were lost");
	std::array<ComPtr<ID3D11ShaderResourceView>, 2> restoredSources;
	context.PSGetShaderResources(0, 1, restoredSources[0].GetAddressOf());
	context.PSGetShaderResources(1, 1, restoredSources[1].GetAddressOf());
	Require(restoredSources[0] == source && restoredSources[1] == source,
			"Changed PS slot was not restored or neighboring slot was changed");
	ComPtr<ID3D11UnorderedAccessView> unchangedCompute;
	context.CSGetUnorderedAccessViews(0, 1, unchangedCompute.GetAddressOf());
	Require(unchangedCompute == computeView, "Preview changed unrelated compute state");
	ComPtr<ID3D11Predicate> restoredPredicate;
	BOOL restoredPredicateValue = FALSE;
	context.GetPredication(restoredPredicate.GetAddressOf(), &restoredPredicateValue);
	Require(restoredPredicate == predicate && restoredPredicateValue, "Predication was lost");
	D3D11_PRIMITIVE_TOPOLOGY topology{};
	context.IAGetPrimitiveTopology(&topology);
	Require(topology == D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, "IA topology was lost");
	D3D11_VIEWPORT viewport{};
	UINT viewportCount = 1;
	context.RSGetViewports(&viewportCount, &viewport);
	Require(viewportCount == 1 && std::memcmp(&viewport, &originalViewport, sizeof(viewport)) == 0,
			"Viewport was lost");
	D3D11_RECT currentScissor{};
	UINT scissorCount = 1;
	context.RSGetScissorRects(&scissorCount, &currentScissor);
	Require(scissorCount == 1 && std::memcmp(&currentScissor, &scissor, sizeof(scissor)) == 0,
			"Preview changed untouched scissor rectangles");
}
} // namespace

int main()
{
	try
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		const auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
											  device.GetAddressOf(), nullptr, context.GetAddressOf());
		if (FAILED(result))
		{
			std::cout << "SKIP: D3D11 WARP unavailable\n";
			return 77;
		}
		if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
		{
			std::cout << "SKIP: D3D11 WARP shader model5 unavailable\n";
			return 77;
		}
		RoundTrip(*device.Get(), *context.Get());
		std::cout << "D3D11 scoped preview state roundtrip passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
