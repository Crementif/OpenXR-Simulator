#include "graphics/backends/windows/WindowsSessionGraphics.h"
#include "Pch.h"
#include "graphics/backends/windows/WindowsApplicationGpuTimer.h"
#include "graphics/FrameSubmission.h"
#include "graphics/backends/d3d11/D3D11VulkanImport.h"
#include "graphics/backends/d3d12/D3D12VulkanImport.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include "graphics/backends/vulkan/VulkanReprojection.h"
#include "runtime/Session.h"

#include <dxgi.h>

namespace simxr
{
static LUID AdapterLuid(ID3D11Device* device)
{
	Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
	Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC description{};
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
		FAILED(adapter->GetDesc(&description)))
		throw std::runtime_error("Cannot identify D3D11 graphics adapter");
	return description.AdapterLuid;
}

static VulkanExternalIdentity AdapterIdentity(LUID adapter)
{
	struct Instance
	{
		VkInstance handle = VK_NULL_HANDLE;

		~Instance()
		{
			if (handle)
				vkDestroyInstance(handle, nullptr);
		}
	} instance;

	VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	create.pApplicationInfo = &application;
	CheckVulkan(vkCreateInstance(&create, nullptr, &instance.handle), "Create Direct3D sharing instance");
	std::vector<VkPhysicalDevice> devices;
	for (std::uint32_t attempt = 0; attempt < 4; ++attempt)
	{
		std::uint32_t count = 0;
		CheckVulkan(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr),
					"Enumerate Direct3D sharing adapters");
		devices.resize(count);
		if (!count)
			break;
		const auto result = vkEnumeratePhysicalDevices(instance.handle, &count, devices.data());
		if (result == VK_INCOMPLETE && attempt < 3)
			continue;
		CheckVulkan(result, "Enumerate Direct3D sharing adapters");
		devices.resize(count);
		break;
	}
	for (const auto physical : devices)
	{
		VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
		vkGetPhysicalDeviceProperties2(physical, &properties);
		if (identity.deviceLUIDValid && !std::memcmp(identity.deviceLUID, &adapter, VK_LUID_SIZE))
		{
			VulkanExternalIdentity result;
			std::memcpy(result.deviceUuid.data(), identity.deviceUUID, VK_UUID_SIZE);
			std::memcpy(result.driverUuid.data(), identity.driverUUID, VK_UUID_SIZE);
			return result;
		}
	}
	throw VulkanError(VK_ERROR_INCOMPATIBLE_DRIVER, "No Vulkan adapter matches the Direct3D application");
}

template<typename Backend>
class WindowsFrameSubmission final : public FrameSubmissionBackend
{
private:
	using Frame = typename Backend::Frame;
	using Source = typename Backend::Source;

	Source Resolve(XrSwapchain swapchain) const
	{
		const auto source = m_resolve(swapchain);
		const auto* native = std::get_if<Source>(&source);
		return native ? *native : Source{};
	}

	static auto Describe(Source source)
	{
		struct Description
		{
			std::int32_t width, height;
			bool multisampled;
		};

		if constexpr (std::is_same_v<Backend, D3D11Backend>)
		{
			D3D11_TEXTURE2D_DESC description{};
			source.texture->GetDesc(&description);
			return Description{static_cast<std::int32_t>(description.Width),
							   static_cast<std::int32_t>(description.Height), description.SampleDesc.Count > 1};
		}
		else if constexpr (std::is_same_v<Backend, OpenGLBackend>)
			return Description{static_cast<std::int32_t>(source.width), static_cast<std::int32_t>(source.height),
							   source.samples > 1};
		else
		{
			const auto description = source.texture->GetDesc();
			return Description{static_cast<std::int32_t>(description.Width),
							   static_cast<std::int32_t>(description.Height), description.SampleDesc.Count > 1};
		}
	}

	auto Import(const std::shared_ptr<Frame>& frame)
	{
		if constexpr (std::is_same_v<Backend, D3D11Backend>)
			return ImportD3D11Frame(m_delivery.Device(), frame);
		else if constexpr (std::is_same_v<Backend, D3D12Backend>)
			return ImportD3D12Frame(m_delivery.Device(), frame);
		else
			return frame;
	}

	static XrResult NativeResult(std::int32_t result)
	{
		if constexpr (std::is_same_v<Backend, OpenGLBackend>)
			return static_cast<XrResult>(result);
		if (result == DXGI_ERROR_WAS_STILL_DRAWING)
			return XR_ERROR_LIMIT_REACHED;
		if (result == DXGI_ERROR_UNSUPPORTED)
			return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		if (result == E_OUTOFMEMORY)
			return XR_ERROR_OUT_OF_MEMORY;
		return FAILED(result) ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
	}

public:
	WindowsFrameSubmission(Backend& native, VulkanBackend& delivery, VulkanInspection& inspection,
						   const WindowsSessionGraphics::ImageResolver& resolve,
						   const WindowsSessionGraphics::SourceUse& sourceUsed, bool retainLayers)
		: m_native(native), m_delivery(delivery), m_inspection(inspection), m_resolve(resolve),
		  m_sourceUsed(sourceUsed), m_retainLayers(retainLayers)
	{}

	FramePool LiveReservations() const override
	{
		auto pool = m_delivery.LiveReservations();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		// Native composition, the Vulkan copy and inspection retire independently.
		// Apply backpressure before admission instead of rejecting a valid app frame.
		while (pool.InUse() >= pool.Capacity())
		{
			pool.Collect();
			if (pool.InUse() < pool.Capacity() || std::chrono::steady_clock::now() >= deadline)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return pool;
	}

	FramePool HistoryReservations() const override
	{
		return m_delivery.HistoryReservations();
	}

	XrResult Prepare(FrameKey, const SessionComposition& composition, std::shared_ptr<FrameReservation> reservation,
					 std::span<const FrameCaptureRequest> captures, PreparedFrameSubmission& output) override
	{
		try
		{
			const auto required =
				std::count_if(captures.begin(), captures.end(), [](const auto& request) { return !request.history; });
			m_frames.reserve(required + 1);
			m_batch.reserve(required + 1);
			auto result = PrepareComposition(composition.plan, std::move(reservation));
			if (XR_FAILED(result))
				return result;
			output = {m_batch.front(), m_frames.front().transfer->Output(), {}, {}, m_batch.front()->Timing()};
			output.captures.reserve(required);
			for (const auto& request : captures)
			{
				try
				{
					if (request.history)
					{
						m_frames.reserve(m_frames.size() + 1);
						m_batch.reserve(m_batch.size() + 1);
						if (!request.preview)
							output.captures.push_back({request.target, {}, request.quadBounds});
					}
					if (!request.reservation)
					{
						continue;
					}
					result = request.depth
								 ? PrepareDepth(*request.depth, request.target.kind == CaptureKind::LayerDepthLinear,
												request.reservation)
							 : request.composition ? PrepareComposition(*request.composition, request.reservation,
																		request.history ? &request : nullptr)
												   : PrepareCapture(*request.image, request.reservation);
					if (request.IgnoreFailure(result))
						continue;
					if (XR_FAILED(result))
						return result;
					if (request.preview)
						output.preview = m_frames.back().transfer->Output();
					else if (request.history)
						output.captures.back().output = m_frames.back().transfer->Output();
					else
						output.captures.push_back(
							{request.target, m_frames.back().transfer->Output(), request.quadBounds});
				}
				catch (const VulkanError& error)
				{
					if (!request.IgnoreFailure(VulkanGraphicsFailure(error.Result())))
						throw;
				}
				catch (const std::bad_alloc&)
				{
					if (!request.IgnoreFailure(XR_ERROR_OUT_OF_MEMORY))
						throw;
				}
			}
			return m_retainLayers ? PrepareReprojection(composition, output.reprojection) : XR_SUCCESS;
		}
		catch (const VulkanError& error)
		{
			if (error.Result() == VK_ERROR_DEVICE_LOST)
				m_delivery.Device()->lost.store(true);
			return VulkanGraphicsFailure(error.Result());
		}
	}

	XrResult Submit() override
	{
		for (const auto& frame : m_frames)
			if (XR_FAILED(SubmitNative(frame.native, frame.sources)))
				return XR_ERROR_RUNTIME_FAILURE;
		for (const auto& frame : m_retainedNative)
			if (XR_FAILED(SubmitNative(frame.native, std::span{&frame.source, 1})))
				return XR_ERROR_RUNTIME_FAILURE;
		const auto result = m_delivery.Submit(m_batch);
		if (XR_SUCCEEDED(result))
		{
			for (const auto& frame : m_frames)
				frame.transfer->Commit();
			if (m_retention)
				m_retention->Commit();
		}
		return result;
	}

	bool Submitted() const noexcept override
	{
		return !m_batch.empty() && m_batch.front()->Submitted();
	}

private:
	void MarkSourcesUsed(const std::shared_ptr<Frame>& frame, std::span<const XrSwapchain> sources) const
	{
		if (m_sourceUsed && frame->SourcesUsed())
			for (const auto source : sources)
				m_sourceUsed(source, frame);
	}

	XrResult SubmitNative(const std::shared_ptr<Frame>& frame, std::span<const XrSwapchain> sources)
	{
		std::int32_t result;
		try
		{
			result = m_native.Submit(frame);
		}
		catch (...)
		{
			MarkSourcesUsed(frame, sources);
			throw;
		}
		MarkSourcesUsed(frame, sources);
		return NativeResult(result);
	}

	XrResult PrepareReprojection(const SessionComposition& composition, ReprojectionInput& output)
	{
		try
		{
			m_retainedNative.reserve(32);
			m_batch.reserve(64);
			output.status = VulkanReprojectionPreparation::Prepare(
				m_inspection, *composition.layers, composition.target, composition.head,
				[this](XrSwapchain swapchain) -> std::optional<XrExtent2Di> {
					const auto source = Resolve(swapchain);
					if (!source.texture)
						return {};
					const auto description = Describe(source);
					return XrExtent2Di{description.width, description.height};
				},
				[this](const LayerImageDescription& image, std::shared_ptr<VulkanFrame>& output) {
					auto reservation = m_delivery.ReprojectionReservations().Reserve();
					if (!reservation)
						return XR_ERROR_LIMIT_REACHED;
					std::shared_ptr<Frame> native;
					const auto prepared =
						NativeResult(m_native.PrepareCapture(image, Resolve(image.source), native, true));
					if (XR_FAILED(prepared))
						return prepared;
					auto imported = Import(native);
					const auto result =
						m_delivery.PrepareNativeCopy(imported->Image(), imported->Ready(), Frame::ReadyValue, imported,
													 {}, output, std::move(reservation));
					if (XR_SUCCEEDED(result))
						m_retainedNative.push_back({std::move(native), image.source});
					return result;
				},
				64 - m_batch.size(), m_retention);
			if (output.status == XR_SUCCESS)
			{
				const auto frames = m_retention->Frames();
				m_batch.insert(m_batch.end(), frames.begin(), frames.end());
				output.source = m_retention->Source();
			}
		}
		catch (const std::bad_alloc&)
		{
			output.status = XR_ERROR_OUT_OF_MEMORY;
		}
		if (output.status == XR_ERROR_SESSION_LOST)
			m_delivery.Device()->lost.store(true);
		if (output.status != XR_SUCCESS)
		{
			m_retention.reset();
			m_retainedNative.clear();
		}
		switch (output.status)
		{
		case XR_SUCCESS:
		case XR_ERROR_LIMIT_REACHED:
		case XR_ERROR_OUT_OF_MEMORY:
		case XR_ERROR_FEATURE_UNSUPPORTED:
		case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED:
		case XR_ERROR_POSE_INVALID:
			return XR_SUCCESS;
		default:
			return output.status;
		}
	}

	XrResult PrepareComposition(const CompositionPlan& plan, std::shared_ptr<FrameReservation> reservation,
								const FrameCaptureRequest* history = nullptr)
	{
		std::vector<Source> sources;
		std::vector<XrSwapchain> swapchains;
		sources.reserve(plan.draws.size());
		swapchains.reserve(plan.draws.size());
		for (const auto& draw : plan.draws)
		{
			sources.push_back(Resolve(draw.source.source));
			swapchains.push_back(draw.source.source);
		}
		std::shared_ptr<Frame> native;
		const auto prepared = NativeResult(m_native.Prepare(plan, sources, native));
		if (XR_FAILED(prepared))
			return prepared;
		return PrepareDelivery(std::move(native), std::move(reservation), std::move(swapchains), history);
	}

	XrResult PrepareCapture(const LayerImageDescription& image, std::shared_ptr<FrameReservation> reservation)
	{
		std::shared_ptr<Frame> native;
		const auto prepared = NativeResult(m_native.PrepareCapture(image, Resolve(image.source), native));
		if (XR_FAILED(prepared))
			return prepared;
		try
		{
			return PrepareDelivery(std::move(native), std::move(reservation), {image.source});
		}
		catch (const VulkanError& error)
		{
			if (error.Result() == VK_ERROR_FORMAT_NOT_SUPPORTED || error.Result() == VK_ERROR_FEATURE_NOT_PRESENT)
				return XR_ERROR_FEATURE_UNSUPPORTED;
			throw;
		}
	}

	XrResult PrepareDepth(const LayerDepthDescription& depth, bool linear,
						  std::shared_ptr<FrameReservation> reservation)
	{
		const auto source = Resolve(depth.image.source);
		if (!source.texture)
			return XR_ERROR_LAYER_INVALID;
		const auto description = Describe(source);
		CompositionPlan plan;
		const auto result = PrepareDepthComposition(depth, linear, {description.width, description.height},
													description.multisampled, plan);
		if (XR_FAILED(result))
			return result;
		std::shared_ptr<Frame> native;
		const auto prepared = NativeResult(m_native.Prepare(plan, std::span{&source, 1}, native, 1));
		if (XR_FAILED(prepared))
			return prepared;
		return PrepareDelivery(std::move(native), std::move(reservation), {depth.image.source});
	}

	XrResult PrepareDelivery(std::shared_ptr<Frame> native, std::shared_ptr<FrameReservation> reservation,
							 std::vector<XrSwapchain> sources, const FrameCaptureRequest* history = nullptr)
	{
		auto imported = Import(native);
		std::shared_ptr<VulkanFrame> copied;
		const auto result = m_delivery.PrepareNativeCopy(imported->Image(), imported->Ready(), Frame::ReadyValue,
														 imported, native->Views(), copied, std::move(reservation));
		if (XR_FAILED(result))
			return result;
		auto transfer = m_inspection.Prepare(
			copied, [producers = m_delivery.LiveReservations()](CompletionStatus) { producers.Collect(); }, {},
			history ? VulkanInspectionKind::History : VulkanInspectionKind::Required);
		if (history && history->preview && transfer->Output()->Image().allocationSize > FramePreviewHistory::SlotBytes)
			throw std::runtime_error("Preview allocation exceeds its history reservation");
		if (history && !transfer->Output()->PromoteCaptureBudget(*history->historyPool, history->historyReservation))
			return XR_ERROR_LIMIT_REACHED;
		m_frames.push_back({std::move(native), std::move(transfer), std::move(sources)});
		m_batch.push_back(std::move(copied));
		return XR_SUCCESS;
	}

	struct NativeFrame
	{
		std::shared_ptr<Frame> native;
		std::unique_ptr<VulkanInspectionTransfer> transfer;
		std::vector<XrSwapchain> sources;
	};

	struct RetainedFrame
	{
		std::shared_ptr<Frame> native;
		XrSwapchain source;
	};

	Backend& m_native;
	VulkanBackend& m_delivery;
	VulkanInspection& m_inspection;
	const WindowsSessionGraphics::ImageResolver& m_resolve;
	const WindowsSessionGraphics::SourceUse& m_sourceUsed;
	const bool m_retainLayers;
	std::unique_ptr<VulkanReprojectionPreparation> m_retention;
	std::vector<RetainedFrame> m_retainedNative;
	std::vector<NativeFrame> m_frames;
	std::vector<std::shared_ptr<VulkanFrame>> m_batch;
};

WindowsSessionGraphics::WindowsSessionGraphics(ID3D11Device* device, std::shared_ptr<Session> session)
	: m_session(std::move(session)), m_native(std::make_unique<D3D11Backend>(device))
{
	Configure(AdapterIdentity(AdapterLuid(device)));
	m_applicationTimer = CreateApplicationGpuTimer(device);
}

WindowsSessionGraphics::WindowsSessionGraphics(ID3D12Device* device, ID3D12CommandQueue* queue,
											   std::shared_ptr<Session> session)
	: m_session(std::move(session)), m_native(std::make_unique<D3D12Backend>(device, queue))
{
	Configure(AdapterIdentity(device->GetAdapterLuid()));
	m_applicationTimer = CreateApplicationGpuTimer(device, queue);
}

WindowsSessionGraphics::WindowsSessionGraphics(HDC device, HGLRC context, std::shared_ptr<Session> session)
	: m_session(std::move(session))
{
	Configure(OpenGLBackend::Identity(device, context));
	m_native = std::make_unique<OpenGLBackend>(device, context, m_device->Device(), m_device->EnabledExtensions());
	m_applicationTimer = std::get<std::unique_ptr<OpenGLBackend>>(m_native)->CreateApplicationTimer();
}

WindowsSessionGraphics::~WindowsSessionGraphics() = default;

XrResult WindowsSessionGraphics::Close()
{
	std::lock_guard close(m_closeMutex);
	{
		std::lock_guard submission(m_submissionMutex);
		m_closing = true;
		if (m_applicationTimer)
			m_applicationTimer->End();
	}
	const auto drained = m_delivery->Drain(5000000000ULL);
	if (drained != VK_SUCCESS && drained != VK_ERROR_DEVICE_LOST)
		return VulkanGraphicsFailure(drained);
	const auto inspected = m_inspection->Close();
	if (inspected != VK_SUCCESS && inspected != VK_ERROR_DEVICE_LOST)
		return VulkanGraphicsFailure(inspected);
	if (m_applicationTimer && !m_applicationTimer->Drain(5000000000ULL))
		return XR_ERROR_RUNTIME_FAILURE;
	m_applicationTimer.reset();
	return m_delivery->Close();
}

control::ImageCapabilities WindowsSessionGraphics::Capabilities() const noexcept
{
	return {m_inspection->CanShare(), true, true};
}

void WindowsSessionGraphics::BeginFrame()
{
	std::lock_guard submission(m_submissionMutex);
	if (!m_closing && m_applicationTimer)
		m_applicationTimer->Begin();
}

XrResult WindowsSessionGraphics::EndFrame(const XrFrameEndInfo& info, const SwapchainDescriptionResolver& describe,
										  const ImageResolver& resolve, const SourceUse& sourceUsed)
{
	std::lock_guard submission(m_submissionMutex);
	if (m_closing)
		return XR_ERROR_SESSION_LOST;
	return std::visit(
		[&](auto& native) {
			WindowsFrameSubmission backend(*native, *m_delivery, *m_inspection, resolve, sourceUsed,
										   m_session->Display().Policy() == PresentationPolicy::Rotational);
			FrameSubmission submission(*m_session, backend,
									   m_applicationTimer ? m_applicationTimer->End() : GpuDuration{});
			if (m_applicationTimer)
				m_applicationTimer->Collect();
			return submission.Execute(info, describe);
		},
		m_native);
}

void WindowsSessionGraphics::Configure(const VulkanExternalIdentity& identity)
{
	m_device = std::make_unique<VulkanInspectionDevice>(identity, nullptr, true);
	if (!m_device->ConsumerTimelineEnabled())
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Direct3D sharing requires Vulkan timeline semaphores");
	m_delivery = std::make_unique<VulkanBackend>(m_device->Device(), 8, m_device->EnabledExtensions(), true);
	m_inspection = std::make_unique<VulkanInspection>(identity, 8);
	const auto configured = m_session->ConfigureCapturePool(
		m_delivery->Reservations(), true,
		[](const std::shared_ptr<const FrameOutput>& output, const FramePool& pool,
		   std::shared_ptr<FrameReservation> reservation) {
			const auto image = std::dynamic_pointer_cast<const VulkanInspectionFrame>(output);
			if (!image)
				return SnapshotAdmission::Unsupported;
			return image->PromoteCaptureBudget(pool, std::move(reservation)) ? SnapshotAdmission::Accepted
																			 : SnapshotAdmission::Capacity;
		},
		true, true);
	if (XR_FAILED(configured) || XR_FAILED(m_session->EnableImageHistory()))
		throw std::runtime_error("Cannot configure Direct3D frame retention");
}
} // namespace simxr
