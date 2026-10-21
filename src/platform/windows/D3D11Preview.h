#pragma once

#include "platform/windows/PreviewPipelineTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace simxr::windows
{
class D3D11PreviewContext;
enum class PreviewSurface
{
	Projection,
	Quad
};

struct PreviewRegion
{
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

using PreviewQuadCorners = std::array<std::array<float, 4>, 4>;

// A strong native reader, not an immutable snapshot. Draw synchronously before
// its cache slot is reused by another Copy/Upload on the same surface.
class D3D11PreviewImage final
{
  public:
	D3D11PreviewImage() = default;
	explicit operator bool() const noexcept;

  private:
	friend class D3D11Preview;
	bool Matches(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) const noexcept;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_view;
};

// Externally serialized like the application's immediate context. Construction
// prepares the complete pipeline family before publication. Draw owns all state
// binding and always unbinds its source. A matching scope isolates and restores
// application bindings across all source preparation and drawing.
class D3D11Preview final
{
  public:
	explicit D3D11Preview(ID3D11Device* device);
	D3D11Preview(const D3D11Preview&) = delete;
	D3D11Preview& operator=(const D3D11Preview&) = delete;

	bool IsFor(ID3D11Device* device) const noexcept;
	D3D11PreviewImage Copy(D3D11PreviewContext& scope, PreviewSurface surface, ID3D11Texture2D* source,
						   std::uint32_t slice, DXGI_FORMAT declaredFormat, std::optional<PreviewRegion> region = {});
	D3D11PreviewImage Upload(D3D11PreviewContext& scope, PreviewSurface surface, std::uint32_t width,
							 std::uint32_t height, DXGI_FORMAT format, std::span<const std::uint8_t> pixels,
							 std::uint32_t rowPitch);
	bool DrawProjection(D3D11PreviewContext& scope, const D3D11PreviewImage& image, ID3D11RenderTargetView* target,
						const D3D11_VIEWPORT& viewport, PreviewChannels channels = PreviewChannels::All);
	bool DrawQuad(D3D11PreviewContext& scope, const D3D11PreviewImage& image, ID3D11RenderTargetView* target,
				  const D3D11_VIEWPORT& viewport, const PreviewQuadCorners& corners, PreviewAlpha alpha,
				  PreviewChannels channels = PreviewChannels::All);

  private:
	using BlendState = Microsoft::WRL::ComPtr<ID3D11BlendState>;
	struct Cache
	{
		std::array<D3D11PreviewImage, 2> images;
		std::size_t next = 0;
	};

	D3D11PreviewImage CreateImage(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);
	D3D11PreviewImage Acquire(Cache& cache, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);
	Cache& SelectCache(PreviewSurface surface);
	bool Owns(const D3D11PreviewImage& image) const noexcept;
	void ValidateScope(const D3D11PreviewContext& scope) const;
	void Bind(const D3D11PreviewImage& image, ID3D11RenderTargetView& target, const D3D11_VIEWPORT& viewport,
			  ID3D11VertexShader& vertex, ID3D11BlendState& blend);
	void DrawAndUnbind();

	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_projectionVertex;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_quadVertex;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_quadConstants;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
	std::array<BlendState, 3> m_projectionBlends;
	std::array<std::array<BlendState, 3>, 3> m_quadBlends;
	Cache m_projectionCache;
	Cache m_quadCache;
	D3D11PreviewImage m_resolve;
};
} // namespace simxr::windows
