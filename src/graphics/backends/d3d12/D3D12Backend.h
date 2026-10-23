#pragma once

#include "graphics/Compositor.h"
#include "graphics/FrameOutput.h"
#include "graphics/FramePool.h"
#include "platform/windows/WindowsLibrary.h"

#include <atomic>
#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <span>
#include <vector>
#include <wrl/client.h>

namespace simxr
{
struct D3D12CompositionSource
{
	ID3D12Resource* texture = nullptr;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_STATES releasedState = D3D12_RESOURCE_STATE_RENDER_TARGET;
};

class D3D12Frame final : public FrameOutput
{
  public:
	~D3D12Frame() override;
	CompletionStatus PollCompletion() const noexcept override;
	bool SourcesUsed() const noexcept
	{
		return m_submitted;
	}
	ID3D12Resource* Texture() const noexcept;
	HANDLE TextureHandle() const noexcept;
	HANDLE FenceHandle() const noexcept;
	static constexpr std::uint64_t ReadyValue = 1;

  private:
	friend class D3D12Backend;
	D3D12Frame(std::shared_ptr<const windows::WindowsLibrary> dxgiLibrary,
			   std::shared_ptr<const windows::WindowsLibrary> runtimeLibrary, std::span<const FrameView> views);
	const std::shared_ptr<const windows::WindowsLibrary> m_dxgiLibrary;
	const std::shared_ptr<const windows::WindowsLibrary> m_runtimeLibrary;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_texture;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commands;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_sources, m_targets;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline, m_depthMultisample;
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_inputs;
	std::shared_ptr<FrameReservation> m_reservation;
	HANDLE m_textureHandle = nullptr, m_fenceHandle = nullptr;
	bool m_submitted = false;
	mutable std::atomic<CompletionStatus> m_completion{CompletionStatus::Pending};
};

// Records the common composition plan on the application's direct queue.
// Every source is restored to its declared release state before submission ends.
class D3D12Backend final
{
  public:
	using Frame = D3D12Frame;
	using Source = D3D12CompositionSource;
	D3D12Backend(ID3D12Device* device, ID3D12CommandQueue* queue);
	~D3D12Backend();
	HRESULT Prepare(const CompositionPlan& plan, std::span<const D3D12CompositionSource> sources,
					std::shared_ptr<D3D12Frame>& output, std::uint32_t outputLayers = 2);
	HRESULT PrepareCapture(const LayerImageDescription& image, D3D12CompositionSource source,
						   std::shared_ptr<D3D12Frame>& output, bool retainMipChain = false);
	HRESULT Submit(const std::shared_ptr<D3D12Frame>& frame);

  private:
	HRESULT Allocate(const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES state,
					 std::span<const FrameView> views, bool retained, std::shared_ptr<D3D12Frame>& output);
	const std::shared_ptr<const windows::WindowsLibrary> m_dxgiLibrary;
	const std::shared_ptr<const windows::WindowsLibrary> m_runtimeLibrary;
	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline, m_depthMultisample;
	FramePool m_frames{8};
	FramePool m_retainedFrames{32};
	// Allocated up front so unconfirmed GPU owners can survive teardown without allocation.
	struct PendingFrames;
	std::unique_ptr<PendingFrames> m_pending;
};
} // namespace simxr
