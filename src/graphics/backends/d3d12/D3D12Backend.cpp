#include "graphics/backends/d3d12/D3D12Backend.h"
#include "CompositorHlsl.h"
#include "Pch.h"

#include <d3dcompiler.h>
#include <dxgi.h>
#include <limits>
#include <stdexcept>

namespace simxr
{
using Microsoft::WRL::ComPtr;
static void Check(HRESULT result, const char* operation)
{
	if (FAILED(result))
		throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(result) + ")");
}
static ComPtr<ID3DBlob> Compile(const char* entry, const char* target, bool multisampledDepth = false)
{
	const D3D_SHADER_MACRO definitions[]{{"SIMXR_DEPTH_MS", "1"}, {nullptr, nullptr}};
	ComPtr<ID3DBlob> code, errors;
	const auto result = D3DCompile(shaders::CompositorHlsl, sizeof(shaders::CompositorHlsl) - 1, "Compositor.hlsl",
								   multisampledDepth ? definitions : nullptr, nullptr, entry, target,
								   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(result) && errors)
		throw std::runtime_error(
			std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
	Check(result, "Compile shared compositor shader");
	return code;
}
static void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
					   D3D12_RESOURCE_STATES after, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
	if (before == after)
		return;
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = {resource, subresource, before, after};
	commands->ResourceBarrier(1, &barrier);
}
struct D3D12DepthFormat
{
	DXGI_FORMAT resource, view;
};
static std::optional<D3D12DepthFormat> DepthFormats(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS:
		return D3D12DepthFormat{DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
		return D3D12DepthFormat{DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS:
		return D3D12DepthFormat{DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
		return D3D12DepthFormat{DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
	default:
		return {};
	}
}

static bool SameDevice(ID3D12DeviceChild* object, ID3D12Device* expected)
{
	ComPtr<ID3D12Device> device;
	return object && SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&device))) && device.Get() == expected;
}
static bool ColorFormat(DXGI_FORMAT resource, DXGI_FORMAT view)
{
	const bool supported = view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
						   view == DXGI_FORMAT_B8G8R8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
						   view == DXGI_FORMAT_R16G16B16A16_FLOAT || view == DXGI_FORMAT_R10G10B10A2_UNORM ||
						   view == DXGI_FORMAT_R32G32B32A32_FLOAT;
	if (!supported)
		return false;
	return resource == view ||
		   (resource == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
			(view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) ||
		   (resource == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
			(view == DXGI_FORMAT_B8G8R8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) ||
		   (resource == DXGI_FORMAT_R32G32B32A32_TYPELESS && view == DXGI_FORMAT_R32G32B32A32_FLOAT) ||
		   (resource == DXGI_FORMAT_R16G16B16A16_TYPELESS && view == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
		   (resource == DXGI_FORMAT_R10G10B10A2_TYPELESS && view == DXGI_FORMAT_R10G10B10A2_UNORM);
}

D3D12Frame::D3D12Frame(std::span<const FrameView> views) : FrameOutput(views) {}
D3D12Frame::~D3D12Frame()
{
	if (m_textureHandle)
		CloseHandle(m_textureHandle);
	if (m_fenceHandle)
		CloseHandle(m_fenceHandle);
}
CompletionStatus D3D12Frame::PollCompletion() const noexcept
{
	const auto cached = m_completion.load();
	if (cached != CompletionStatus::Pending || !m_submitted)
		return cached;
	const auto value = m_fence->GetCompletedValue();
	if (value == std::numeric_limits<UINT64>::max())
		m_completion.store(CompletionStatus::Failed);
	else if (value >= ReadyValue)
		m_completion.store(CompletionStatus::Complete);
	return m_completion.load();
}
ID3D12Resource* D3D12Frame::Texture() const noexcept
{
	return m_texture.Get();
}
HANDLE D3D12Frame::TextureHandle() const noexcept
{
	return m_textureHandle;
}
HANDLE D3D12Frame::FenceHandle() const noexcept
{
	return m_fenceHandle;
}

struct D3D12Backend::PendingFrames
{
	std::shared_ptr<FrameReservation> preservation;
	std::vector<std::shared_ptr<D3D12Frame>> frames;
	explicit PendingFrames(std::size_t capacity)
	{
		static const FramePool s_owners{64};
		preservation = s_owners.Reserve();
		if (!preservation)
			throw std::bad_alloc();
		frames.reserve(capacity);
	}
};

D3D12Backend::D3D12Backend(ID3D12Device* device, ID3D12CommandQueue* queue)
	: m_device(device), m_queue(queue),
	  m_pending(std::make_unique<PendingFrames>(m_frames.Capacity() + m_retainedFrames.Capacity()))
{
	if (!device || !SameDevice(queue, device) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
		throw std::invalid_argument("D3D12 compositor requires the application's direct queue");
	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	D3D12_ROOT_PARAMETER parameters[2]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[0].DescriptorTable = {1, &range};
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[1].Constants = {2, 0, sizeof(LayerConstants) / sizeof(std::uint32_t)};
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.MaxAnisotropy = 1;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 1;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC root{};
	root.NumParameters = 2;
	root.pParameters = parameters;
	root.NumStaticSamplers = 1;
	root.pStaticSamplers = &sampler;
	ComPtr<ID3DBlob> serialized, errors;
	Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
		  "Serialize compositor root signature");
	Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
									  IID_PPV_ARGS(&m_root)),
		  "Create compositor root signature");
	const auto vertex = Compile("VSMain", "vs_5_0");
	const auto pixel = Compile("PSMain", "ps_5_0");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
	pipeline.pRootSignature = m_root.Get();
	pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
	pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
	auto& blend = pipeline.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pipeline.SampleMask = UINT_MAX;
	pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pipeline.RasterizerState.DepthClipEnable = TRUE;
	pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipeline.NumRenderTargets = 1;
	pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	pipeline.SampleDesc.Count = 1;
	Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipeline)),
		  "Create canonical compositor pipeline");
	const auto depthMultisample = Compile("PSMain", "ps_5_0", true);
	pipeline.PS = {depthMultisample->GetBufferPointer(), depthMultisample->GetBufferSize()};
	Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_depthMultisample)),
		  "Create canonical multisample depth pipeline");
}

D3D12Backend::~D3D12Backend()
{
	if (m_pending->frames.empty())
		return;
	const auto& last = m_pending->frames.back();
	const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (last->PollCompletion() == CompletionStatus::Pending && event &&
		SUCCEEDED(last->m_fence->SetEventOnCompletion(D3D12Frame::ReadyValue, event)))
		WaitForSingleObject(event, 5000);
	if (event)
		CloseHandle(event);
	if (std::any_of(m_pending->frames.begin(), m_pending->frames.end(),
					[](const auto& frame)
					{
						return frame->PollCompletion() == CompletionStatus::Pending;
					}))
		m_pending.release(); // Preserved frames keep their process-wide reservation charged.
}

HRESULT D3D12Backend::Allocate(const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES state,
							   std::span<const FrameView> views, bool retained, std::shared_ptr<D3D12Frame>& output)
{
	std::erase_if(m_pending->frames,
				  [](const auto& frame)
				  {
					  return frame->PollCompletion() != CompletionStatus::Pending;
				  });
	auto& pool = retained ? m_retainedFrames : m_frames;
	auto reservation = pool.Reserve();
	if (!reservation)
		return DXGI_ERROR_WAS_STILL_DRAWING;
	auto frame = std::shared_ptr<D3D12Frame>(new D3D12Frame(views));
	frame->m_queue = m_queue;
	frame->m_root = m_root;
	frame->m_pipeline = m_pipeline;
	frame->m_depthMultisample = m_depthMultisample;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	auto result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description, state, nullptr,
													IID_PPV_ARGS(&frame->m_texture));
	if (FAILED(result))
		return result;
	result =
		m_device->CreateSharedHandle(frame->m_texture.Get(), nullptr, GENERIC_ALL, nullptr, &frame->m_textureHandle);
	if (FAILED(result))
		return result;
	result = m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&frame->m_fence));
	if (FAILED(result))
		return result;
	result = m_device->CreateSharedHandle(frame->m_fence.Get(), nullptr, GENERIC_ALL, nullptr, &frame->m_fenceHandle);
	if (FAILED(result))
		return result;
	result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame->m_allocator));
	if (FAILED(result))
		return result;
	result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame->m_allocator.Get(), m_pipeline.Get(),
										 IID_PPV_ARGS(&frame->m_commands));
	if (FAILED(result))
		return result;
	if (!pool.Consume(reservation))
		return E_UNEXPECTED;
	frame->m_reservation = std::move(reservation);
	output = std::move(frame);
	return S_OK;
}

HRESULT D3D12Backend::Prepare(const CompositionPlan& plan, std::span<const D3D12CompositionSource> sources,
							  std::shared_ptr<D3D12Frame>& output, std::uint32_t outputLayers)
{
	if (!plan.width || !plan.height || !outputLayers || outputLayers > 2 || sources.size() != plan.draws.size() ||
		sources.size() > UINT_MAX)
		return E_INVALIDARG;
	D3D12_RESOURCE_DESC target{};
	target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	target.Width = plan.width;
	target.Height = plan.height;
	target.DepthOrArraySize = static_cast<UINT16>(outputLayers);
	target.MipLevels = 1;
	target.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	target.SampleDesc.Count = 1;
	target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	std::shared_ptr<D3D12Frame> frame;
	auto result = Allocate(target, D3D12_RESOURCE_STATE_RENDER_TARGET, plan.views, false, frame);
	if (FAILED(result))
		return result;
	frame->m_inputs.reserve(sources.size() * 2);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
	descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	descriptors.NumDescriptors = std::max(1u, static_cast<UINT>(sources.size()));
	descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	result = m_device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&frame->m_sources));
	if (FAILED(result))
		return result;
	descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	descriptors.NumDescriptors = outputLayers;
	descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	result = m_device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&frame->m_targets));
	if (FAILED(result))
		return result;
	const auto sourceStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const auto targetStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto* commands = frame->m_commands.Get();
	for (std::size_t index = 0; index < sources.size(); ++index)
	{
		const auto& source = sources[index];
		const auto& draw = plan.draws[index];
		if (!SameDevice(source.texture, m_device.Get()) || draw.eye >= outputLayers)
			return E_INVALIDARG;
		const auto original = source.texture->GetDesc();
		auto format = source.format == DXGI_FORMAT_UNKNOWN ? original.Format : source.format;
		const bool depth = draw.constants.control[0] == 2;
		auto copyDescription = original;
		if (depth)
		{
			const auto requested = DepthFormats(format);
			const auto available = DepthFormats(original.Format);
			if (!requested || !available || requested->resource != available->resource)
				return DXGI_ERROR_UNSUPPORTED;
			format = requested->view;
			copyDescription.Format = requested->resource;
		}
		else if (!ColorFormat(original.Format, format))
			return DXGI_ERROR_UNSUPPORTED;
		if (original.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			draw.source.arraySlice >= original.DepthOrArraySize)
			return E_INVALIDARG;
		const bool multisampled = original.SampleDesc.Count > 1;
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
		result = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
		const UINT required =
			depth
				? D3D12_FORMAT_SUPPORT1_SHADER_LOAD | (multisampled ? D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD : 0)
				: D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | (multisampled ? D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE : 0);
		if (FAILED(result) || (support.Support1 & required) != required)
			return DXGI_ERROR_UNSUPPORTED;
		copyDescription.DepthOrArraySize = 1;
		if (!depth)
			copyDescription.SampleDesc = {1, 0};
		copyDescription.Alignment = 0;
		copyDescription.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE;
		copyDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		ComPtr<ID3D12Resource> copy;
		const auto copyState =
			multisampled && !depth ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
		const auto sourceState =
			multisampled && !depth ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
		result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copyDescription, copyState, nullptr,
												   IID_PPV_ARGS(&copy));
		if (FAILED(result))
			return result;
		frame->m_inputs.emplace_back(source.texture);
		frame->m_inputs.push_back(copy);
		D3D12_FEATURE_DATA_FORMAT_INFO planes{original.Format, 0};
		result = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &planes, sizeof(planes));
		if (FAILED(result))
			return result;
		for (UINT local = 0; local < original.MipLevels * planes.PlaneCount; ++local)
		{
			const UINT mip = local % original.MipLevels;
			const UINT plane = local / original.MipLevels;
			const UINT subresource = mip + draw.source.arraySlice * original.MipLevels +
									 plane * original.MipLevels * original.DepthOrArraySize;
			Transition(commands, source.texture, source.releasedState, sourceState, subresource);
			if (multisampled && !depth)
				commands->ResolveSubresource(copy.Get(), mip, source.texture, subresource, format);
			else
			{
				D3D12_TEXTURE_COPY_LOCATION from{}, to{};
				from.pResource = source.texture;
				from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				from.SubresourceIndex = subresource;
				to.pResource = copy.Get();
				to.SubresourceIndex = local;
				commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			}
			Transition(commands, source.texture, sourceState, source.releasedState, subresource);
		}
		Transition(commands, copy.Get(), copyState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = format;
		view.ViewDimension = depth && multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
			view.Texture2D.MipLevels = original.MipLevels;
		auto descriptor = frame->m_sources->GetCPUDescriptorHandleForHeapStart();
		descriptor.ptr += index * sourceStride;
		m_device->CreateShaderResourceView(copy.Get(), &view, descriptor);
	}
	commands->SetGraphicsRootSignature(m_root.Get());
	ID3D12DescriptorHeap* heaps[]{frame->m_sources.Get()};
	commands->SetDescriptorHeaps(1, heaps);
	commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(plan.width), static_cast<float>(plan.height), 0, 1};
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(plan.width), static_cast<LONG>(plan.height)};
	commands->RSSetViewports(1, &viewport);
	commands->RSSetScissorRects(1, &scissor);
	for (UINT eye = 0; eye < outputLayers; ++eye)
	{
		auto descriptor = frame->m_targets->GetCPUDescriptorHandleForHeapStart();
		descriptor.ptr += eye * targetStride;
		D3D12_RENDER_TARGET_VIEW_DESC view{};
		view.Format = target.Format;
		view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		view.Texture2DArray.FirstArraySlice = eye;
		view.Texture2DArray.ArraySize = 1;
		m_device->CreateRenderTargetView(frame->m_texture.Get(), &view, descriptor);
		const float clear[]{0, 0, 0, 1};
		commands->ClearRenderTargetView(descriptor, clear, 0, nullptr);
		commands->OMSetRenderTargets(1, &descriptor, FALSE, nullptr);
		for (std::size_t index = 0; index < plan.draws.size(); ++index)
		{
			if (plan.draws[index].eye != eye)
				continue;
			auto source = frame->m_sources->GetGPUDescriptorHandleForHeapStart();
			source.ptr += index * sourceStride;
			const bool depthMultisample =
				plan.draws[index].constants.control[0] == 2 && sources[index].texture->GetDesc().SampleDesc.Count > 1;
			commands->SetPipelineState(depthMultisample ? m_depthMultisample.Get() : m_pipeline.Get());
			commands->SetGraphicsRootDescriptorTable(0, source);
			commands->SetGraphicsRoot32BitConstants(1, sizeof(LayerConstants) / sizeof(std::uint32_t),
													&plan.draws[index].constants, 0);
			commands->DrawInstanced(3, 1, 0, 0);
		}
	}
	Transition(commands, frame->m_texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
	result = commands->Close();
	if (FAILED(result))
		return result;
	output = std::move(frame);
	return S_OK;
}

HRESULT D3D12Backend::PrepareCapture(const LayerImageDescription& image, D3D12CompositionSource source,
									 std::shared_ptr<D3D12Frame>& output, bool retainMipChain)
{
	if (!SameDevice(source.texture, m_device.Get()))
		return E_INVALIDARG;
	const auto original = source.texture->GetDesc();
	const auto format = source.format == DXGI_FORMAT_UNKNOWN ? original.Format : source.format;
	if (!ColorFormat(original.Format, format))
		return DXGI_ERROR_UNSUPPORTED;
	const auto& rectangle = image.rectangle;
	if (original.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || image.arraySlice >= original.DepthOrArraySize ||
		rectangle.offset.x < 0 || rectangle.offset.y < 0 || rectangle.extent.width <= 0 ||
		rectangle.extent.height <= 0 ||
		static_cast<std::uint64_t>(rectangle.offset.x) + rectangle.extent.width > original.Width ||
		static_cast<std::uint64_t>(rectangle.offset.y) + rectangle.extent.height > original.Height)
		return E_INVALIDARG;
	const bool multisampled = original.SampleDesc.Count > 1;
	if (multisampled)
	{
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
		if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
			!(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE))
			return DXGI_ERROR_UNSUPPORTED;
	}
	auto description = original;
	description.Format = format;
	description.DepthOrArraySize = 1;
	description.MipLevels = retainMipChain ? original.MipLevels : 1;
	if (description.MipLevels == 1)
	{
		description.Width = rectangle.extent.width;
		description.Height = rectangle.extent.height;
	}
	description.SampleDesc = {1, 0};
	description.Alignment = 0;
	description.Flags = D3D12_RESOURCE_FLAG_NONE;
	description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	std::shared_ptr<D3D12Frame> frame;
	auto result = Allocate(description, D3D12_RESOURCE_STATE_COPY_DEST, {}, retainMipChain, frame);
	if (FAILED(result))
		return result;
	frame->m_inputs.emplace_back(source.texture);
	auto* commands = frame->m_commands.Get();
	ComPtr<ID3D12Resource> resolved;
	if (multisampled)
	{
		auto temporary = description;
		temporary.Width = original.Width;
		temporary.Height = original.Height;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		result = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &temporary,
												   D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved));
		if (FAILED(result))
			return result;
		frame->m_inputs.push_back(resolved);
		const UINT subresource = image.arraySlice * original.MipLevels;
		Transition(commands, source.texture, source.releasedState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, subresource);
		commands->ResolveSubresource(resolved.Get(), 0, source.texture, subresource, format);
		Transition(commands, source.texture, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, source.releasedState, subresource);
		Transition(commands, resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
	}
	const D3D12_BOX box{static_cast<UINT>(rectangle.offset.x),
						static_cast<UINT>(rectangle.offset.y),
						0,
						static_cast<UINT>(rectangle.offset.x + rectangle.extent.width),
						static_cast<UINT>(rectangle.offset.y + rectangle.extent.height),
						1};
	for (UINT mip = 0; mip < description.MipLevels; ++mip)
	{
		D3D12_TEXTURE_COPY_LOCATION from{}, to{};
		from.pResource = resolved ? resolved.Get() : source.texture;
		from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		from.SubresourceIndex = resolved ? 0 : mip + image.arraySlice * original.MipLevels;
		to.pResource = frame->m_texture.Get();
		to.SubresourceIndex = mip;
		if (!resolved)
			Transition(commands, source.texture, source.releasedState, D3D12_RESOURCE_STATE_COPY_SOURCE,
					   from.SubresourceIndex);
		commands->CopyTextureRegion(&to, 0, 0, 0, &from, description.MipLevels > 1 ? nullptr : &box);
		if (!resolved)
			Transition(commands, source.texture, D3D12_RESOURCE_STATE_COPY_SOURCE, source.releasedState,
					   from.SubresourceIndex);
	}
	Transition(commands, frame->m_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	result = commands->Close();
	if (FAILED(result))
		return result;
	output = std::move(frame);
	return S_OK;
}

HRESULT D3D12Backend::Submit(const std::shared_ptr<D3D12Frame>& frame)
{
	if (!frame || frame->m_submitted || frame->m_queue.Get() != m_queue.Get())
		return E_INVALIDARG;
	m_pending->frames.push_back(frame);
	frame->m_submitted = true;
	ID3D12CommandList* commands[]{frame->m_commands.Get()};
	m_queue->ExecuteCommandLists(1, commands);
	return m_queue->Signal(frame->m_fence.Get(), D3D12Frame::ReadyValue);
}
} // namespace simxr
