#include "graphics/backends/windows/WindowsApplicationGpuTimer.h"
#include "Pch.h"
#include "graphics/FramePool.h"
#include "platform/windows/ModuleReference.h"
#include "platform/windows/OpenGLLibrary.h"
#include "platform/windows/OpenGLContextScope.h"
#include <wrl/client.h>

namespace simxr
{
using Microsoft::WRL::ComPtr;

static void CheckTimer(HRESULT result)
{
	if (FAILED(result))
		throw std::runtime_error("Cannot initialize application GPU timing");
}

static std::int64_t Nanoseconds(std::uint64_t begin, std::uint64_t end, std::uint64_t frequency)
{
	if (!frequency || end < begin)
		return -1;
	const double duration = static_cast<double>(end - begin) * 1000000000.0 / static_cast<double>(frequency);
	return duration < static_cast<double>(INT64_MAX) ? static_cast<std::int64_t>(duration) : -1;
}

class D3D11ApplicationTimer final : public ApplicationGpuTimer
{
public:
	explicit D3D11ApplicationTimer(ID3D11Device* device)
	{
		device->GetImmediateContext(&m_context);
		const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		const D3D11_QUERY_DESC timestamp{D3D11_QUERY_TIMESTAMP, 0};
		for (auto& slot : m_queries)
		{
			CheckTimer(device->CreateQuery(&disjoint, &slot.disjoint));
			CheckTimer(device->CreateQuery(&timestamp, &slot.begin));
			CheckTimer(device->CreateQuery(&timestamp, &slot.end));
		}
	}

	~D3D11ApplicationTimer() override
	{
		End();
		Collect();
	}

private:
	bool Write(std::size_t index, bool end) override
	{
		auto& slot = m_queries[index];
		if (end)
		{
			m_context->End(slot.end.Get());
			m_context->End(slot.disjoint.Get());
		}
		else
		{
			m_context->Begin(slot.disjoint.Get());
			m_context->End(slot.begin.Get());
		}
		return true;
	}

	bool Read(std::size_t index, std::int64_t& nanoseconds) override
	{
		auto& slot = m_queries[index];
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
		UINT64 begin = 0, end = 0;
		const auto status =
			m_context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (FAILED(status))
			return true;
		if (status != S_OK ||
			m_context->GetData(slot.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			m_context->GetData(slot.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return false;
		if (!disjoint.Disjoint)
			nanoseconds = Nanoseconds(begin, end, disjoint.Frequency);
		return true;
	}

	struct Queries
	{
		ComPtr<ID3D11Query> disjoint, begin, end;
	};

	ComPtr<ID3D11DeviceContext> m_context;
	std::array<Queries, SlotCount> m_queries;
};

class D3D12ApplicationTimer final : public ApplicationGpuTimer
{
	struct Resources
	{
		windows::ModuleReference module;
		windows::WindowsLibrary runtime{L"d3d12.dll"};
		std::shared_ptr<FrameReservation> preservation;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12QueryHeap> heap;
		ComPtr<ID3D12Resource> readback;
		ComPtr<ID3D12Fence> fence;
		ComPtr<ID3D12CommandAllocator> allocator;
		std::array<ComPtr<ID3D12GraphicsCommandList>, SlotCount * 2> lists;
		std::array<UINT64, SlotCount> completed{};
		const UINT64* ticks = nullptr;
		UINT64 submitted = 0;
		bool uncertain = false;

		Resources()
		{
			static FramePool s_owners(32);
			preservation = s_owners.Reserve();
			if (!preservation)
				throw std::runtime_error("Application timing owners exhausted");
		}

		~Resources()
		{
			if (ticks)
				readback->Unmap(0, nullptr);
		}
	};

public:
	D3D12ApplicationTimer(ID3D12Device* device, ID3D12CommandQueue* queue) : m_resources(std::make_unique<Resources>())
	{
		auto& r = *m_resources;
		r.device = device;
		r.queue = queue;
		CheckTimer(queue->GetTimestampFrequency(&m_frequency));
		if (!m_frequency)
			throw std::runtime_error("GPU timestamps unavailable");
		const auto type = queue->GetDesc().Type;
		const D3D12_QUERY_HEAP_DESC heap{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, SlotCount * 2, 0};
		const D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = SlotCount * 2 * sizeof(UINT64);
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		CheckTimer(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&r.heap)));
		CheckTimer(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
												   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.readback)));
		CheckTimer(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)));
		CheckTimer(device->CreateCommandAllocator(type, IID_PPV_ARGS(&r.allocator)));
		for (UINT index = 0; index < SlotCount * 2; ++index)
		{
			auto& list = r.lists[index];
			CheckTimer(device->CreateCommandList(0, type, r.allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
			list->EndQuery(r.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
			list->ResolveQueryData(r.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1, r.readback.Get(),
								   index * sizeof(UINT64));
			CheckTimer(list->Close());
		}
		const D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
		void* mapped = nullptr;
		CheckTimer(r.readback->Map(0, &range, &mapped));
		r.ticks = static_cast<UINT64*>(mapped);
	}

	bool Drain(std::uint64_t timeout) override
	{
		auto& r = *m_resources;
		if (FAILED(r.device->GetDeviceRemovedReason()))
			return true;
		if (r.uncertain)
			return false;
		if (r.fence->GetCompletedValue() < r.submitted)
		{
			const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!event)
				return false;
			const auto signaled = r.fence->SetEventOnCompletion(r.submitted, event);
			if (SUCCEEDED(signaled))
				WaitForSingleObject(event, static_cast<DWORD>(std::min<std::uint64_t>(timeout / 1000000, 5000)));
			CloseHandle(event);
			if (FAILED(signaled) || r.fence->GetCompletedValue() < r.submitted)
				return false;
		}
		Collect();
		return true;
	}

	~D3D12ApplicationTimer() override
	{
		Collect();
		const auto& r = *m_resources;
		if (SUCCEEDED(r.device->GetDeviceRemovedReason()) &&
			(r.uncertain || r.fence->GetCompletedValue() < r.submitted))
			m_resources.release(); // Bounded preservation includes the module, device and every native dependency.
	}

private:
	bool Write(std::size_t index, bool end) override
	{
		auto& r = *m_resources;
		ID3D12CommandList* list = r.lists[index * 2 + static_cast<std::size_t>(end)].Get();
		r.queue->ExecuteCommandLists(1, &list);
		r.uncertain = true;
		if (FAILED(r.queue->Signal(r.fence.Get(), ++r.submitted)))
			return false;
		r.uncertain = false;
		if (end)
			r.completed[index] = r.submitted;
		return true;
	}

	bool Read(std::size_t index, std::int64_t& nanoseconds) override
	{
		const auto& r = *m_resources;
		const auto completed = r.fence->GetCompletedValue();
		if (completed == UINT64_MAX)
			return true;
		if (completed < r.completed[index])
			return false;
		nanoseconds = Nanoseconds(r.ticks[index * 2], r.ticks[index * 2 + 1], m_frequency);
		return true;
	}

	std::unique_ptr<Resources> m_resources;
	UINT64 m_frequency = 0;
};

class OpenGLApplicationTimer final : public ApplicationGpuTimer
{
public:
	OpenGLApplicationTimer(HDC device, HGLRC context) : m_device(device), m_context(context)
	{
		windows::OpenGLContextScope current(m_gl, device, context);
		if (!current.Current())
			throw std::runtime_error("Application GL context unavailable");
		m_counter = Procedure<Counter>("glQueryCounter");
		m_result = Procedure<Result>("glGetQueryObjectui64v");
		m_available = Procedure<Available>("glGetQueryObjectiv");
		m_delete = Procedure<Delete>("glDeleteQueries");
		Procedure<Generate>("glGenQueries")(static_cast<int>(m_queries.size()), m_queries.data());
	}

	~OpenGLApplicationTimer() override
	{
		Collect();
		windows::OpenGLContextScope current(m_gl, m_device, m_context);
		if (current.Current())
			m_delete(static_cast<int>(m_queries.size()), m_queries.data());
		// If the application context has gone away, it owns the remaining query cleanup.
	}

private:
	using Counter = void(APIENTRY*)(unsigned, unsigned);
	using Result = void(APIENTRY*)(unsigned, unsigned, std::uint64_t*);
	using Available = void(APIENTRY*)(unsigned, unsigned, int*);
	using Delete = void(APIENTRY*)(int, const unsigned*);
	using Generate = void(APIENTRY*)(int, unsigned*);

	template<class T>
	T Procedure(const char* name)
	{
		const auto value = m_gl.GetProcAddress(name);
		if (!value || reinterpret_cast<std::intptr_t>(value) == -1 || reinterpret_cast<std::uintptr_t>(value) <= 3)
			throw std::runtime_error("GL timestamps unavailable");
		return std::bit_cast<T>(value);
	}

	bool Write(std::size_t slot, bool end) override
	{
		windows::OpenGLContextScope current(m_gl, m_device, m_context);
		if (!current.Current())
			return false;
		m_counter(m_queries[slot * 2 + static_cast<std::size_t>(end)], 0x8E28); // GL_TIMESTAMP
		return true;
	}

	bool Read(std::size_t slot, std::int64_t& nanoseconds) override
	{
		windows::OpenGLContextScope current(m_gl, m_device, m_context);
		if (!current.Current())
			return false;
		int available = 0;
		m_available(m_queries[slot * 2 + 1], 0x8867, &available); // GL_QUERY_RESULT_AVAILABLE
		if (!available)
			return false;
		std::uint64_t begin = 0, end = 0;
		m_result(m_queries[slot * 2], 0x8866, &begin);
		m_result(m_queries[slot * 2 + 1], 0x8866, &end);
		nanoseconds = Nanoseconds(begin, end, 1000000000);
		return true;
	}

	windows::OpenGLLibrary m_gl;
	HDC m_device;
	HGLRC m_context;
	Counter m_counter = nullptr;
	Result m_result = nullptr;
	Available m_available = nullptr;
	Delete m_delete = nullptr;
	std::array<unsigned, SlotCount * 2> m_queries{};
};

std::unique_ptr<ApplicationGpuTimer> CreateApplicationGpuTimer(ID3D11Device* device)
{
	try
	{
		return std::make_unique<D3D11ApplicationTimer>(device);
	}
	catch (const std::runtime_error&)
	{
		return {};
	}
}

std::unique_ptr<ApplicationGpuTimer> CreateApplicationGpuTimer(ID3D12Device* device, ID3D12CommandQueue* queue)
{
	try
	{
		return std::make_unique<D3D12ApplicationTimer>(device, queue);
	}
	catch (const std::runtime_error&)
	{
		return {};
	}
}

std::unique_ptr<ApplicationGpuTimer> CreateApplicationGpuTimer(HDC device, HGLRC context)
{
	try
	{
		return std::make_unique<OpenGLApplicationTimer>(device, context);
	}
	catch (const std::runtime_error&)
	{
		return {};
	}
}
}
