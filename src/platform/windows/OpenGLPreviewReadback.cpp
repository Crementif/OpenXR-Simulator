#include "platform/windows/OpenGLPreviewReadback.h"

#include "platform/windows/PreviewCapturePixels.h"

#include <bit>
#include <span>
#include <stdexcept>

namespace simxr::windows
{
namespace
{
constexpr GLenum TextureArray = 0x8C1A;
constexpr GLenum TextureArrayBinding = 0x8C1D;
constexpr GLenum TextureDepth = 0x8071;
constexpr GLenum PixelPackBuffer = 0x88EB;
constexpr GLenum PixelPackBufferBinding = 0x88ED;
constexpr GLenum PackImageHeight = 0x806C;
constexpr GLenum PackSkipImages = 0x806B;
} // namespace

OpenGLPreviewReadback::OpenGLPreviewReadback(HDC device, HGLRC context) noexcept
	: m_context(device, context), m_pack{{{GL_PACK_ALIGNMENT},
										  {GL_PACK_ROW_LENGTH},
										  {GL_PACK_SKIP_ROWS},
										  {GL_PACK_SKIP_PIXELS},
										  {PackImageHeight},
										  {PackSkipImages}}}
{
	if (!m_context.Current())
		return;
	m_acquired = true;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture2D);
	glGetIntegerv(TextureArrayBinding, &m_textureArray);
	glGetIntegerv(PixelPackBufferBinding, &m_packBuffer);
	for (auto& state : m_pack)
		glGetIntegerv(state.parameter, &state.previous);
	const auto bindBuffer = wglGetProcAddress("glBindBuffer");
	const auto address = std::bit_cast<std::uintptr_t>(bindBuffer);
	if (address > 3 && address != UINTPTR_MAX)
		m_bindBuffer = std::bit_cast<BindBuffer>(bindBuffer);
	if (m_packBuffer && !m_bindBuffer)
		return;
	if (m_bindBuffer)
		m_bindBuffer(PixelPackBuffer, 0);
	for (const auto& state : m_pack)
		glPixelStorei(state.parameter, state.parameter == GL_PACK_ALIGNMENT ? 1 : 0);
	m_valid = true;
	glFinish();
}

OpenGLPreviewReadback::~OpenGLPreviewReadback()
{
	if (!m_acquired)
		return;
	for (const auto& state : m_pack)
		glPixelStorei(state.parameter, state.previous);
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_texture2D));
	glBindTexture(TextureArray, static_cast<GLuint>(m_textureArray));
	if (m_bindBuffer)
		m_bindBuffer(PixelPackBuffer, static_cast<GLuint>(m_packBuffer));
}

bool OpenGLPreviewReadback::Valid() const noexcept
{
	return m_valid;
}

std::vector<std::uint8_t> OpenGLPreviewReadback::Read(GLuint texture, std::uint32_t arrayLayers,
													  const LayerImageDescription& image, XrExtent2Di extent) const
{
	if (!m_valid || !texture || !arrayLayers || image.arraySlice >= arrayLayers || extent.width <= 0 ||
		extent.height <= 0)
		throw std::invalid_argument("OpenGL preview requires a valid context and source slice");
	const GLenum target = arrayLayers == 1 ? GL_TEXTURE_2D : TextureArray;
	glBindTexture(target, texture);
	GLint width = 0;
	GLint height = 0;
	GLint layers = 1;
	glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(target, 0, GL_TEXTURE_HEIGHT, &height);
	if (arrayLayers != 1)
		glGetTexLevelParameteriv(target, 0, TextureDepth, &layers);
	if (width != extent.width || height != extent.height || layers <= 0 ||
		static_cast<std::uint32_t>(layers) != arrayLayers)
		throw std::invalid_argument("OpenGL preview texture storage differs from its accepted source metadata");
	const auto pixelsPerSlice = std::uint64_t(width) * height;
	if (pixelsPerSlice > PreviewCaptureLayout::MaxBytes / 4 / arrayLayers)
		throw std::invalid_argument("OpenGL preview source exceeds its bounded compatibility readback");
	const auto bytesPerSlice = static_cast<std::size_t>(pixelsPerSlice * 4);
	std::vector<std::uint8_t> pixels(bytesPerSlice * arrayLayers);
	glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	// One error observation, never a blanket drain. An outstanding application
	// error also makes this compatibility capture fail rather than publish black.
	if (glGetError() != GL_NO_ERROR)
		throw std::runtime_error("OpenGL preview readback failed or observed an outstanding context error");
	const auto selected =
		std::span<const std::uint8_t>(pixels).subspan(bytesPerSlice * image.arraySlice, bytesPerSlice);
	const auto& rectangle = image.rectangle;
	return CopyPreviewRgbaCrop(
		selected, static_cast<std::uint32_t>(extent.width), static_cast<std::uint32_t>(extent.height),
		{static_cast<std::uint32_t>(rectangle.offset.x), static_cast<std::uint32_t>(rectangle.offset.y),
		 static_cast<std::uint32_t>(rectangle.extent.width), static_cast<std::uint32_t>(rectangle.extent.height)},
		true);
}
} // namespace simxr::windows
