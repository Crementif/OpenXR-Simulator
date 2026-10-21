#pragma once

#include "graphics/LayerBuilder.h"
#include "platform/windows/OpenGLContextScope.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <GL/gl.h>

#include <array>
#include <cstdint>
#include <vector>

namespace simxr::windows
{
// Scoped compatibility bridge, not an asynchronous GPU transport. The borrowed
// context and every changed pack/texture binding are restored on all exits.
class OpenGLPreviewReadback final
{
  public:
	OpenGLPreviewReadback(HDC device, HGLRC context) noexcept;
	~OpenGLPreviewReadback();
	OpenGLPreviewReadback(const OpenGLPreviewReadback&) = delete;
	OpenGLPreviewReadback& operator=(const OpenGLPreviewReadback&) = delete;
	bool Valid() const noexcept;
	std::vector<std::uint8_t> Read(GLuint texture, std::uint32_t arrayLayers, const LayerImageDescription& image,
								   XrExtent2Di extent) const;

  private:
	using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
	struct PackState
	{
		GLenum parameter;
		GLint previous = 0;
	};
	OpenGLContextScope m_context;
	std::array<PackState, 6> m_pack;
	GLint m_texture2D = 0;
	GLint m_textureArray = 0;
	GLint m_packBuffer = 0;
	BindBuffer m_bindBuffer = nullptr;
	bool m_acquired = false;
	bool m_valid = false;
};
} // namespace simxr::windows
