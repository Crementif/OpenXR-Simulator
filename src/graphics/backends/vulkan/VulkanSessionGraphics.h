#pragma once

#include "control/ImageInspection.h"
#include "graphics/SwapchainWaits.h"
#include "graphics/backends/vulkan/VulkanDevice.h"
#include "presentation/FrameTypes.h"
#include "runtime/HandleRegistry.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace simxr
{
class Session;
class VulkanBackend;
class VulkanCaptureBindings;
class VulkanInspection;
class VulkanFrameObserver;
class VulkanSwapchain;
struct LegacyCaptureRequest;
struct CompositionPlan;
struct VulkanInspectionValidation;

// Native operations require the adapter's queue/binding serialization. BeginClose
// closes native admission under that gate; the caller closes the core session
// and drains outside it. FinishClose retires native children before the backend.
// Capabilities are immutable.
class VulkanSessionGraphics final
{
  public:
	using SwapchainRef = HandleRef<XrSwapchain, VulkanSwapchain>;

	static XrResult Create(const VulkanBinding& binding, std::shared_ptr<Session> session, bool validation,
						   std::unique_ptr<VulkanSessionGraphics>& output);
	~VulkanSessionGraphics();
	VulkanSessionGraphics(const VulkanSessionGraphics&) = delete;
	VulkanSessionGraphics& operator=(const VulkanSessionGraphics&) = delete;
	std::shared_ptr<VulkanDevice> Device() const;
	void SetObserver(std::unique_ptr<VulkanFrameObserver> observer);
	control::ImageCapabilities Capabilities() const noexcept;
	bool SupportsReprojection() const noexcept;
	XrResult CreateSwapchain(const XrSwapchainCreateInfo& description, XrSwapchain& output);
	SwapchainRef FindSwapchain(XrSwapchain handle) const;
	void DestroySwapchain(SwapchainRef swapchain);
	XrResult Acquire(VulkanSwapchain& swapchain, std::uint32_t& index);
	std::optional<SwapchainWaits::Registration> EnterWait();
	XrResult EndFrame(const XrFrameEndInfo& info);
	void RequestCapture(LegacyCaptureRequest request, FrameId frame);
	void PollCaptures();
	void BeginClose();
	XrResult Drain(std::chrono::nanoseconds timeout = std::chrono::seconds(5));
	XrResult FinishClose();

  private:
	class FrameBindings;

	enum class CloseStage
	{
		Pending,
		Drained,
		Closed
	};

	VulkanSessionGraphics(const VulkanBinding& binding, std::shared_ptr<Session> session, bool validation);
	XrResult ConfigureCapture();
	XrResult GraphicsFailure(VkResult result);

	const std::shared_ptr<Session> m_session;
	std::unique_ptr<VulkanBackend> m_backend;
	HandleRegistry<XrSwapchain, VulkanSwapchain, const Session*> m_swapchains;
	std::shared_ptr<VulkanInspectionValidation> m_validation;
	std::unique_ptr<VulkanInspection> m_inspection;
	std::unique_ptr<VulkanFrameObserver> m_observer;
	control::ImageCapabilities m_capabilities;
	SwapchainWaits m_waits;
	std::mutex m_closeMutex;
	CloseStage m_closeStage = CloseStage::Pending;
	bool m_closing = false;
};
} // namespace simxr
