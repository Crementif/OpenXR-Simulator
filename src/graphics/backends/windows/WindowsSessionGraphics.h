#pragma once

#include "control/ImageInspection.h"
#include "graphics/SessionComposition.h"
#include "graphics/backends/d3d11/D3D11Backend.h"
#include "graphics/backends/d3d12/D3D12Backend.h"
#include "graphics/backends/opengl/OpenGLBackend.h"
#include <variant>

namespace simxr
{
class VulkanBackend;
class VulkanInspection;
class VulkanInspectionDevice;
class WindowsSessionGraphics
{
  public:
	using Source = std::variant<D3D11CompositionSource, D3D12CompositionSource, OpenGLCompositionSource>;
	using ImageResolver = std::function<Source(XrSwapchain)>;
	WindowsSessionGraphics(ID3D11Device* device, std::shared_ptr<Session> session);
	WindowsSessionGraphics(ID3D12Device* device, ID3D12CommandQueue* queue, std::shared_ptr<Session> session);
	WindowsSessionGraphics(HDC device, HGLRC context, std::shared_ptr<Session> session);
	~WindowsSessionGraphics();
	XrResult EndFrame(const XrFrameEndInfo& info, const SwapchainDescriptionResolver& describe,
		const ImageResolver& resolve);
	control::ImageCapabilities Capabilities() const noexcept;
	XrResult Close();

  private:
	void Configure(const VulkanExternalIdentity& identity);
	const std::shared_ptr<Session> m_session;
	std::variant<std::unique_ptr<D3D11Backend>, std::unique_ptr<D3D12Backend>, std::unique_ptr<OpenGLBackend>> m_native;
	std::unique_ptr<VulkanInspectionDevice> m_device;
	std::unique_ptr<VulkanBackend> m_delivery;
	std::unique_ptr<VulkanInspection> m_inspection;
	bool m_closing = false;
};
}
