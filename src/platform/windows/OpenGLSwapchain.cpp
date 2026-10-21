#include "platform/windows/OpenGLSwapchain.h"

#include "platform/windows/OpenGLContextScope.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

namespace simxr::windows
{
namespace
{
constexpr GLenum TextureArray = 0x8C1A;
constexpr GLenum TextureArrayBinding = 0x8C1D;
constexpr GLenum MaxArrayLayers = 0x88FF;
constexpr GLenum UnpackBuffer = 0x88EC;
constexpr GLenum UnpackBufferBinding = 0x88EF;
constexpr GLenum UnpackImageHeight = 0x806E;
constexpr GLenum UnpackSkipImages = 0x806D;
using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
using TextureImage3D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
									   const void*);

template <class Function> Function FindProcedure(const char* name) noexcept
{
	const auto procedure = wglGetProcAddress(name);
	const auto address = std::bit_cast<std::uintptr_t>(procedure);
	if (address <= 3 || address == UINTPTR_MAX)
		return nullptr;
	return std::bit_cast<Function>(procedure);
}

class TextureUploadScope final
{
  public:
	explicit TextureUploadScope(bool arrayTexture) noexcept : m_arrayTexture(arrayTexture)
	{
		m_bindBuffer = FindProcedure<BindBuffer>("glBindBuffer");
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture2D);
		if (m_arrayTexture)
			glGetIntegerv(TextureArrayBinding, &m_textureArray);
		glGetIntegerv(UnpackBufferBinding, &m_unpackBuffer);
		for (auto& parameter : m_unpack)
			glGetIntegerv(parameter.name, &parameter.previous);
		if (m_unpackBuffer && !m_bindBuffer)
			return;
		if (m_bindBuffer)
			m_bindBuffer(UnpackBuffer, 0);
		for (const auto& parameter : m_unpack)
			glPixelStorei(parameter.name, parameter.name == GL_UNPACK_ALIGNMENT ? 1 : 0);
		m_valid = true;
	}
	TextureUploadScope(const TextureUploadScope&) = delete;
	TextureUploadScope& operator=(const TextureUploadScope&) = delete;
	~TextureUploadScope()
	{
		if (!m_valid)
			return;
		for (const auto& parameter : m_unpack)
			glPixelStorei(parameter.name, parameter.previous);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_texture2D));
		if (m_arrayTexture)
			glBindTexture(TextureArray, static_cast<GLuint>(m_textureArray));
		if (m_bindBuffer)
			m_bindBuffer(UnpackBuffer, static_cast<GLuint>(m_unpackBuffer));
	}
	bool Valid() const noexcept
	{
		return m_valid;
	}

  private:
	struct Parameter
	{
		GLenum name;
		GLint previous = 0;
	};
	const bool m_arrayTexture;
	std::array<Parameter, 8> m_unpack{{{GL_UNPACK_ALIGNMENT},
									   {GL_UNPACK_ROW_LENGTH},
									   {GL_UNPACK_SKIP_ROWS},
									   {GL_UNPACK_SKIP_PIXELS},
									   {GL_UNPACK_SWAP_BYTES},
									   {GL_UNPACK_LSB_FIRST},
									   {UnpackImageHeight},
									   {UnpackSkipImages}}};
	BindBuffer m_bindBuffer = nullptr;
	GLint m_texture2D = 0;
	GLint m_textureArray = 0;
	GLint m_unpackBuffer = 0;
	bool m_valid = false;
};

struct PixelFormat
{
	GLenum channels;
	GLenum type;
};

std::optional<PixelFormat> DescribeFormat(std::int64_t format) noexcept
{
	switch (format)
	{
	case 0x8058: // RGBA8
	case 0x8C43: // SRGB8_ALPHA8
	case 0x881A: // RGBA16F
	case 0x8814: // RGBA32F
	case 0x8059: // RGB10_A2
		return PixelFormat{GL_RGBA, GL_UNSIGNED_BYTE};
	case 0x8CAC: // DEPTH_COMPONENT32F
	case 0x81A5: // DEPTH_COMPONENT16
		return PixelFormat{GL_DEPTH_COMPONENT, GL_FLOAT};
	case 0x88F0: // DEPTH24_STENCIL8
		return PixelFormat{0x84F9, 0x84FA};
	default:
		return {};
	}
}

XrResult Validate(const XrSwapchainCreateInfo& description) noexcept
{
	if (description.type != XR_TYPE_SWAPCHAIN_CREATE_INFO || !description.width || !description.height ||
		!description.arraySize || !description.mipCount || !description.sampleCount || !description.usageFlags ||
		(description.faceCount != 1 && description.faceCount != 6))
		return XR_ERROR_VALIDATION_FAILURE;
	constexpr auto maxDimension = static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max());
	if (description.width > maxDimension || description.height > maxDimension || description.arraySize > maxDimension ||
		description.sampleCount != 1)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	const auto largestDimension = std::max(description.width, description.height);
	if (description.mipCount > static_cast<std::uint32_t>(std::bit_width(largestDimension)))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!DescribeFormat(description.format))
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	constexpr XrSwapchainUsageFlags supportedUsage =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
		XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	if ((description.createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
		(description.usageFlags & ~supportedUsage) || description.faceCount != 1 || description.sampleCount != 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	return XR_SUCCESS;
}
} // namespace

std::span<const std::int64_t> OpenGLSwapchain::Formats() noexcept
{
	static constexpr std::array<std::int64_t, 8> formats{0x8C43, 0x8058, 0x881A, 0x8814,
														 0x8059, 0x8CAC, 0x88F0, 0x81A5};
	return formats;
}

OpenGLSwapchain::OpenGLSwapchain(HDC device, HGLRC context, GLenum format)
	: m_device(device), m_context(context), m_format(format)
{
}

XrResult OpenGLSwapchain::Create(HDC device, HGLRC context, const XrSwapchainCreateInfo& description,
								 std::unique_ptr<OpenGLSwapchain>& output)
{
	const auto validation = Validate(description);
	if (XR_FAILED(validation))
		return validation;
	if (output)
		return XR_ERROR_CALL_ORDER_INVALID;
	auto prepared =
		std::unique_ptr<OpenGLSwapchain>(new OpenGLSwapchain(device, context, static_cast<GLenum>(description.format)));
	const auto result = prepared->Allocate(description);
	if (XR_FAILED(result))
		return result;
	output.swap(prepared);
	return XR_SUCCESS;
}

XrResult OpenGLSwapchain::Allocate(const XrSwapchainCreateInfo& description)
{
	const auto imageCount = (description.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1u : 3u;
	// Reserve CPU bookkeeping before native acquisition; every generated name is
	// registered immediately so all subsequent failures have the same rollback.
	std::vector<GLuint> images(imageCount, 0);
	OpenGLContextScope context(m_device, m_context);
	if (!context.Current())
		return XR_ERROR_RUNTIME_FAILURE;
	// GL has no error-queue save/restore API. Reject one outstanding host error
	// rather than misattribute it to successful storage allocation.
	if (glGetError() != GL_NO_ERROR)
		return XR_ERROR_RUNTIME_FAILURE;
	const bool arrayTexture = description.arraySize > 1;
	const auto image3D = FindProcedure<TextureImage3D>("glTexImage3D");
	if (arrayTexture && !image3D)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	GLint maxSize = 0;
	GLint maxLayers = 1;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
	if (arrayTexture)
		glGetIntegerv(MaxArrayLayers, &maxLayers);
	if (maxSize <= 0 || maxLayers <= 0 || description.width > static_cast<std::uint32_t>(maxSize) ||
		description.height > static_cast<std::uint32_t>(maxSize) ||
		description.arraySize > static_cast<std::uint32_t>(maxLayers))
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	XrResult result = XR_SUCCESS;
	{
		TextureUploadScope upload(arrayTexture);
		if (!upload.Valid())
			return XR_ERROR_RUNTIME_FAILURE;
		const auto pixelFormat = *DescribeFormat(description.format);
		const GLenum target = arrayTexture ? TextureArray : GL_TEXTURE_2D;
		for (auto& image : images)
		{
			glGenTextures(1, &image);
			if (!image)
			{
				result = XR_ERROR_OUT_OF_MEMORY;
				break;
			}
			glBindTexture(target, image);
			for (std::uint32_t level = 0; level < description.mipCount; ++level)
			{
				const auto width = static_cast<GLsizei>(std::max(1u, description.width >> level));
				const auto height = static_cast<GLsizei>(std::max(1u, description.height >> level));
				if (arrayTexture)
					image3D(target, static_cast<GLint>(level), static_cast<GLint>(m_format), width, height,
							static_cast<GLsizei>(description.arraySize), 0, pixelFormat.channels, pixelFormat.type,
							nullptr);
				else
					glTexImage2D(target, static_cast<GLint>(level), static_cast<GLint>(m_format), width, height, 0,
								 pixelFormat.channels, pixelFormat.type, nullptr);
			}
			glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			const auto error = glGetError();
			if (error != GL_NO_ERROR)
			{
				result = error == GL_OUT_OF_MEMORY ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
				break;
			}
		}
		if (XR_FAILED(result))
			glDeleteTextures(static_cast<GLsizei>(images.size()), images.data());
		else
			m_images.swap(images);
	}
	if (!context.Restore())
		return XR_ERROR_RUNTIME_FAILURE;
	return result;
}

OpenGLSwapchain::~OpenGLSwapchain()
{
	Close();
}

std::span<const GLuint> OpenGLSwapchain::Images() const noexcept
{
	return m_images;
}

GLenum OpenGLSwapchain::Format() const noexcept
{
	return m_format;
}

bool OpenGLSwapchain::Close() noexcept
{
	if (m_images.empty())
		return true;
	OpenGLContextScope context(m_device, m_context);
	if (!context.Current())
		return false;
	glDeleteTextures(static_cast<GLsizei>(m_images.size()), m_images.data());
	m_images.clear();
	return context.Restore();
}
} // namespace simxr::windows
