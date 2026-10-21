#pragma once

#include "platform/windows/PreviewPipelineTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>

namespace simxr::windows
{
class D3D12Preview;

// A binding owns its native readers independently of the pipeline catalog. Retain
// it before source barriers; Bind then records only already-retained resources.
class PreviewPipelineBinding final
{
  public:
	std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> SourceView(const D3D12_RESOURCE_DESC& source,
															  std::uint32_t slice) const noexcept;
	bool Retain(D3D12Preview& preview) const;
	void Bind(ID3D12GraphicsCommandList& commands) const noexcept;

  private:
	friend class D3D12PreviewPipelines;
	enum class Pass
	{
		Projection,
		Quad
	};
	PreviewPipelineBinding(Microsoft::WRL::ComPtr<ID3D12RootSignature> signature,
						   Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline, Pass pass);
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_signature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
	Pass m_pass;
};

// Construction publishes the complete format/alpha/channel family or throws.
// Size-independent immutable pipelines survive presentation target resizes.
class D3D12PreviewPipelines final
{
  public:
	static constexpr std::uint32_t BlitConstantCount = 8;
	static constexpr std::uint32_t QuadConstantCount = 28;
	explicit D3D12PreviewPipelines(ID3D12Device* device);
	D3D12PreviewPipelines(const D3D12PreviewPipelines&) = delete;
	D3D12PreviewPipelines& operator=(const D3D12PreviewPipelines&) = delete;

	PreviewPipelineBinding Blit(PreviewChannels channels = PreviewChannels::All) const;
	PreviewPipelineBinding Quad(PreviewAlpha alpha, PreviewChannels channels = PreviewChannels::All) const;

  private:
	using Pipeline = Microsoft::WRL::ComPtr<ID3D12PipelineState>;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_blitSignature;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_quadSignature;
	std::array<Pipeline, 3> m_blitPipelines;
	std::array<std::array<Pipeline, 3>, 3> m_quadPipelines;
};
} // namespace simxr::windows
