#include "platform/windows/D3D12Preview.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cassert>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace simxr::windows
{
using Microsoft::WRL::ComPtr;

class D3D12Preview::State
{
	static constexpr std::size_t SourcesPerFrame = 64;
	static constexpr std::size_t PipelinesPerFrame = 16;
	static constexpr std::size_t OwnerCapacity = 4;
	static constexpr DXGI_FORMAT TargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	static constexpr DXGI_FORMAT SrgbFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

	class Preservation
	{
		struct Storage
		{
			std::mutex mutex;
			std::array<bool, OwnerCapacity> reserved{};
			std::array<std::unique_ptr<State>, OwnerCapacity> owners;
		};
		static Storage& Owners()
		{
			// Unknown native completion is deliberately retained for process
			// lifetime. Admission reserves this cell before creating any device work.
			static auto* storage = new Storage;
			return *storage;
		}
		const std::size_t m_index;
		static std::size_t Reserve()
		{
			auto& storage = Owners();
			std::lock_guard lock(storage.mutex);
			for (std::size_t index = 0; index < OwnerCapacity; ++index)
				if (!storage.reserved[index])
				{
					storage.reserved[index] = true;
					return index;
				}
			throw std::runtime_error("D3D12 preview native-owner capacity exhausted");
		}

	  public:
		Preservation() : m_index(Reserve()) {}
		~Preservation()
		{
			auto& storage = Owners();
			std::lock_guard lock(storage.mutex);
			storage.reserved[m_index] = false;
		}
		void Preserve(std::unique_ptr<State> owner) noexcept
		{
			auto& storage = Owners();
			std::lock_guard lock(storage.mutex);
			assert(!storage.owners[m_index]);
			storage.owners[m_index].swap(owner);
		}
	};

	class Event
	{
		HANDLE m_handle = nullptr;

	  public:
		Event() : m_handle(CreateEventW(nullptr, FALSE, FALSE, nullptr))
		{
			if (!m_handle)
				throw std::runtime_error("D3D12 preview fence event creation failed");
		}
		~Event()
		{
			CloseHandle(m_handle);
		}
		HANDLE Get() const
		{
			return m_handle;
		}
	};

	struct Slot
	{
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> composition;
		ComPtr<ID3D12GraphicsCommandList> presentation;
		ComPtr<ID3D12Resource> target;
		ComPtr<ID3D12Resource> readback;
		ComPtr<ID3D12Resource> quadReadback;
		std::optional<PreviewCaptureLayout> quadLayout;
		bool quadBgra = true;
		std::array<ComPtr<ID3D12Resource>, SourcesPerFrame> sources;
		std::array<ComPtr<ID3D12RootSignature>, PipelinesPerFrame> signatures;
		std::array<ComPtr<ID3D12PipelineState>, PipelinesPerFrame> pipelines;
		std::size_t sourceCount = 0;
		std::size_t pipelineCount = 0;
		ComPtr<ID3D12Fence> ready;
		std::uint64_t readyValue = 0;
		ComPtr<ID3D12Fence> released;
		std::uint64_t releasedValue = 0;
		bool wantsReadback = false;
		bool presentAccepted = false;
		bool copied = false;
		bool cancelled = false;
		bool bridgeUnreleased = false;
		HRESULT result = S_OK;
		bool consuming = false;
	};

	Preservation m_preservation;
	ComPtr<ID3D12Device> m_device;
	const HWND m_window;
	ComPtr<ID3D12CommandQueue> m_queue;
	ComPtr<ID3D12Fence> m_fence;
	ComPtr<ID3D12Fence> m_crossFence;
	std::uint64_t m_crossValue = 0;
	Event m_event;
	ComPtr<IDXGISwapChain3> m_swapchain;
	std::array<ComPtr<ID3D12Resource>, PreviewFrameSchedule::Capacity> m_backbuffers;
	ComPtr<ID3D12DescriptorHeap> m_renderTargets;
	ComPtr<ID3D12DescriptorHeap> m_sources;
	UINT m_renderTargetStride = 0;
	UINT m_sourceStride = 0;
	std::array<Slot, PreviewFrameSchedule::Capacity> m_slots;
	PreviewFrameSchedule m_schedule;
	std::optional<PreviewExtent> m_extent;
	mutable std::mutex m_mutex;
	std::mutex m_waitMutex;
	std::mutex m_closeMutex;
	std::condition_variable m_changed;
	std::array<std::size_t, PreviewFrameSchedule::Capacity> m_jobs{};
	std::size_t m_jobHead = 0;
	std::size_t m_jobCount = 0;
	bool m_stop = false;
	HRESULT m_failure = S_OK;
	std::thread m_worker;

	static bool& PresentationThreadFlag() noexcept
	{
		static thread_local bool active = false;
		return active;
	}
	static void Check(HRESULT result)
	{
		if (FAILED(result))
			throw std::runtime_error("D3D12 preview native acquisition failed: " + std::to_string(result));
	}
	static void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
						   D3D12_RESOURCE_STATES after)
	{
		if (before == after)
			return;
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		commands->ResourceBarrier(1, &barrier);
	}
	void Observe()
	{
		const auto completed = m_fence->GetCompletedValue();
		if (completed == std::numeric_limits<std::uint64_t>::max())
			m_failure = DXGI_ERROR_DEVICE_REMOVED;
		else
			m_schedule.Observe(completed);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE Rtv(std::size_t index, bool srgb) const
	{
		auto handle = m_renderTargets->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += (index * 2 + static_cast<std::size_t>(srgb)) * m_renderTargetStride;
		return handle;
	}
	HRESULT Present(std::size_t index) noexcept
	{
		auto& slot = m_slots[index];
		bool readyAccepted = !slot.ready;
		const auto recordAndExecute = [&]() -> HRESULT
		{
			// This also precedes cancellation: advancing a shared timeline
			// release must never overtake the producer's lower ready value.
			if (slot.ready)
			{
				const auto ready = m_queue->Wait(slot.ready.Get(), slot.readyValue);
				if (FAILED(ready))
					return ready;
				readyAccepted = true;
			}
			if (slot.cancelled)
				return E_ABORT;
			auto* backbuffer = m_backbuffers[m_swapchain->GetCurrentBackBufferIndex()].Get();
			auto* commands = slot.presentation.Get();
			auto result = commands->Reset(slot.allocator.Get(), nullptr);
			if (FAILED(result))
				return result;
			Transition(commands, slot.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
					   D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(commands, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
			commands->CopyResource(backbuffer, slot.target.Get());
			Transition(commands, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
			if (slot.wantsReadback)
			{
				D3D12_TEXTURE_COPY_LOCATION destination{};
				destination.pResource = slot.readback.Get();
				destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				destination.PlacedFootprint.Footprint.Format = TargetFormat;
				destination.PlacedFootprint.Footprint.Width = m_extent->width;
				destination.PlacedFootprint.Footprint.Height = m_extent->height;
				destination.PlacedFootprint.Footprint.Depth = 1;
				destination.PlacedFootprint.Footprint.RowPitch = m_extent->rowPitch;
				D3D12_TEXTURE_COPY_LOCATION source{};
				source.pResource = slot.target.Get();
				source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
			}
			Transition(commands, slot.target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			result = commands->Close();
			if (FAILED(result))
				return result;
			ID3D12CommandList* lists[] = {slot.composition.Get(), commands};
			m_queue->ExecuteCommandLists(2, lists);
			slot.copied = true;
			// A private thread prevents hook reentry into the host's Present
			// stack. A busy desktop drops the optional mirror presentation.
			result = m_swapchain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
			slot.presentAccepted = result == S_OK;
			return result == DXGI_ERROR_WAS_STILL_DRAWING ? S_OK : result;
		};
		HRESULT result;
		try
		{
			result = recordAndExecute();
		}
		catch (...)
		{
			result = E_FAIL;
		}

		// Every accepted host ticket has an actual queue retirement marker,
		// including failures BEFORE Execute. This releases a healthy producer's
		// queued source-reuse wait without executing an invalid composition.
		// After Execute we still attempt both signals even if Present or the
		// external release signal failed. A rejected marker stays uncompleted
		// and preserves native ownership; there is no CPU fence signal fallback.
		if (slot.released && !readyAccepted)
			slot.bridgeUnreleased = true;
		if (slot.released && readyAccepted)
		{
			const auto released = m_queue->Signal(slot.released.Get(), slot.releasedValue);
			if (FAILED(released))
				slot.bridgeUnreleased = true;
			if (SUCCEEDED(result) && FAILED(released))
				result = released;
		}
		const auto retirement = m_queue->Signal(m_fence.Get(), m_schedule.Fence(index));
		if (SUCCEEDED(result) && FAILED(retirement))
			result = retirement;
		slot.result = result;
		return result;
	}
	void Run() noexcept
	{
		for (;;)
		{
			std::size_t index;
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait(lock,
							   [&]
							   {
								   return m_stop || m_jobCount != 0;
							   });
				if (m_jobCount == 0)
					return;
				index = m_jobs[m_jobHead];
				m_jobHead = (m_jobHead + 1) % m_jobs.size();
				--m_jobCount;
			}
			const auto result = Present(index);
			{
				std::lock_guard lock(m_mutex);
				if (FAILED(result))
					m_failure = result;
				m_schedule.Submitted(index);
				m_changed.notify_all();
			}
		}
	}
	void Release(std::size_t index)
	{
		std::lock_guard lock(m_mutex);
		m_slots[index].consuming = false;
		m_schedule.Release(index);
	}

  public:
	static bool IsPresentationThread() noexcept
	{
		return PresentationThreadFlag();
	}
	State(ID3D12Device* device, HWND window) : m_device(device), m_window(window)
	{
		if (!device || !window)
			throw std::invalid_argument("D3D12 preview requires a device and window");
		D3D12_COMMAND_QUEUE_DESC queue{};
		queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		Check(device->CreateCommandQueue(&queue, IID_PPV_ARGS(m_queue.GetAddressOf())));
		Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
		Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_crossFence.GetAddressOf())));
		D3D12_DESCRIPTOR_HEAP_DESC heap{};
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap.NumDescriptors = PreviewFrameSchedule::Capacity * 2;
		Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(m_renderTargets.GetAddressOf())));
		m_renderTargetStride = device->GetDescriptorHandleIncrementSize(heap.Type);
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap.NumDescriptors = PreviewFrameSchedule::Capacity * SourcesPerFrame;
		heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(m_sources.GetAddressOf())));
		m_sourceStride = device->GetDescriptorHandleIncrementSize(heap.Type);
		for (auto& slot : m_slots)
		{
			Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
												 IID_PPV_ARGS(slot.allocator.GetAddressOf())));
			Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
											IID_PPV_ARGS(slot.composition.GetAddressOf())));
			Check(slot.composition->Close());
			Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
											IID_PPV_ARGS(slot.presentation.GetAddressOf())));
			Check(slot.presentation->Close());
		}
		m_worker = std::thread(
			[this]
			{
				PresentationThreadFlag() = true;
				Run();
				PresentationThreadFlag() = false;
			});
	}
	void Preserve(std::unique_ptr<State> state) noexcept
	{
		m_preservation.Preserve(std::move(state));
	}
	HRESULT Failure() const
	{
		std::lock_guard lock(m_mutex);
		return m_failure;
	}
	bool Resize(std::uint32_t width, std::uint32_t height)
	{
		const auto extent = PreviewExtent::Make(width, height);
		if (!extent)
			return false;
		std::lock_guard lock(m_mutex);
		if (m_stop || FAILED(m_failure))
			return false;
		if (m_extent && m_extent->width == width && m_extent->height == height)
			return true;
		Observe();
		for (std::size_t index = 0; index < m_slots.size(); ++index)
			if (m_schedule.State(index) != PreviewFrameSchedule::Phase::Free)
				return false;
		try
		{
			std::array<ComPtr<ID3D12Resource>, PreviewFrameSchedule::Capacity> targets;
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = width;
			description.Height = height;
			description.DepthOrArraySize = 1;
			description.MipLevels = 1;
			description.SampleDesc.Count = 1;
			description.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_HEAP_PROPERTIES memory{};
			memory.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = TargetFormat;
			clear.Color[3] = 1;
			for (auto& target : targets)
				Check(m_device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &description,
														D3D12_RESOURCE_STATE_COMMON, &clear,
														IID_PPV_ARGS(target.GetAddressOf())));
			for (auto& backbuffer : m_backbuffers)
				backbuffer.Reset();
			if (m_swapchain)
				Check(m_swapchain->ResizeBuffers(PreviewFrameSchedule::Capacity, width, height, TargetFormat, 0));
			else
			{
				ComPtr<IDXGIFactory2> factory;
				Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())));
				DXGI_SWAP_CHAIN_DESC1 swapchain{};
				swapchain.Width = width;
				swapchain.Height = height;
				swapchain.Format = TargetFormat;
				swapchain.SampleDesc.Count = 1;
				swapchain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				swapchain.BufferCount = PreviewFrameSchedule::Capacity;
				swapchain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				swapchain.Scaling = DXGI_SCALING_STRETCH;
				ComPtr<IDXGISwapChain1> created;
				Check(factory->CreateSwapChainForHwnd(m_queue.Get(), m_window, &swapchain, nullptr, nullptr,
													  created.GetAddressOf()));
				Check(created.As(&m_swapchain));
				factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
			}
			for (std::size_t index = 0; index < m_slots.size(); ++index)
			{
				Check(m_swapchain->GetBuffer(static_cast<UINT>(index),
											 IID_PPV_ARGS(m_backbuffers[index].GetAddressOf())));
				m_slots[index].target = std::move(targets[index]);
				m_slots[index].readback.Reset();
				D3D12_RENDER_TARGET_VIEW_DESC view{};
				view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				view.Format = TargetFormat;
				m_device->CreateRenderTargetView(m_slots[index].target.Get(), &view, Rtv(index, false));
				view.Format = SrgbFormat;
				m_device->CreateRenderTargetView(m_slots[index].target.Get(), &view, Rtv(index, true));
			}
			m_extent = extent;
			return true;
		}
		catch (...)
		{
			// A partially resized swapchain is terminal; never fall back to stale
			// resources or the former CPU presentation path.
			m_failure = E_FAIL;
			return false;
		}
	}
	std::optional<std::size_t> Begin(bool readback)
	{
		std::lock_guard lock(m_mutex);
		if (!m_extent || m_stop || FAILED(m_failure))
			return {};
		if (m_schedule.Recording())
			return m_schedule.Recording();
		const auto index = m_schedule.Begin();
		if (!index)
			return {};
		auto& slot = m_slots[*index];
		try
		{
			if (readback && !slot.readback)
			{
				D3D12_RESOURCE_DESC description{};
				description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				description.Width = m_extent->readbackBytes;
				description.Height = 1;
				description.DepthOrArraySize = 1;
				description.MipLevels = 1;
				description.SampleDesc.Count = 1;
				description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				D3D12_HEAP_PROPERTIES memory{};
				memory.Type = D3D12_HEAP_TYPE_READBACK;
				Check(m_device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &description,
														D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
														IID_PPV_ARGS(slot.readback.GetAddressOf())));
			}
			Check(slot.allocator->Reset());
			Check(slot.composition->Reset(slot.allocator.Get(), nullptr));
			Transition(slot.composition.Get(), slot.target.Get(), D3D12_RESOURCE_STATE_COMMON,
					   D3D12_RESOURCE_STATE_RENDER_TARGET);
			const auto target = Rtv(*index, false);
			const float black[4] = {0, 0, 0, 1};
			slot.composition->ClearRenderTargetView(target, black, 0, nullptr);
			ID3D12DescriptorHeap* heaps[] = {m_sources.Get()};
			slot.composition->SetDescriptorHeaps(1, heaps);
			slot.composition->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			slot.quadLayout.reset();
			slot.wantsReadback = readback;
			slot.sourceCount = 0;
			slot.pipelineCount = 0;
			slot.presentAccepted = false;
			slot.copied = false;
			slot.cancelled = false;
			slot.bridgeUnreleased = false;
			slot.result = S_OK;
			return index;
		}
		catch (...)
		{
			m_schedule.CancelRecording();
			return {};
		}
	}
	std::optional<std::size_t> Recording() const
	{
		std::lock_guard lock(m_mutex);
		return m_schedule.Recording();
	}
	ID3D12GraphicsCommandList* Commands() const
	{
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		return index ? m_slots[*index].composition.Get() : nullptr;
	}
	ID3D12Resource* Target() const
	{
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		return index ? m_slots[*index].target.Get() : (m_extent ? m_slots[0].target.Get() : nullptr);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE RenderTarget(bool srgb) const
	{
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		return index ? Rtv(*index, srgb) : D3D12_CPU_DESCRIPTOR_HANDLE{};
	}
	std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> Sample(ID3D12Resource* source,
													  const D3D12_SHADER_RESOURCE_VIEW_DESC& description)
	{
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		if (!index || !source)
			return {};
		auto& slot = m_slots[*index];
		if (slot.sourceCount == slot.sources.size())
			return {};
		const auto descriptor = *index * SourcesPerFrame + slot.sourceCount;
		auto cpu = m_sources->GetCPUDescriptorHandleForHeapStart();
		auto gpu = m_sources->GetGPUDescriptorHandleForHeapStart();
		cpu.ptr += descriptor * m_sourceStride;
		gpu.ptr += descriptor * m_sourceStride;
		slot.sources[slot.sourceCount++] = source;
		m_device->CreateShaderResourceView(source, &description, cpu);
		return gpu;
	}
	PreviewQuadResult CaptureQuad(ID3D12Resource* source, std::uint32_t slice, PreviewCrop crop,
								  D3D12_RESOURCE_STATES sourceState)
	{
		ComPtr<ID3D12Resource> retired;
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		if (!index || !source)
			return PreviewQuadResult::Invalid;
		auto& slot = m_slots[*index];
		if (slot.quadLayout)
			return PreviewQuadResult::AlreadyRecorded;
		const auto description = source->GetDesc();
		const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
						  description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
						  description.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
		const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
						  description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
						  description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
		if ((!bgra && !rgba) || description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			description.SampleDesc.Count != 1)
			return PreviewQuadResult::Unsupported;
		if (description.Width > std::numeric_limits<std::uint32_t>::max())
			return PreviewQuadResult::Invalid;
		const auto layout =
			PreviewCaptureLayout::Make(static_cast<std::uint32_t>(description.Width), description.Height,
									   description.MipLevels, description.DepthOrArraySize, slice, crop);
		if (!layout)
			return PreviewQuadResult::Invalid;
		if (slot.sourceCount == slot.sources.size())
			return PreviewQuadResult::Capacity;
		ComPtr<ID3D12Resource> readback;
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = layout->Extent().readbackBytes;
		buffer.Height = 1;
		buffer.DepthOrArraySize = 1;
		buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_HEAP_PROPERTIES memory{};
		memory.Type = D3D12_HEAP_TYPE_READBACK;
		if (FAILED(m_device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &buffer,
													 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
													 IID_PPV_ARGS(readback.GetAddressOf()))))
			return PreviewQuadResult::Capacity;
		// All acquisition and validation precedes recording. Only the first
		// eligible quad is copied, bounding one isolated result per frame.
		slot.sources[slot.sourceCount++] = source;
		auto* commands = slot.composition.Get();
		Transition(commands, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = readback.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		destination.PlacedFootprint.Footprint.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
		destination.PlacedFootprint.Footprint.Width = crop.width;
		destination.PlacedFootprint.Footprint.Height = crop.height;
		destination.PlacedFootprint.Footprint.Depth = 1;
		destination.PlacedFootprint.Footprint.RowPitch = layout->Extent().rowPitch;
		D3D12_TEXTURE_COPY_LOCATION origin{};
		origin.pResource = source;
		origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		origin.SubresourceIndex = layout->Subresource();
		const D3D12_BOX region{crop.x, crop.y, 0, crop.x + crop.width, crop.y + crop.height, 1};
		commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, &region);
		Transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
		retired.Swap(slot.quadReadback);
		slot.quadReadback = std::move(readback);
		slot.quadLayout = layout;
		slot.quadBgra = bgra;
		return PreviewQuadResult::Recorded;
	}
	bool RetainPipeline(ID3D12RootSignature* signature, ID3D12PipelineState* pipeline)
	{
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		if (!index || !signature || !pipeline)
			return false;
		auto& slot = m_slots[*index];
		for (std::size_t item = 0; item < slot.pipelineCount; ++item)
			if (slot.signatures[item].Get() == signature && slot.pipelines[item].Get() == pipeline)
				return true;
		if (slot.pipelineCount == slot.pipelines.size())
			return false;
		slot.signatures[slot.pipelineCount] = signature;
		slot.pipelines[slot.pipelineCount++] = pipeline;
		return true;
	}
	bool Submit(ID3D12CommandQueue* applicationQueue, PreviewFencePoint ready, PreviewFencePoint released)
	{
		std::lock_guard lock(m_mutex);
		const auto index = m_schedule.Recording();
		if (!index || m_stop || FAILED(m_failure))
			return false;
		auto& slot = m_slots[*index];
		const auto closed = slot.composition->Close();
		if (FAILED(closed))
		{
			m_failure = closed;
			return false;
		}
		if (m_crossValue == std::numeric_limits<std::uint64_t>::max() - 1)
		{
			m_failure = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
			return false;
		}
		slot.ready = ready.fence;
		slot.readyValue = ready.value;
		slot.released = released.fence;
		slot.releasedValue = released.value;
		if (applicationQueue)
		{
			const auto result = applicationQueue->Signal(m_crossFence.Get(), ++m_crossValue);
			if (FAILED(result))
			{
				m_failure = result;
				return false;
			}
			slot.ready = m_crossFence;
			slot.readyValue = m_crossValue;
		}
		const auto sequence = m_schedule.Queue();
		if (!sequence)
			return false;
		m_jobs[(m_jobHead + m_jobCount) % m_jobs.size()] = *index;
		++m_jobCount;
		// Queue waits preserve the application's source reuse contract without
		// making xrEndFrame wait for the private presentation worker or GPU.
		if (applicationQueue)
		{
			const auto result = applicationQueue->Wait(m_fence.Get(), *sequence);
			if (FAILED(result))
			{
				slot.cancelled = true;
				m_failure = result;
			}
		}
		m_changed.notify_all();
		return SUCCEEDED(m_failure);
	}
	void Consume(const std::function<void(const PreviewCompletion&, const PreviewReadback&)>& consumer)
	{
		for (std::size_t index = 0; index < m_slots.size(); ++index)
		{
			PreviewCompletion completion;
			PreviewReadback readback;
			ComPtr<ID3D12Resource> buffer;
			ComPtr<ID3D12Resource> quadBuffer;
			PreviewReadback quad;
			std::array<ComPtr<ID3D12Resource>, SourcesPerFrame> sources;
			std::array<ComPtr<ID3D12RootSignature>, PipelinesPerFrame> signatures;
			std::array<ComPtr<ID3D12PipelineState>, PipelinesPerFrame> pipelines;
			ComPtr<ID3D12Fence> ready;
			ComPtr<ID3D12Fence> released;
			{
				std::lock_guard lock(m_mutex);
				Observe();
				auto& slot = m_slots[index];
				if (m_schedule.State(index) != PreviewFrameSchedule::Phase::Complete || slot.consuming ||
					slot.bridgeUnreleased)
					continue;
				slot.consuming = true;
				completion = {index, m_schedule.Fence(index), slot.presentAccepted, slot.copied, slot.result};
				if (slot.wantsReadback)
				{
					if (slot.copied)
						buffer = slot.readback;
					readback.kind = PreviewReadbackKind::Composite;
					readback.width = m_extent->width;
					readback.height = m_extent->height;
					readback.rowPitch = m_extent->rowPitch;
				}
				if (slot.quadLayout)
				{
					if (slot.copied)
						quadBuffer.Swap(slot.quadReadback);
					quad.kind = PreviewReadbackKind::Quad;
					quad.layout = slot.quadLayout;
					quad.bgra = slot.quadBgra;
					quad.width = quad.layout->Extent().width;
					quad.height = quad.layout->Extent().height;
					quad.rowPitch = quad.layout->Extent().rowPitch;
				}
				sources.swap(slot.sources);
				signatures.swap(slot.signatures);
				pipelines.swap(slot.pipelines);
				ready.Swap(slot.ready);
				released.Swap(slot.released);
			}
			struct CompletionRelease
			{
				State& owner;
				std::size_t index;
				~CompletionRelease()
				{
					owner.Release(index);
				}
			} release{*this, index};
			struct Mapping
			{
				ID3D12Resource* buffer;
				bool mapped = false;
				~Mapping()
				{
					if (mapped)
					{
						const D3D12_RANGE written{0, 0};
						buffer->Unmap(0, &written);
					}
				}
			} mapping{buffer.Get()};
			if (quad.kind == PreviewReadbackKind::Quad)
			{
				Mapping quadMapping{quadBuffer.Get()};
				const D3D12_RANGE range{0, static_cast<SIZE_T>(quad.layout->Extent().readbackBytes)};
				void* pixels = nullptr;
				if (quadBuffer && SUCCEEDED(quadBuffer->Map(0, &range, &pixels)) && pixels)
				{
					quadMapping.mapped = true;
					quad.pixels = static_cast<const std::uint8_t*>(pixels);
				}
				consumer(completion, quad);
			}
			if (buffer)
			{
				const D3D12_RANGE range{0, static_cast<SIZE_T>(readback.rowPitch) * readback.height};
				void* pixels = nullptr;
				if (SUCCEEDED(buffer->Map(0, &range, &pixels)) && pixels)
				{
					mapping.mapped = true;
					readback.pixels = static_cast<const std::uint8_t*>(pixels);
				}
			}
			// No owner mutex is held while CPU capture/diagnostics or native
			// last-reader retirement executes.
			consumer(completion, readback);
		}
	}
	bool WaitIdle(std::chrono::milliseconds timeout)
	{
		std::lock_guard waiter(m_waitMutex);
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		std::uint64_t sequence;
		{
			std::unique_lock lock(m_mutex);
			if (!m_changed.wait_until(lock, deadline,
									  [&]
									  {
										  for (std::size_t index = 0; index < m_slots.size(); ++index)
											  if (m_schedule.State(index) == PreviewFrameSchedule::Phase::Queued)
												  return false;
										  return true;
									  }))
				return false;
			for (const auto& slot : m_slots)
				if (slot.bridgeUnreleased)
					return false;
			sequence = m_schedule.LastQueued();
		}
		const auto completed = m_fence->GetCompletedValue();
		if (completed == std::numeric_limits<std::uint64_t>::max())
			return false;
		if (completed >= sequence)
			return true;
		if (FAILED(m_fence->SetEventOnCompletion(sequence, m_event.Get())))
			return false;
		const auto remaining =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		if (remaining.count() <= 0 ||
			WaitForSingleObject(m_event.Get(), static_cast<DWORD>(remaining.count())) != WAIT_OBJECT_0)
			return false;
		const auto observed = m_fence->GetCompletedValue();
		return observed != std::numeric_limits<std::uint64_t>::max() && observed >= sequence;
	}
	bool Close(std::chrono::milliseconds timeout)
	{
		std::lock_guard lifecycle(m_closeMutex);
		{
			std::lock_guard lock(m_mutex);
			m_schedule.CloseAdmission();
			m_stop = true;
			m_changed.notify_all();
		}
		if (m_worker.joinable())
			m_worker.join();
		return WaitIdle(timeout);
	}
};

D3D12Preview::D3D12Preview(ID3D12Device* device, HWND window) : m_state(std::make_unique<State>(device, window)) {}
D3D12Preview::~D3D12Preview()
{
	if (m_state && !m_state->Close(std::chrono::milliseconds(1000)))
	{
		OutputDebugStringA("[SimXR] D3D12 preview completion unknown; preserving bounded native owner\n");
		auto* state = m_state.get();
		state->Preserve(std::move(m_state));
	}
}
bool D3D12Preview::Resize(std::uint32_t width, std::uint32_t height)
{
	return m_state->Resize(width, height);
}
std::optional<std::size_t> D3D12Preview::Begin(bool readback)
{
	return m_state->Begin(readback);
}
bool D3D12Preview::Recording() const
{
	return m_state->Recording().has_value();
}
std::size_t D3D12Preview::CurrentSlot() const
{
	return m_state->Recording().value_or(0);
}
ID3D12GraphicsCommandList* D3D12Preview::Commands() const
{
	return m_state->Commands();
}
ID3D12Resource* D3D12Preview::Target() const
{
	return m_state->Target();
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Preview::RenderTarget(bool srgb) const
{
	return m_state->RenderTarget(srgb);
}
std::optional<D3D12_GPU_DESCRIPTOR_HANDLE> D3D12Preview::Sample(ID3D12Resource* source,
																const D3D12_SHADER_RESOURCE_VIEW_DESC& description)
{
	return m_state->Sample(source, description);
}
bool D3D12Preview::IsPresentationThread() noexcept
{
	return State::IsPresentationThread();
}
PreviewQuadResult D3D12Preview::CaptureQuad(ID3D12Resource* source, std::uint32_t slice, PreviewCrop crop,
											D3D12_RESOURCE_STATES sourceState)
{
	return m_state->CaptureQuad(source, slice, crop, sourceState);
}
bool D3D12Preview::RetainPipeline(ID3D12RootSignature* signature, ID3D12PipelineState* pipeline)
{
	return m_state->RetainPipeline(signature, pipeline);
}
bool D3D12Preview::Submit(ID3D12CommandQueue* queue, PreviewFencePoint ready, PreviewFencePoint released)
{
	return m_state->Submit(queue, ready, released);
}
void D3D12Preview::Consume(const std::function<void(const PreviewCompletion&, const PreviewReadback&)>& consumer)
{
	m_state->Consume(consumer);
}
bool D3D12Preview::WaitIdle(std::chrono::milliseconds timeout)
{
	return m_state->WaitIdle(timeout);
}
bool D3D12Preview::Close(std::chrono::milliseconds timeout)
{
	return m_state->Close(timeout);
}
HRESULT D3D12Preview::Failure() const
{
	return m_state->Failure();
}
} // namespace simxr::windows
