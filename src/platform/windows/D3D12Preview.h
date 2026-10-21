#pragma once

#include "platform/windows/PreviewCapturePixels.h"
#include "platform/windows/PreviewFrameSchedule.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <windows.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace simxr::windows
{
struct PreviewFencePoint
{
	ID3D12Fence* fence = nullptr;
	std::uint64_t value = 0;
};
enum class PreviewReadbackKind
{
	None,
	Composite,
	Quad
};
enum class PreviewQuadResult
{
	Recorded,
	AlreadyRecorded,
	Unsupported,
	Invalid,
	Capacity
};

struct PreviewReadback
{
	PreviewReadbackKind kind = PreviewReadbackKind::None;
	std::optional<PreviewCaptureLayout> layout;
	bool bgra = true;
	const std::uint8_t* pixels = nullptr;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t rowPitch = 0;
};
struct PreviewCompletion
{
	std::size_t slot = 0;
	std::uint64_t sequence = 0;
	bool presentAccepted = false;
	bool copied = false;
	HRESULT result = S_OK;
};

// One session owns a private queue and joined presentation worker. Application
// recording borrows only the currently admitted slot. Present never runs inside
// the application's xrEndFrame/Present call stack. Normal frames allocate no
// CPU readback; explicit capture admission opts into one fenced CPU copy.
class D3D12Preview final
{
  public:
	D3D12Preview(ID3D12Device* device, HWND window);
	~D3D12Preview();
	D3D12Preview(const D3D12Preview&) = delete;
	D3D12Preview& operator=(const D3D12Preview&) = delete;
	bool Resize(std::uint32_t width, std::uint32_t height);
	std::optional<std::size_t> Begin(bool readback);
	bool Recording() const;
	std::size_t CurrentSlot() const;
	ID3D12GraphicsCommandList* Commands() const;
	ID3D12Resource* Target() const;
	D3D12_CPU_DESCRIPTOR_HANDLE RenderTarget(bool srgb = false) const;
	std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> Sample(ID3D12Resource* source,
													  const D3D12_SHADER_RESOURCE_VIEW_DESC& description);
	PreviewQuadResult CaptureQuad(ID3D12Resource* source, std::uint32_t slice, PreviewCrop crop,
								  D3D12_RESOURCE_STATES sourceState);
	static bool IsPresentationThread() noexcept;
	bool RetainPipeline(ID3D12RootSignature* signature, ID3D12PipelineState* pipeline);
	bool Submit(ID3D12CommandQueue* applicationQueue, PreviewFencePoint ready = {}, PreviewFencePoint released = {});
	void Consume(const std::function<void(const PreviewCompletion&, const PreviewReadback&)>& consumer);
	bool WaitIdle(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
	// The timeout bounds the fence wait, not a blocked driver call during worker join.
	bool Close(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
	HRESULT Failure() const;

  private:
	class State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::windows
