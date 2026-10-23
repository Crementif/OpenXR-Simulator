#include "Pch.h"
#include "graphics/backends/d3d11/D3D11Backend.h"
#include "CompositorHlsl.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace simxr
{
namespace
{
using Microsoft::WRL::ComPtr;

void Check(HRESULT result, const char* operation)
{
	if (FAILED(result))
		throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(result) + ")");
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target, bool multisampledDepth = false)
{
	ComPtr<ID3DBlob> bytecode;
	ComPtr<ID3DBlob> errors;
	const D3D_SHADER_MACRO defines[]{{"SIMXR_DEPTH_MS", "1"}, {nullptr, nullptr}};
	const auto result = D3DCompile(shaders::CompositorHlsl, sizeof(shaders::CompositorHlsl) - 1,
		"Compositor.hlsl", multisampledDepth ? defines : nullptr, nullptr, entry, target,
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
	if (FAILED(result) && errors)
		throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
	Check(result, "Compile shared compositor shader");
	return bytecode;
}

bool CompatibleFormat(DXGI_FORMAT texture, DXGI_FORMAT view)
{
	if (texture == view)
		return true;
	if (texture == DXGI_FORMAT_R8G8B8A8_TYPELESS)
		return view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	if (texture == DXGI_FORMAT_B8G8R8A8_TYPELESS)
		return view == DXGI_FORMAT_B8G8R8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	if (texture == DXGI_FORMAT_R32G32B32A32_TYPELESS)
		return view == DXGI_FORMAT_R32G32B32A32_FLOAT;
	if (texture == DXGI_FORMAT_R16G16B16A16_TYPELESS)
		return view == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (texture == DXGI_FORMAT_R10G10B10A2_TYPELESS)
		return view == DXGI_FORMAT_R10G10B10A2_UNORM;
	return false;
}

struct DepthFormat
{
	DXGI_FORMAT resource;
	DXGI_FORMAT view;
};
std::optional<DepthFormat> DepthFormats(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS:
		return DepthFormat{DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
		return DepthFormat{DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS:
		return DepthFormat{DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
		return DepthFormat{DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
	default:
		return {};
	}
}

bool SameDevice(ID3D11DeviceChild* resource, ID3D11Device* expected)
{
	ComPtr<ID3D11Device> device;
	resource->GetDevice(&device);
	return device.Get() == expected;
}
}

D3D11Frame::D3D11Frame(std::span<const FrameView> views) : FrameOutput(views) {}

D3D11Frame::~D3D11Frame()
{
	if (m_textureHandle)
		CloseHandle(m_textureHandle);
	if (m_fenceHandle)
		CloseHandle(m_fenceHandle);
}

CompletionStatus D3D11Frame::PollCompletion() const noexcept
{
	auto status = m_completion.load();
	if (status != CompletionStatus::Pending)
		return status;
	const auto value = m_fence->GetCompletedValue();
	const auto observed = value == std::numeric_limits<std::uint64_t>::max() ? CompletionStatus::Failed :
		value >= ReadyValue ? CompletionStatus::Complete : CompletionStatus::Pending;
	m_completion.compare_exchange_strong(status, observed);
	return m_completion.load();
}

HANDLE D3D11Frame::TextureHandle() const noexcept
{
	return m_textureHandle;
}
HANDLE D3D11Frame::FenceHandle() const noexcept
{
	return m_fenceHandle;
}
ID3D11Texture2D* D3D11Frame::Texture() const noexcept
{
	return m_texture.Get();
}

DXGI_FORMAT D3D11Frame::Format() const noexcept
{
	return m_format;
}

HRESULT D3D11Backend::Allocate(std::uint32_t width, std::uint32_t height, std::uint32_t layers, DXGI_FORMAT format,
	std::span<const FrameView> views, std::shared_ptr<D3D11Frame>& output, std::uint32_t mipLevels, bool retained)
{
	std::erase_if(m_pending, [](const auto& frame) { return frame->PollCompletion() == CompletionStatus::Complete; });
	const auto& pool = retained ? m_retainedFrames : m_frames;
	auto reservation = pool.Reserve();
	if (!reservation)
		return DXGI_ERROR_WAS_STILL_DRAWING;
	ComPtr<ID3D11Device5> device;
	auto result = m_device.As(&device);
	if (FAILED(result))
		return result;
	ComPtr<ID3D11DeviceContext> immediate;
	m_device->GetImmediateContext(&immediate);
	ComPtr<ID3D11DeviceContext4> context;
	result = immediate.As(&context);
	if (FAILED(result))
		return result;
	auto frame = std::shared_ptr<D3D11Frame>(new D3D11Frame(views));
	D3D11_TEXTURE2D_DESC description{};
	description.Width = width;
	description.Height = height;
	description.MipLevels = mipLevels;
	description.ArraySize = layers;
	description.Format = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
		? DXGI_FORMAT_B8G8R8A8_TYPELESS : format == DXGI_FORMAT_R16G16B16A16_FLOAT
		? DXGI_FORMAT_R16G16B16A16_TYPELESS : format == DXGI_FORMAT_R10G10B10A2_UNORM
		? DXGI_FORMAT_R10G10B10A2_TYPELESS : format == DXGI_FORMAT_R32G32B32A32_FLOAT
		? DXGI_FORMAT_R32G32B32A32_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
	frame->m_format = format;
	description.SampleDesc.Count = 1;
	description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	result = device->CreateTexture2D(&description, nullptr, &frame->m_texture);
	if (FAILED(result))
		return result;
	ComPtr<IDXGIResource1> shared;
	result = frame->m_texture.As(&shared);
	if (FAILED(result))
		return result;
	result = shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		nullptr, &frame->m_textureHandle);
	if (FAILED(result))
		return result;
	result = device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&frame->m_fence));
	if (FAILED(result))
		return result;
	result = frame->m_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &frame->m_fenceHandle);
	if (FAILED(result))
		return result;
	if (!pool.Consume(reservation))
		return E_UNEXPECTED;
	frame->m_reservation = std::move(reservation);
	output = std::move(frame);
	return S_OK;
}

HRESULT D3D11Backend::Prepare(const CompositionPlan& plan,
	std::span<const D3D11CompositionSource> sources, std::shared_ptr<D3D11Frame>& output, std::uint32_t outputLayers)
{
	if (!outputLayers || outputLayers > 2)
		return E_INVALIDARG;
	std::shared_ptr<D3D11Frame> frame;
	auto result = Allocate(plan.width, plan.height, outputLayers, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, plan.views, frame);
	if (FAILED(result))
		return result;
	result = Record(plan, sources, frame->m_texture.Get(), frame->m_commands);
	if (FAILED(result))
		return result;
	output = std::move(frame);
	return S_OK;
}

HRESULT D3D11Backend::PrepareCapture(const LayerImageDescription& image, D3D11CompositionSource source,
	std::shared_ptr<D3D11Frame>& output, bool retainMipChain)
{
	if (!source.texture || !SameDevice(source.texture, m_device.Get()))
		return E_INVALIDARG;
	D3D11_TEXTURE2D_DESC description{};
	source.texture->GetDesc(&description);
	const auto format = source.format == DXGI_FORMAT_UNKNOWN ? description.Format : source.format;
	if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
		format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
		(!retainMipChain || (format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_R10G10B10A2_UNORM &&
		 format != DXGI_FORMAT_R32G32B32A32_FLOAT)))
		return DXGI_ERROR_UNSUPPORTED;
	if (!CompatibleFormat(description.Format, format))
		return E_INVALIDARG;
	const auto& rectangle = image.rectangle;
	if (image.arraySlice >= description.ArraySize || rectangle.offset.x < 0 || rectangle.offset.y < 0 ||
		rectangle.extent.width <= 0 || rectangle.extent.height <= 0 ||
		static_cast<std::uint64_t>(rectangle.offset.x) + rectangle.extent.width > description.Width ||
		static_cast<std::uint64_t>(rectangle.offset.y) + rectangle.extent.height > description.Height)
		return E_INVALIDARG;
	ComPtr<ID3D11Texture2D> resolved;
	if (description.SampleDesc.Count > 1)
	{
		UINT support = 0;
		const auto supported = m_device->CheckFormatSupport(format, &support);
		if (FAILED(supported) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
			return DXGI_ERROR_UNSUPPORTED;
		auto temporary = description;
		temporary.MipLevels = temporary.ArraySize = 1;
		temporary.SampleDesc = {1, 0};
		temporary.Usage = D3D11_USAGE_DEFAULT;
		temporary.CPUAccessFlags = temporary.MiscFlags = 0;
		temporary.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const auto result = m_device->CreateTexture2D(&temporary, nullptr, &resolved);
		if (FAILED(result))
			return result;
	}
	std::shared_ptr<D3D11Frame> frame;
	const auto mipLevels = retainMipChain ? description.MipLevels : 1;
	auto result = Allocate(mipLevels > 1 ? description.Width : rectangle.extent.width,
		mipLevels > 1 ? description.Height : rectangle.extent.height, 1, format, {}, frame, mipLevels, retainMipChain);
	if (FAILED(result))
		return result;
	m_commands->ClearState();
	const auto subresource = D3D11CalcSubresource(0, image.arraySlice, description.MipLevels);
	if (resolved)
		m_commands->ResolveSubresource(resolved.Get(), 0, source.texture, subresource, format);
	const D3D11_BOX box{static_cast<UINT>(rectangle.offset.x), static_cast<UINT>(rectangle.offset.y), 0,
		static_cast<UINT>(rectangle.offset.x + rectangle.extent.width),
		static_cast<UINT>(rectangle.offset.y + rectangle.extent.height), 1};
	for (UINT mip = 0; mip < mipLevels; ++mip)
		m_commands->CopySubresourceRegion(frame->m_texture.Get(), mip, 0, 0, 0,
			resolved ? resolved.Get() : source.texture,
			resolved ? 0 : D3D11CalcSubresource(mip, image.arraySlice, description.MipLevels),
			mipLevels > 1 ? nullptr : &box);
	result = m_commands->FinishCommandList(FALSE, &frame->m_commands);
	if (FAILED(result))
		return result;
	output = std::move(frame);
	return S_OK;
}

HRESULT D3D11Backend::Submit(const std::shared_ptr<D3D11Frame>& frame)
{
	if (!frame || frame->m_submitted || !frame->m_commands || !SameDevice(frame->m_texture.Get(), m_device.Get()))
		return E_INVALIDARG;
	ComPtr<ID3D11DeviceContext> immediate;
	m_device->GetImmediateContext(&immediate);
	ComPtr<ID3D11DeviceContext4> context;
	auto result = immediate.As(&context);
	if (FAILED(result))
		return result;
	m_pending.push_back(frame);
	frame->m_submitted = true;
	context->ExecuteCommandList(frame->m_commands.Get(), TRUE);
	frame->m_commands.Reset();
	result = context->Signal(frame->m_fence.Get(), D3D11Frame::ReadyValue);
	if (FAILED(result))
	{
		frame->m_completion.store(CompletionStatus::Failed);
		return result;
	}
	context->Flush();
	return S_OK;
}

D3D11Backend::D3D11Backend(ID3D11Device* device) : m_device(device)
{
	if (!device)
		throw std::invalid_argument("D3D11 compositor requires a device");
	m_pending.reserve(m_frames.Capacity() + m_retainedFrames.Capacity());
	Check(device->CreateDeferredContext(0, &m_commands), "Create compositor context");
	const auto vertex = Compile("VSMain", "vs_5_0");
	const auto pixel = Compile("PSMain", "ps_5_0");
	Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &m_vertex),
		"Create compositor vertex shader");
	Check(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &m_pixel),
		"Create compositor pixel shader");
	D3D11_BUFFER_DESC buffer{};
	buffer.ByteWidth = sizeof(LayerConstants);
	buffer.Usage = D3D11_USAGE_DYNAMIC;
	buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	Check(device->CreateBuffer(&buffer, nullptr, &m_constants), "Create compositor constants");
	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	Check(device->CreateSamplerState(&sampler, &m_sampler), "Create compositor sampler");
	D3D11_BLEND_DESC blend{};
	auto& color = blend.RenderTarget[0];
	color.BlendEnable = TRUE;
	color.SrcBlend = color.SrcBlendAlpha = D3D11_BLEND_ONE;
	color.DestBlend = color.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	color.BlendOp = color.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	color.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	Check(device->CreateBlendState(&blend, &m_blend), "Create compositor blending");
	D3D11_RASTERIZER_DESC rasterizer{};
	rasterizer.FillMode = D3D11_FILL_SOLID;
	rasterizer.CullMode = D3D11_CULL_NONE;
	rasterizer.DepthClipEnable = TRUE;
	Check(device->CreateRasterizerState(&rasterizer, &m_rasterizer), "Create compositor rasterizer");
	D3D11_DEPTH_STENCIL_DESC depth{};
	Check(device->CreateDepthStencilState(&depth, &m_depth), "Create compositor depth state");
}

HRESULT D3D11Backend::Record(const CompositionPlan& plan,
	std::span<const D3D11CompositionSource> sources, ID3D11Texture2D* output,
	ComPtr<ID3D11CommandList>& outputCommands)
{
	if (!output || !plan.width || !plan.height || sources.size() != plan.draws.size() ||
		!SameDevice(output, m_device.Get()))
		return E_INVALIDARG;
	D3D11_TEXTURE2D_DESC description{};
	output->GetDesc(&description);
	if (description.Width != plan.width || description.Height != plan.height || !description.ArraySize || description.ArraySize > 2 ||
		description.MipLevels != 1 || description.SampleDesc.Count != 1 ||
		!(description.BindFlags & D3D11_BIND_RENDER_TARGET) ||
		(description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && description.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
		return E_INVALIDARG;
	struct SourceCopy
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11ShaderResourceView> view;
		D3D11_TEXTURE2D_DESC original;
		DXGI_FORMAT format;
		bool depth;
	};
	std::vector<SourceCopy> copies(sources.size());
	for (std::size_t index = 0; index < sources.size(); ++index)
	{
		const auto& source = sources[index];
		const auto& draw = plan.draws[index];
		if (!source.texture || draw.eye >= description.ArraySize || source.texture == output ||
			!SameDevice(source.texture, m_device.Get()))
			return E_INVALIDARG;
		auto& copy = copies[index];
		source.texture->GetDesc(&copy.original);
		if (draw.source.arraySlice >= copy.original.ArraySize)
			return E_INVALIDARG;
		copy.format = source.format == DXGI_FORMAT_UNKNOWN ? copy.original.Format : source.format;
		copy.depth = draw.constants.control[0] == 2;
		auto description = copy.original;
		if (copy.depth)
		{
			const auto depth = DepthFormats(copy.format);
			const auto original = DepthFormats(copy.original.Format);
			if (!depth || !original || depth->resource != original->resource)
				return DXGI_ERROR_UNSUPPORTED;
			copy.format = depth->view;
			description.Format = depth->resource;
		}
		else if (!CompatibleFormat(copy.original.Format, copy.format))
			return E_INVALIDARG;
		UINT support = 0;
		auto result = m_device->CheckFormatSupport(copy.format, &support);
		const UINT required = copy.depth
			? D3D11_FORMAT_SUPPORT_SHADER_LOAD | (copy.original.SampleDesc.Count > 1 ? D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD : 0)
			: D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | (copy.original.SampleDesc.Count > 1 ? D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE : 0);
		if (FAILED(result) || (support & required) != required)
			return DXGI_ERROR_UNSUPPORTED;
		description.ArraySize = 1;
		if (!copy.depth)
			description.SampleDesc = {1, 0};
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		description.CPUAccessFlags = description.MiscFlags = 0;
		result = m_device->CreateTexture2D(&description, nullptr, &copy.texture);
		if (FAILED(result))
			return result;
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = copy.format;
		view.ViewDimension = copy.depth && description.SampleDesc.Count > 1
			? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
		if (view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
			view.Texture2D.MipLevels = description.MipLevels;
		result = m_device->CreateShaderResourceView(copy.texture.Get(), &view, &copy.view);
		if (FAILED(result))
			return result;
	}
	std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
	for (UINT eye = 0; eye < description.ArraySize; ++eye)
	{
		D3D11_RENDER_TARGET_VIEW_DESC view{};
		view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		view.Texture2DArray.FirstArraySlice = eye;
		view.Texture2DArray.ArraySize = 1;
		const auto result = m_device->CreateRenderTargetView(output, &view, &targets[eye]);
		if (FAILED(result))
			return result;
	}
	if (!m_depthMultisample && std::any_of(copies.begin(), copies.end(),
		[](const SourceCopy& source) { return source.depth && source.original.SampleDesc.Count > 1; }))
	{
		const auto shader = Compile("PSMain", "ps_5_0", true);
		const auto result = m_device->CreatePixelShader(shader->GetBufferPointer(), shader->GetBufferSize(),
			nullptr, &m_depthMultisample);
		if (FAILED(result))
			return result;
	}
	m_commands->ClearState();
	for (std::size_t index = 0; index < sources.size(); ++index)
	{
		const auto& copy = copies[index];
		const auto slice = plan.draws[index].source.arraySlice;
		for (UINT mip = 0; mip < copy.original.MipLevels; ++mip)
		{
			const auto subresource = D3D11CalcSubresource(mip, slice, copy.original.MipLevels);
			if (!copy.depth && copy.original.SampleDesc.Count > 1)
				m_commands->ResolveSubresource(copy.texture.Get(), mip, sources[index].texture, subresource, copy.format);
			else
				m_commands->CopySubresourceRegion(copy.texture.Get(), mip, 0, 0, 0,
					sources[index].texture, subresource, nullptr);
		}
	}
	m_commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_commands->VSSetShader(m_vertex.Get(), nullptr, 0);
	m_commands->PSSetShader(m_pixel.Get(), nullptr, 0);
	m_commands->PSSetConstantBuffers(2, 1, m_constants.GetAddressOf());
	m_commands->PSSetSamplers(1, 1, m_sampler.GetAddressOf());
	m_commands->RSSetState(m_rasterizer.Get());
	const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(plan.width), static_cast<float>(plan.height), 0, 1};
	m_commands->RSSetViewports(1, &viewport);
	m_commands->OMSetBlendState(m_blend.Get(), nullptr, 0xffffffff);
	m_commands->OMSetDepthStencilState(m_depth.Get(), 0);
	const float clear[]{0, 0, 0, 1};
	HRESULT result = S_OK;
	for (UINT eye = 0; eye < description.ArraySize && SUCCEEDED(result); ++eye)
	{
		m_commands->ClearRenderTargetView(targets[eye].Get(), clear);
		m_commands->OMSetRenderTargets(1, targets[eye].GetAddressOf(), nullptr);
		for (std::size_t index = 0; index < plan.draws.size(); ++index)
		{
			if (plan.draws[index].eye != eye)
				continue;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			result = m_commands->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
			if (FAILED(result))
				break;
			*static_cast<LayerConstants*>(mapped.pData) = plan.draws[index].constants;
			m_commands->Unmap(m_constants.Get(), 0);
			m_commands->PSSetShader(copies[index].depth && copies[index].original.SampleDesc.Count > 1
				? m_depthMultisample.Get() : m_pixel.Get(), nullptr, 0);
			m_commands->PSSetShaderResources(0, 1, copies[index].view.GetAddressOf());
			m_commands->Draw(3, 0);
		}
	}
	// Finish even after a recording failure, so the next frame cannot execute
	// abandoned commands. Executing with restoration preserves application state.
	ComPtr<ID3D11CommandList> commands;
	const auto finished = m_commands->FinishCommandList(FALSE, &commands);
	if (FAILED(result))
		return result;
	if (FAILED(finished))
		return finished;
	outputCommands = std::move(commands);
	return S_OK;
}
}
