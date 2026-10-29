#pragma once

#include "graphics/Compositor.h"
#include "graphics/FrameOutput.h"
#include "graphics/FramePool.h"
#include "platform/windows/WindowsLibrary.h"

#include <atomic>
#include <cstdint>
#include <d3d11_4.h>
#include <memory>
#include <span>
#include <vector>
#include <wrl/client.h>

namespace simxr
{
struct D3D11CompositionSource
{
	ID3D11Texture2D* texture = nullptr;
	// Typed swapchain format; required when the application texture is typeless.
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

class D3D11Frame final : public FrameOutput
{
public:
	~D3D11Frame() override;

	CompletionStatus PollCompletion() const noexcept override;

	bool SourcesUsed() const noexcept
	{
		return m_submitted;
	}

	// Borrowed handles remain valid while the frame is retained. Consumers must
	// duplicate handles before transferring ownership to another process.
	HANDLE TextureHandle() const noexcept;
	HANDLE FenceHandle() const noexcept;
	ID3D11Texture2D* Texture() const noexcept;
	DXGI_FORMAT Format() const noexcept;
	static constexpr std::uint64_t ReadyValue = 1;

private:
	friend class D3D11Backend;

	D3D11Frame(std::shared_ptr<const windows::WindowsLibrary> dxgiLibrary,
			   std::shared_ptr<const windows::WindowsLibrary> runtimeLibrary, std::span<const FrameView> views);

	const std::shared_ptr<const windows::WindowsLibrary> m_dxgiLibrary;
	const std::shared_ptr<const windows::WindowsLibrary> m_runtimeLibrary;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
	Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
	Microsoft::WRL::ComPtr<ID3D11CommandList> m_commands;
	DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
	bool m_submitted = false;
	HANDLE m_textureHandle = nullptr;
	HANDLE m_fenceHandle = nullptr;
	std::shared_ptr<FrameReservation> m_reservation;
	mutable std::atomic<CompletionStatus> m_completion{CompletionStatus::Pending};
};

// Native command execution and bounded shared outputs for the common compositor.
// The caller owns source acquisition and consumer GPU lifetime.
class D3D11Backend
{
public:
	using Frame = D3D11Frame;
	using Source = D3D11CompositionSource;
	explicit D3D11Backend(ID3D11Device* device);

	// Produces an immutable, GPU-shared frame. Completion observation never touches
	// the application context. Readers retain the frame until GPU use completes.
	HRESULT Prepare(const CompositionPlan& plan, std::span<const D3D11CompositionSource> sources,
					std::shared_ptr<D3D11Frame>& output, std::uint32_t outputLayers = 2);
	HRESULT PrepareCapture(const LayerImageDescription& image, D3D11CompositionSource source,
						   std::shared_ptr<D3D11Frame>& output, bool retainMipChain = false);
	HRESULT Submit(const std::shared_ptr<D3D11Frame>& frame);

private:
	HRESULT Allocate(std::uint32_t width, std::uint32_t height, std::uint32_t layers, DXGI_FORMAT format,
					 std::span<const FrameView> views, std::shared_ptr<D3D11Frame>& output, std::uint32_t mipLevels = 1,
					 bool retained = false);
	// One acquired texture per plan draw. Array selection comes from the shared
	// plan. Copies/resolves stay on the GPU; application textures need no SRV bind.
	// Output is a private two-slice sRGB target. Serialize with application use
	// of the immediate context.
	HRESULT Record(const CompositionPlan& plan, std::span<const D3D11CompositionSource> sources,
				   ID3D11Texture2D* output, Microsoft::WRL::ComPtr<ID3D11CommandList>& commands);

	const std::shared_ptr<const windows::WindowsLibrary> m_dxgiLibrary;
	const std::shared_ptr<const windows::WindowsLibrary> m_runtimeLibrary;
	FramePool m_frames{8};
	FramePool m_retainedFrames{32};
	std::vector<std::shared_ptr<D3D11Frame>> m_pending;
	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_commands;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_depthMultisample;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depth;
};
} // namespace simxr
