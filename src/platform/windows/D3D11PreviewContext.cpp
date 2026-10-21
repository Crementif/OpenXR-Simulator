#include "platform/windows/D3D11PreviewContext.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simxr::windows
{
namespace
{
template <class Interface, std::size_t Count>
std::array<Interface*, Count> Readers(const std::array<Microsoft::WRL::ComPtr<Interface>, Count>& owners) noexcept
{
	std::array<Interface*, Count> readers{};
	for (std::size_t index = 0; index < Count; ++index)
		readers[index] = owners[index].Get();
	return readers;
}

template <class Interface, std::size_t Count>
void Adopt(std::array<Microsoft::WRL::ComPtr<Interface>, Count>& owners,
		   const std::array<Interface*, Count>& acquired) noexcept
{
	for (std::size_t index = 0; index < Count; ++index)
		owners[index].Attach(acquired[index]);
}
} // namespace

template <class Shader>
template <class Getter>
void D3D11PreviewContext::ShaderState<Shader>::Capture(ID3D11DeviceContext& context, Getter getter) noexcept
{
	std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> acquired{};
	m_instanceCount = static_cast<UINT>(acquired.size());
	(context.*getter)(m_shader.GetAddressOf(), acquired.data(), &m_instanceCount);
	Adopt(m_instances, acquired);
	m_instanceCount = std::min(m_instanceCount, static_cast<UINT>(m_instances.size()));
}

template <class Shader>
template <class Setter>
void D3D11PreviewContext::ShaderState<Shader>::Restore(ID3D11DeviceContext& context, Setter setter) const noexcept
{
	const auto instances = Readers(m_instances);
	(context.*setter)(m_shader.Get(), instances.data(), m_instanceCount);
}

D3D11PreviewContext::D3D11PreviewContext(ID3D11DeviceContext* context)
{
	if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		throw std::invalid_argument("Preview isolation requires an immediate D3D11 context");
	m_context = context;
	m_context.As(&m_context1);
	if (m_context1)
	{
		Microsoft::WRL::ComPtr<ID3D11Device> device;
		m_context->GetDevice(device.GetAddressOf());
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) ||
			!options.ConstantBufferOffsetting)
			m_context1.Reset();
	}
	m_context->IAGetInputLayout(m_inputLayout.GetAddressOf());
	m_context->IAGetPrimitiveTopology(&m_topology);
	m_context->RSGetState(m_rasterizer.GetAddressOf());
	m_viewportCount = static_cast<UINT>(m_viewports.size());
	m_context->RSGetViewports(&m_viewportCount, m_viewports.data());
	m_context->OMGetBlendState(m_blend.GetAddressOf(), m_blendFactor.data(), &m_sampleMask);
	m_context->OMGetDepthStencilState(m_depthState.GetAddressOf(), &m_stencilReference);
	m_vertex.Capture(*m_context.Get(), &ID3D11DeviceContext::VSGetShader);
	m_pixel.Capture(*m_context.Get(), &ID3D11DeviceContext::PSGetShader);
	m_geometry.Capture(*m_context.Get(), &ID3D11DeviceContext::GSGetShader);
	m_hull.Capture(*m_context.Get(), &ID3D11DeviceContext::HSGetShader);
	m_domain.Capture(*m_context.Get(), &ID3D11DeviceContext::DSGetShader);
	m_context->PSGetShaderResources(0, 1, m_pixelSource.GetAddressOf());
	m_context->PSGetSamplers(0, 1, m_pixelSampler.GetAddressOf());
	if (m_context1)
		m_context1->VSGetConstantBuffers1(0, 1, m_vertexConstants.GetAddressOf(), &m_firstConstant, &m_constantCount);
	else
		m_context->VSGetConstantBuffers(0, 1, m_vertexConstants.GetAddressOf());
	m_context->GetPredication(m_predicate.GetAddressOf(), &m_predicateValue);
	CaptureOutputs();
	Isolate();
}

bool D3D11PreviewContext::IsFor(ID3D11DeviceContext* context) const noexcept
{
	return m_context.Get() == context;
}

void D3D11PreviewContext::CaptureOutputs() noexcept
{
	Microsoft::WRL::ComPtr<ID3D11Device> device;
	m_context->GetDevice(device.GetAddressOf());
	if (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1)
		m_unorderedCount = D3D11_1_UAV_SLOT_COUNT;
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
	std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> unorderedViews{};
	m_context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), m_depthTarget.GetAddressOf());
	Adopt(m_targets, targets);
	for (std::size_t index = 0; index < m_targets.size(); ++index)
		if (m_targets[index])
			m_targetCount = static_cast<UINT>(index + 1);
	if (m_targetCount < m_unorderedCount)
		m_context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, m_targetCount,
															 m_unorderedCount - m_targetCount,
															 unorderedViews.data() + m_targetCount);
	Adopt(m_unorderedViews, unorderedViews);
	std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> streamTargets{};
	m_context->SOGetTargets(static_cast<UINT>(streamTargets.size()), streamTargets.data());
	Adopt(m_streamTargets, streamTargets);
}

void D3D11PreviewContext::Isolate() noexcept
{
	m_context->SetPredication(nullptr, FALSE);
	m_context->SOSetTargets(0, nullptr, nullptr);
	m_context->GSSetShader(nullptr, nullptr, 0);
	m_context->HSSetShader(nullptr, nullptr, 0);
	m_context->DSSetShader(nullptr, nullptr, 0);
	std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> empty{};
	m_context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, m_unorderedCount, empty.data(),
														 nullptr);
}

void D3D11PreviewContext::RestoreOutputs() const noexcept
{
	const auto targets = Readers(m_targets);
	const auto unorderedViews = Readers(m_unorderedViews);
	std::array<UINT, D3D11_1_UAV_SLOT_COUNT> preserveCounters;
	preserveCounters.fill((std::numeric_limits<UINT>::max)());
	if (m_targetCount < m_unorderedCount)
	{
		m_context->OMSetRenderTargetsAndUnorderedAccessViews(
			m_targetCount, targets.data(), m_depthTarget.Get(), m_targetCount, m_unorderedCount - m_targetCount,
			unorderedViews.data() + m_targetCount, preserveCounters.data() + m_targetCount);
	}
	else
		m_context->OMSetRenderTargets(m_targetCount, targets.data(), m_depthTarget.Get());
	const auto streamTargets = Readers(m_streamTargets);
	std::array<UINT, D3D11_SO_BUFFER_SLOT_COUNT> appendOffsets;
	appendOffsets.fill((std::numeric_limits<UINT>::max)());
	m_context->SOSetTargets(static_cast<UINT>(streamTargets.size()), streamTargets.data(), appendOffsets.data());
}

D3D11PreviewContext::~D3D11PreviewContext() noexcept
{
	ID3D11ShaderResourceView* empty = nullptr;
	m_context->PSSetShaderResources(0, 1, &empty);
	RestoreOutputs();
	m_context->IASetInputLayout(m_inputLayout.Get());
	m_context->IASetPrimitiveTopology(m_topology);
	m_context->RSSetState(m_rasterizer.Get());
	m_context->RSSetViewports(m_viewportCount, m_viewports.data());
	m_context->OMSetBlendState(m_blend.Get(), m_blendFactor.data(), m_sampleMask);
	m_context->OMSetDepthStencilState(m_depthState.Get(), m_stencilReference);
	m_vertex.Restore(*m_context.Get(), &ID3D11DeviceContext::VSSetShader);
	m_pixel.Restore(*m_context.Get(), &ID3D11DeviceContext::PSSetShader);
	m_geometry.Restore(*m_context.Get(), &ID3D11DeviceContext::GSSetShader);
	m_hull.Restore(*m_context.Get(), &ID3D11DeviceContext::HSSetShader);
	m_domain.Restore(*m_context.Get(), &ID3D11DeviceContext::DSSetShader);
	auto* source = m_pixelSource.Get();
	m_context->PSSetShaderResources(0, 1, &source);
	auto* sampler = m_pixelSampler.Get();
	m_context->PSSetSamplers(0, 1, &sampler);
	auto* constants = m_vertexConstants.Get();
	if (m_context1)
		m_context1->VSSetConstantBuffers1(0, 1, &constants, &m_firstConstant, &m_constantCount);
	else
		m_context->VSSetConstantBuffers(0, 1, &constants);
	m_context->SetPredication(m_predicate.Get(), m_predicateValue);
}
} // namespace simxr::windows
