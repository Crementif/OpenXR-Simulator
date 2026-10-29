#pragma once

#include "graphics/Compositor.h"
#include "graphics/FrameOutput.h"
#include "graphics/FramePool.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <windows.h>

#include <GL/gl.h>

namespace simxr
{
struct OpenGLCompositionSource
{
	GLuint texture = 0;
	GLenum format = GL_RGBA;
	std::uint32_t width = 0, height = 0, layers = 1, mipLevels = 1, samples = 1;
};

class OpenGLContext;
class OpenGLSharedImage;

class OpenGLFrame final : public FrameOutput
{
public:
	~OpenGLFrame() override;
	CompletionStatus PollCompletion() const noexcept override;
	// Queued reads still require source retirement when Submit fails.
	bool SourcesUsed() const noexcept;
	std::shared_ptr<VulkanImage> Image() const noexcept;
	VkSemaphore Ready() const noexcept;
	static constexpr std::uint64_t ReadyValue = 0;

private:
	friend class OpenGLBackend;
	struct Resources;
	OpenGLFrame(std::shared_ptr<OpenGLContext> context, std::span<const FrameView> views);
	std::unique_ptr<Resources> m_resources;
};

class OpenGLBackend final
{
public:
	using Frame = OpenGLFrame;
	using Source = OpenGLCompositionSource;
	OpenGLBackend(HDC applicationDevice, HGLRC applicationContext, std::shared_ptr<VulkanDevice> delivery,
				  std::span<const char* const> extensions);
	~OpenGLBackend();
	// Validate application features before allocating the native delivery device.
	static VulkanExternalIdentity Identity(HDC device, HGLRC context);
	XrResult Prepare(const CompositionPlan& plan, std::span<const OpenGLCompositionSource> sources,
					 std::shared_ptr<OpenGLFrame>& output, std::uint32_t outputLayers = 2);
	XrResult PrepareCapture(const LayerImageDescription& image, OpenGLCompositionSource source,
							std::shared_ptr<OpenGLFrame>& output, bool retainMipChain = false);
	XrResult Submit(const std::shared_ptr<OpenGLFrame>& frame);

private:
	XrResult PrepareImage(const CompositionPlan& plan, std::span<const OpenGLCompositionSource> sources,
						  const VulkanExternalImageSpec& description, GLenum format, bool retained,
						  std::shared_ptr<OpenGLFrame>& output);
	const HDC m_applicationDevice;
	const HGLRC m_applicationContext;
	std::shared_ptr<OpenGLContext> m_context;
	VulkanExternalDevice m_external;
	std::array<std::shared_ptr<OpenGLSharedImage>, 8> m_images;
	FramePool m_frames{8};
	FramePool m_retainedFrames{32};
	struct PendingFrames;
	std::unique_ptr<PendingFrames> m_pending;
};
} // namespace simxr
