#include "graphics/backends/vulkan/VulkanSessionGraphics.h"

#include "control/legacy/LegacyFileChannel.h"
#include "graphics/backends/vulkan/VulkanFrameObserver.h"
#include "graphics/backends/vulkan/VulkanFrameSubmission.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"
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

XrResult VulkanSessionGraphics::Create(const VulkanBinding& binding, std::shared_ptr<Session> session, bool validation,
									   std::unique_ptr<VulkanSessionGraphics>& output)
{
	if (!binding.instance || !binding.physicalDevice || !binding.device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	if (!session)
		throw std::invalid_argument("Vulkan session graphics requires a session owner");
	try
	{
		auto candidate =
			std::unique_ptr<VulkanSessionGraphics>(new VulkanSessionGraphics(binding, std::move(session), validation));
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
											 bool validation)
	: m_session(std::move(session))
{
	const auto sharing = VulkanSharingExtensions(binding.physicalDevice);
#ifdef _WIN32
	constexpr const char* memoryCommand = "vkGetMemoryWin32HandleKHR";
	constexpr const char* semaphoreCommand = "vkGetSemaphoreWin32HandleKHR";
	constexpr const char* importCommand = "vkImportSemaphoreWin32HandleKHR";
#else
	constexpr const char* memoryCommand = "vkGetMemoryFdKHR";
	constexpr const char* semaphoreCommand = "vkGetSemaphoreFdKHR";
	constexpr const char* importCommand = "vkImportSemaphoreFdKHR";
#endif
	if (!sharing.empty() &&
		(!vkGetDeviceProcAddr(binding.device, memoryCommand) ||
		 !vkGetDeviceProcAddr(binding.device, semaphoreCommand) || !vkGetDeviceProcAddr(binding.device, importCommand)))
		throw std::invalid_argument("Vulkan sharing extensions were not enabled on the device");
	m_backend = std::make_unique<VulkanBackend>(binding, 0, sharing, VK_API_VERSION_1_1);
	if (m_backend->Sharing())
	{
		VulkanExternalDevice external(m_backend->Device(), sharing, VK_API_VERSION_1_1);
		if (validation)
			m_validation = std::make_shared<VulkanInspectionValidation>();
		m_inspection = std::make_unique<VulkanInspection>(external.Identity(), 0, m_validation);
		m_capabilities = {m_inspection->CanShare(), true};
	}
}

VulkanSessionGraphics::~VulkanSessionGraphics() = default;

XrResult VulkanSessionGraphics::ConfigureCapture()
{
	if (!m_inspection)
		return XR_SUCCESS;
	const auto result =
		m_session->ConfigureCapturePool(m_backend->Reservations(), true, PromoteInspectionCapture, true, true);
	return XR_FAILED(result) ? result : m_session->EnableImageHistory();
}

control::ImageCapabilities VulkanSessionGraphics::Capabilities() const noexcept
{
	auto capabilities = m_capabilities;
	capabilities.rotationalReprojection = SupportsReprojection();
	return capabilities;
}

bool VulkanSessionGraphics::SupportsReprojection() const noexcept
{
	return m_inspection != nullptr;
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
	const FrameBindings bindings(*this);
	VulkanFrameSubmission backend(*m_session, *m_backend, m_inspection.get(), bindings,
		m_session->Display().Policy() == PresentationPolicy::Rotational);
	FrameSubmission submission(*m_session, backend);
	const auto result = submission.Execute(info,
		[&bindings](XrSwapchain swapchain) { return bindings.Describe(swapchain); });
	if (XR_SUCCEEDED(result) && m_observer)
		m_observer->Observe(info, submission.Composition(), bindings);
	return result;
}

std::shared_ptr<VulkanDevice> VulkanSessionGraphics::Device() const
{
	return m_backend->Device();
}
void VulkanSessionGraphics::SetObserver(std::unique_ptr<VulkanFrameObserver> observer)
{
	if (m_observer || m_closing)
		throw std::logic_error("Vulkan frame observer is already installed or closing");
	m_observer = std::move(observer);
}

void VulkanSessionGraphics::RequestCapture(LegacyCaptureRequest request, FrameId frame)
{
	if (m_observer)
		m_observer->Request(std::move(request), frame);
}

void VulkanSessionGraphics::PollCaptures()
{
	if (m_observer)
		m_observer->Poll();
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
	if (m_observer && m_observer->Close(static_cast<std::uint64_t>(remaining().count())) != VK_SUCCESS)
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
	m_observer.reset();
	m_inspection.reset();
	m_validation.reset();
	m_backend.reset();
	m_closeStage = CloseStage::Closed;
	return XR_SUCCESS;
}
} // namespace simxr
