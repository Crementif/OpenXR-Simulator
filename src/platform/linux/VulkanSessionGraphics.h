#pragma once

#include "control/ImageInspection.h"
#include "graphics/SwapchainWaits.h"
#include "graphics/backends/vulkan/VulkanDevice.h"
#include "presentation/FrameTypes.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace flicker
{
class Diagnostics;
}

namespace simxr
{
class LegacyFileChannel;
class Session;
class VulkanBackend;
class VulkanCaptureBindings;
class VulkanFrameBindings;
class VulkanInspection;
class VulkanLegacyCapture;
class VulkanSwapchain;
struct LegacyCaptureRequest;
struct CompositionPlan;
struct VulkanInspectionValidation;

// Native operations require the adapter's queue/binding serialization. BeginClose
// closes CPU admission under that gate; Drain waits outside it. FinishClose runs
// after native child retirement. Capabilities are immutable after construction.
class VulkanSessionGraphics final
{
  public:
	static XrResult Create(const VulkanBinding& binding, std::shared_ptr<Session> session,
						   std::shared_ptr<LegacyFileChannel> files, std::shared_ptr<flicker::Diagnostics> diagnostics,
						   bool validation, std::unique_ptr<VulkanSessionGraphics>& output);
	~VulkanSessionGraphics();
	VulkanSessionGraphics(const VulkanSessionGraphics&) = delete;
	VulkanSessionGraphics& operator=(const VulkanSessionGraphics&) = delete;
	control::ImageCapabilities Capabilities() const noexcept;
	XrResult CreateSwapchain(const XrSwapchainCreateInfo& description, std::unique_ptr<VulkanSwapchain>& output);
	XrResult Acquire(VulkanSwapchain& swapchain, std::uint32_t& index);
	std::optional<SwapchainWaits::Registration> EnterWait();
	XrResult EndFrame(const XrFrameEndInfo& info, const VulkanFrameBindings& frames,
					  const VulkanCaptureBindings& captures);
	void RequestCapture(LegacyCaptureRequest request, FrameId frame);
	void PollCaptures();
	void BeginClose();
	XrResult Drain(std::chrono::nanoseconds timeout = std::chrono::seconds(5));
	XrResult FinishClose();

  private:
	enum class CloseStage
	{
		Pending,
		Drained,
		Closed
	};

	VulkanSessionGraphics(const VulkanBinding& binding, std::shared_ptr<Session> session,
						  std::shared_ptr<LegacyFileChannel> files, std::shared_ptr<flicker::Diagnostics> diagnostics,
						  bool validation);
	XrResult ConfigureCapture();
	XrResult GraphicsFailure(VkResult result);
	void ObserveFrame(const XrFrameEndInfo& info, const CompositionPlan& composition,
					  const VulkanCaptureBindings& bindings) noexcept;
	void LogObserverFailure(const char* detail) noexcept;

	const std::shared_ptr<Session> m_session;
	const std::shared_ptr<LegacyFileChannel> m_files;
	const std::shared_ptr<flicker::Diagnostics> m_diagnostics;
	std::unique_ptr<VulkanBackend> m_backend;
	std::shared_ptr<VulkanInspectionValidation> m_validation;
	std::unique_ptr<VulkanInspection> m_inspection;
	std::unique_ptr<VulkanLegacyCapture> m_capture;
	control::ImageCapabilities m_capabilities;
	SwapchainWaits m_waits;
	std::mutex m_closeMutex;
	CloseStage m_closeStage = CloseStage::Pending;
};
} // namespace simxr
