#pragma once

#include <openxr/openxr.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <GL/gl.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace simxr::windows
{
// The application keeps the borrowed WGL context alive until Close succeeds.
// Image IDs are borrowed by the application; this owner alone deletes them.
class OpenGLSwapchain final
{
  public:
	static std::span<const std::int64_t> Formats() noexcept;
	static XrResult MaxSampleCount(std::uint32_t& output);
	// Output must be empty; every failure leaves it unchanged.
	static XrResult Create(HDC device, HGLRC context, const XrSwapchainCreateInfo& description,
						   std::unique_ptr<OpenGLSwapchain>& output);
	~OpenGLSwapchain();
	OpenGLSwapchain(const OpenGLSwapchain&) = delete;
	OpenGLSwapchain& operator=(const OpenGLSwapchain&) = delete;

	std::span<const GLuint> Images() const noexcept;
	GLenum Format() const noexcept;
	// Context acquisition failure keeps every image owned for a later Close retry.
	bool Close() noexcept;

  private:
	OpenGLSwapchain(HDC device, HGLRC context, GLenum format);
	XrResult Allocate(const XrSwapchainCreateInfo& description);

	const HDC m_device;
	const HGLRC m_context;
	const GLenum m_format;
	std::vector<GLuint> m_images;
};
} // namespace simxr::windows
