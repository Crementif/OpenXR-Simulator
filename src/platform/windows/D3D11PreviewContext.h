#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>

namespace simxr::windows
{
// Scoped isolation for D3D11Preview and its private targets/copy textures. Saves
// only the bindings this draw scope changes, including SO/UAV counter-preserving
// restoration and Context1's VS b0 range. Other shader resources and CS state
// cannot alias private preview resources and are left untouched. Active queries
// remain active; this is not a complete device-context or query snapshot.
class D3D11PreviewContext final
{
  public:
	explicit D3D11PreviewContext(ID3D11DeviceContext* context);
	~D3D11PreviewContext() noexcept;
	D3D11PreviewContext(const D3D11PreviewContext&) = delete;
	D3D11PreviewContext& operator=(const D3D11PreviewContext&) = delete;

  private:
	friend class D3D11Preview;
	bool IsFor(ID3D11DeviceContext* context) const noexcept;
	template <class Shader> class ShaderState
	{
	  public:
		template <class Getter> void Capture(ID3D11DeviceContext& context, Getter getter) noexcept;
		template <class Setter> void Restore(ID3D11DeviceContext& context, Setter setter) const noexcept;

	  private:
		Microsoft::WRL::ComPtr<Shader> m_shader;
		std::array<Microsoft::WRL::ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> m_instances;
		UINT m_instanceCount = 0;
	};

	void CaptureOutputs() noexcept;
	void Isolate() noexcept;
	void RestoreOutputs() const noexcept;

	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context1;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
	D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
	std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> m_viewports{};
	UINT m_viewportCount = 0;
	std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> m_targets;
	UINT m_targetCount = 0;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthTarget;
	std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> m_unorderedViews;
	UINT m_unorderedCount = D3D11_PS_CS_UAV_REGISTER_COUNT;
	std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, D3D11_SO_BUFFER_SLOT_COUNT> m_streamTargets;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
	std::array<FLOAT, 4> m_blendFactor{};
	UINT m_sampleMask = 0;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
	UINT m_stencilReference = 0;
	ShaderState<ID3D11VertexShader> m_vertex;
	ShaderState<ID3D11PixelShader> m_pixel;
	ShaderState<ID3D11GeometryShader> m_geometry;
	ShaderState<ID3D11HullShader> m_hull;
	ShaderState<ID3D11DomainShader> m_domain;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pixelSource;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pixelSampler;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexConstants;
	UINT m_firstConstant = 0;
	UINT m_constantCount = 0;
	Microsoft::WRL::ComPtr<ID3D11Predicate> m_predicate;
	BOOL m_predicateValue = FALSE;
};
} // namespace simxr::windows
