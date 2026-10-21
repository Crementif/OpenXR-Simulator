#include "platform/linux/VulkanSessionGraphics.h"

#include "control/legacy/backends/vulkan/VulkanLegacyCapture.h"
#include "flicker_detector.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
#include "platform/linux/VulkanFrameSubmission.h"
#include "platform/linux/VulkanInstanceGraphics.h"
#include "runtime/Session.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace simxr
{
namespace
{
SnapshotAdmission PromoteInspectionCapture(const std::shared_ptr<const FrameOutput>& output, const FramePool& pool,
										   std::shared_ptr<FrameReservation> reservation)
{
	const auto frame = std::dynamic_pointer_cast<const VulkanInspectionFrame>(output);
	if (!frame)
		return SnapshotAdmission::Unsupported;
	if (frame->PromoteCaptureBudget(pool, std::move(reservation)))
		return SnapshotAdmission::Accepted;
	return SnapshotAdmission::Capacity;
}
} // namespace

class VulkanSessionGraphics::FrameBindings final : public VulkanCaptureBindings, public VulkanFrameBindings
{
  public:
	explicit FrameBindings(const VulkanSessionGraphics& graphics) : m_graphics(graphics) {}
	std::optional<SwapchainDescription> Describe(XrSwapchain swapchain) const override
	{
		const auto chain = m_graphics.FindSwapchain(swapchain);
		return chain ? std::optional{chain->Description()} : std::nullopt;
	}
	std::shared_ptr<VulkanImage> ReleasedImage(XrSwapchain swapchain) const override
	{
		const auto chain = m_graphics.FindSwapchain(swapchain);
		return chain ? chain->ReleasedImage() : nullptr;
	}
	void MarkReleasedUsed(XrSwapchain swapchain,
						  const std::shared_ptr<const GpuCompletion>& completion) const noexcept override
	{
		if (const auto chain = m_graphics.FindSwapchain(swapchain))
			chain->MarkReleasedUsed(completion);
	}
	std::optional<VulkanCaptureSource> ResolveSource(const LayerImageDescription& image) const override
	{
		const auto chain = m_graphics.FindSwapchain(image.source);
		if (!chain)
			return {};
		const auto source = chain->Resolve(image);
		if (!source)
			return {};
		return VulkanCaptureSource{source->region, image.source, source->imageIndex};
	}
	void MarkSourceUsed(const VulkanCaptureSource& source,
						const std::shared_ptr<VulkanReadbackBatch>& completion) const noexcept override
	{
		if (const auto chain = m_graphics.FindSwapchain(source.swapchain))
			chain->MarkUsed(source.imageIndex, completion);
	}

  private:
	const VulkanSessionGraphics& m_graphics;
};

XrResult VulkanSessionGraphics::Create(const VulkanBinding& binding, std::shared_ptr<Session> session,
									   std::shared_ptr<LegacyFileChannel> files,
									   std::shared_ptr<flicker::Diagnostics> diagnostics, bool validation,
									   std::unique_ptr<VulkanSessionGraphics>& output)
{
	if (!binding.instance || !binding.physicalDevice || !binding.device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	if (!session || !files || !diagnostics)
		throw std::invalid_argument("Vulkan session graphics requires matching session and observation owners");
	try
	{
		auto candidate = std::unique_ptr<VulkanSessionGraphics>(new VulkanSessionGraphics(
			binding, std::move(session), std::move(files), std::move(diagnostics), validation));
		const auto result = candidate->ConfigureCapture();
		if (XR_FAILED(result))
			return result;
		output = std::move(candidate);
		return XR_SUCCESS;
	}
	catch (const VulkanError& error)
	{
		if (error.Result() == VK_ERROR_FORMAT_NOT_SUPPORTED || error.Result() == VK_ERROR_DEVICE_LOST)
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		return VulkanGraphicsFailure(error.Result());
	}
	catch (const std::invalid_argument&)
	{
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
}

VulkanSessionGraphics::VulkanSessionGraphics(const VulkanBinding& binding, std::shared_ptr<Session> session,
											 std::shared_ptr<LegacyFileChannel> files,
											 std::shared_ptr<flicker::Diagnostics> diagnostics, bool validation)
	: m_session(std::move(session)), m_files(std::move(files)), m_diagnostics(std::move(diagnostics))
{
	const auto sharing = VulkanSharingExtensions(binding.physicalDevice);
	if (!sharing.empty() && (!vkGetDeviceProcAddr(binding.device, "vkGetMemoryFdKHR") ||
							 !vkGetDeviceProcAddr(binding.device, "vkGetSemaphoreFdKHR") ||
							 !vkGetDeviceProcAddr(binding.device, "vkImportSemaphoreFdKHR")))
		throw std::invalid_argument("Vulkan sharing extensions were not enabled on the device");
	m_backend = std::make_unique<VulkanBackend>(binding, 8, sharing, VK_API_VERSION_1_1);
	if (m_backend->Sharing())
	{
		VulkanExternalDevice external(m_backend->Device(), sharing, VK_API_VERSION_1_1);
		if (validation)
			m_validation = std::make_shared<VulkanInspectionValidation>();
		m_inspection = std::make_unique<VulkanInspection>(external.Identity(), 8, m_validation);
		m_capabilities = {m_inspection->CanShare(), true};
	}
	m_capture = std::make_unique<VulkanLegacyCapture>(m_backend->Device(), m_session, m_files, m_diagnostics);
}

VulkanSessionGraphics::~VulkanSessionGraphics() = default;

XrResult VulkanSessionGraphics::ConfigureCapture()
{
	if (!m_inspection)
		return XR_SUCCESS;
	return m_session->ConfigureCapturePool(m_backend->Reservations(), true, PromoteInspectionCapture, true, true);
}

control::ImageCapabilities VulkanSessionGraphics::Capabilities() const noexcept
{
	return m_capabilities;
}

XrResult VulkanSessionGraphics::GraphicsFailure(VkResult result)
{
	if (result == VK_ERROR_DEVICE_LOST)
		m_backend->Device()->lost.store(true);
	return VulkanGraphicsFailure(result);
}

XrResult VulkanSessionGraphics::CreateSwapchain(const XrSwapchainCreateInfo& description, XrSwapchain& output)
{
	if (m_closing || m_session->Snapshot().closed)
		return XR_ERROR_SESSION_LOST;
	try
	{
		std::unique_ptr<VulkanSwapchain> candidate;
		const auto result = VulkanSwapchain::Create(*m_backend, description, candidate);
		if (XR_FAILED(result))
			return result;
		output = m_swapchains.Insert(m_session.get(), std::move(candidate));
		return XR_SUCCESS;
	}
	catch (const VulkanError& error)
	{
		return GraphicsFailure(error.Result());
	}
}

VulkanSessionGraphics::SwapchainRef VulkanSessionGraphics::FindSwapchain(XrSwapchain handle) const
{
	return m_swapchains.Find(handle, m_session.get());
}

void VulkanSessionGraphics::DestroySwapchain(SwapchainRef swapchain)
{
	swapchain->Close();
	m_swapchains.Erase(swapchain);
}

XrResult VulkanSessionGraphics::Acquire(VulkanSwapchain& swapchain, std::uint32_t& index)
{
	if (m_closing || m_session->Snapshot().closed)
		return XR_ERROR_SESSION_LOST;
	try
	{
		return swapchain.Acquire(*m_backend, index);
	}
	catch (const VulkanError& error)
	{
		return GraphicsFailure(error.Result());
	}
}

std::optional<SwapchainWaits::Registration> VulkanSessionGraphics::EnterWait()
{
	return m_waits.Enter();
}

XrResult VulkanSessionGraphics::EndFrame(const XrFrameEndInfo& info)
{
	if (m_closing)
		return XR_ERROR_SESSION_LOST;
	if (!m_session->Snapshot().running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	const auto begun = m_session->Scheduler().Snapshot().begun;
	if (!begun)
		return XR_ERROR_CALL_ORDER_INVALID;
	const FrameBindings bindings(*this);
	VulkanFrameSubmission submission(*m_session, *m_backend, m_inspection.get(), bindings,
									 {m_session->Snapshot().generation, begun->id});
	const auto result = submission.Execute(info);
	if (XR_SUCCEEDED(result))
		ObserveFrame(info, submission.Composition(), bindings);
	return result;
}

void VulkanSessionGraphics::ObserveFrame(const XrFrameEndInfo& info, const CompositionPlan& composition,
										 const VulkanCaptureBindings& bindings) noexcept
{
	try
	{
		m_capture->Submit(info, composition, bindings);
		std::uint32_t projections = 0;
		for (std::uint32_t layer = 0; layer < info.layerCount; ++layer)
			if (info.layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
				++projections;
		m_diagnostics->ObserveSubmission(m_session->Snapshot().lastSubmitted.value, projections, info.layerCount);
	}
	catch (const std::exception& error)
	{
		LogObserverFailure(error.what());
	}
	catch (...)
	{
		LogObserverFailure("unknown exception");
	}
}

void VulkanSessionGraphics::LogObserverFailure(const char* detail) noexcept
{
	try
	{
		m_files->Log(std::string("submitted frame observer failed: ") + detail);
	}
	catch (...)
	{
	}
}

void VulkanSessionGraphics::RequestCapture(LegacyCaptureRequest request, FrameId frame)
{
	if (m_capture)
		m_capture->Request(std::move(request), frame);
}

void VulkanSessionGraphics::PollCaptures()
{
	if (m_capture)
		m_capture->Poll();
}

void VulkanSessionGraphics::BeginClose()
{
	m_closing = true;
	m_waits.Close();
	for (const auto handle : m_swapchains.Children(m_session.get()))
		FindSwapchain(handle)->Close();
}

XrResult VulkanSessionGraphics::Drain(std::chrono::nanoseconds timeout)
{
	if (timeout < std::chrono::nanoseconds::zero())
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(m_closeMutex);
	if (m_closeStage != CloseStage::Pending)
		return XR_SUCCESS;
	if (!m_session->Snapshot().closed)
		return XR_ERROR_CALL_ORDER_INVALID;
	using Clock = std::chrono::steady_clock;
	const auto maximum = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(5));
	const auto deadline = Clock::now() + std::min(timeout, maximum);
	const auto remaining = [&]
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::max(deadline - Clock::now(), Clock::duration::zero()));
	};
	if (!m_waits.Drain(remaining()))
		return XR_ERROR_RUNTIME_FAILURE;
	const auto drained = m_backend->Drain(static_cast<std::uint64_t>(remaining().count()));
	if (drained != VK_SUCCESS && drained != VK_ERROR_DEVICE_LOST)
		return XR_ERROR_RUNTIME_FAILURE;
	if (m_inspection)
	{
		const auto inspected = m_inspection->Close(static_cast<std::uint64_t>(remaining().count()));
		if (inspected != VK_SUCCESS && inspected != VK_ERROR_DEVICE_LOST)
			return XR_ERROR_RUNTIME_FAILURE;
		if (m_validation && m_validation->errors.load())
			return XR_ERROR_RUNTIME_FAILURE;
	}
	if (m_capture->Close(static_cast<std::uint64_t>(remaining().count())) != VK_SUCCESS)
		return XR_ERROR_RUNTIME_FAILURE;
	m_closeStage = CloseStage::Drained;
	return XR_SUCCESS;
}

XrResult VulkanSessionGraphics::FinishClose()
{
	std::lock_guard lock(m_closeMutex);
	if (m_closeStage == CloseStage::Closed)
		return XR_SUCCESS;
	if (m_closeStage != CloseStage::Drained)
		return XR_ERROR_CALL_ORDER_INVALID;
	for (const auto handle : m_swapchains.Children(m_session.get()))
		DestroySwapchain(FindSwapchain(handle));
	const auto result = m_backend->Close();
	if (XR_FAILED(result))
		return result;
	m_capture.reset();
	m_inspection.reset();
	m_validation.reset();
	m_backend.reset();
	m_closeStage = CloseStage::Closed;
	return XR_SUCCESS;
}
} // namespace simxr
