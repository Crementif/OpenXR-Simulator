#include "platform/windows/OpenGLSwapchain.h"
#include "Pch.h"

#include "platform/windows/OpenGLContextScope.h"
#include <GL/glext.h>

#include <bit>
#include <limits>

namespace simxr::windows
{
template <class Function> static Function FindProcedure(const char* name) noexcept
{
	const auto procedure = wglGetProcAddress(name);
	const auto address = std::bit_cast<std::uintptr_t>(procedure);
	if (address <= 3 || address == UINTPTR_MAX)
		return nullptr;
	return std::bit_cast<Function>(procedure);
}

static XrResult Validate(const XrSwapchainCreateInfo& description) noexcept
{
	if (description.type != XR_TYPE_SWAPCHAIN_CREATE_INFO || !description.width || !description.height ||
		!description.arraySize || !description.mipCount || !description.sampleCount || !description.usageFlags ||
		(description.faceCount != 1 && description.faceCount != 6))
		return XR_ERROR_VALIDATION_FAILURE;
	constexpr auto maxDimension = static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max());
	if (description.width > maxDimension || description.height > maxDimension || description.arraySize > maxDimension)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	const auto largestDimension = std::max(description.width, description.height);
	if (!std::has_single_bit(description.sampleCount) || description.sampleCount > 64 ||
		(description.sampleCount > 1 && description.mipCount != 1) ||
		description.mipCount > static_cast<std::uint32_t>(std::bit_width(largestDimension)))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto formats = OpenGLSwapchain::Formats();
	if (std::find(formats.begin(), formats.end(), description.format) == formats.end())
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	constexpr XrSwapchainUsageFlags supportedUsage =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
		XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	if ((description.createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
		(description.usageFlags & ~supportedUsage) || description.faceCount != 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	return XR_SUCCESS;
}

static XrResult SampleCounts(GLenum format, GLenum target, std::uint32_t& output)
{
	const auto query = FindProcedure<PFNGLGETINTERNALFORMATIVPROC>("glGetInternalformativ");
	if (!query)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	GLint count = 0;
	query(target, format, GL_NUM_SAMPLE_COUNTS, 1, &count);
	std::array<GLint, 32> samples{};
	if (count < 0 || count > static_cast<GLint>(samples.size()))
		return XR_ERROR_RUNTIME_FAILURE;
	if (count)
		query(target, format, GL_SAMPLES, count, samples.data());
	if (glGetError() != GL_NO_ERROR)
		return XR_ERROR_RUNTIME_FAILURE;
	output = 1;
	for (const auto sample : samples)
		if (sample > 1 && sample <= 64 && std::has_single_bit(static_cast<std::uint32_t>(sample)))
			output |= static_cast<std::uint32_t>(sample);
	return XR_SUCCESS;
}

struct OpenGLCapabilityContext
{
	HWND window = nullptr;
	HDC device = nullptr;
	HGLRC context = nullptr;
	~OpenGLCapabilityContext()
	{
		if (context && wglGetCurrentContext() == context)
			wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (device)
			ReleaseDC(window, device);
		if (window)
			DestroyWindow(window);
	}
};

XrResult OpenGLSwapchain::MaxSampleCount(std::uint32_t& output)
{
	OpenGLCapabilityContext owned;
	owned.window = CreateWindowExW(0, L"STATIC", L"OpenXR capability query", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
								   GetModuleHandleW(nullptr), nullptr);
	if (!owned.window || !(owned.device = GetDC(owned.window)))
		return XR_ERROR_RUNTIME_FAILURE;
	PIXELFORMATDESCRIPTOR format{};
	format.nSize = sizeof(format);
	format.nVersion = 1;
	format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
	format.iPixelType = PFD_TYPE_RGBA;
	format.cColorBits = 32;
	const auto selected = ChoosePixelFormat(owned.device, &format);
	if (!selected || !SetPixelFormat(owned.device, selected, &format) ||
		!(owned.context = wglCreateContext(owned.device)))
		return XR_ERROR_RUNTIME_FAILURE;
	OpenGLContextScope current(owned.device, owned.context);
	if (!current.Current())
		return XR_ERROR_RUNTIME_FAILURE;
	std::uint32_t common = 127;
	for (const auto format : Formats())
		for (const auto target : {GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY})
		{
			std::uint32_t samples = 1;
			const auto result = SampleCounts(static_cast<GLenum>(format), target, samples);
			if (XR_FAILED(result))
				return result;
			common &= samples;
		}
	if (!current.Restore())
		return XR_ERROR_RUNTIME_FAILURE;
	output = std::bit_floor(common);
	return XR_SUCCESS;
}

std::span<const std::int64_t> OpenGLSwapchain::Formats() noexcept
{
	static constexpr std::array<std::int64_t, 8> s_formats{0x8C43, 0x8058, 0x881A, 0x8814,
														   0x8059, 0x8CAC, 0x88F0, 0x81A5};
	return s_formats;
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
	GLint maxSize = 0;
	GLint maxLayers = 1;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
	if (arrayTexture)
		glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
	if (maxSize <= 0 || maxLayers <= 0 || description.width > static_cast<std::uint32_t>(maxSize) ||
		description.height > static_cast<std::uint32_t>(maxSize) ||
		description.arraySize > static_cast<std::uint32_t>(maxLayers))
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	const bool multisampled = description.sampleCount > 1;
	const GLenum target = multisampled ? (arrayTexture ? GL_TEXTURE_2D_MULTISAMPLE_ARRAY : GL_TEXTURE_2D_MULTISAMPLE)
									   : (arrayTexture ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
	if (multisampled)
	{
		std::uint32_t samples = 1;
		const auto supported = SampleCounts(m_format, target, samples);
		if (XR_FAILED(supported))
			return supported;
		if (!(samples & description.sampleCount))
			return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	const auto create = FindProcedure<PFNGLCREATETEXTURESPROC>("glCreateTextures");
	const auto storage2D = FindProcedure<PFNGLTEXTURESTORAGE2DPROC>("glTextureStorage2D");
	const auto storage3D = FindProcedure<PFNGLTEXTURESTORAGE3DPROC>("glTextureStorage3D");
	const auto storage2DMs = FindProcedure<PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC>("glTextureStorage2DMultisample");
	const auto storage3DMs = FindProcedure<PFNGLTEXTURESTORAGE3DMULTISAMPLEPROC>("glTextureStorage3DMultisample");
	const auto parameter = FindProcedure<PFNGLTEXTUREPARAMETERIPROC>("glTextureParameteri");
	if (!create || !storage2D || !storage3D || !storage2DMs || !storage3DMs || !parameter)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	create(target, static_cast<GLsizei>(images.size()), images.data());
	for (const auto image : images)
	{
		if (multisampled && arrayTexture)
			storage3DMs(image, description.sampleCount, m_format, description.width, description.height,
						description.arraySize, GL_TRUE);
		else if (multisampled)
			storage2DMs(image, description.sampleCount, m_format, description.width, description.height, GL_TRUE);
		else if (arrayTexture)
			storage3D(image, description.mipCount, m_format, description.width, description.height,
					  description.arraySize);
		else
			storage2D(image, description.mipCount, m_format, description.width, description.height);
		if (!multisampled)
		{
			parameter(image, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			parameter(image, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
	}
	const auto error = glGetError();
	if (error != GL_NO_ERROR)
	{
		glDeleteTextures(static_cast<GLsizei>(images.size()), images.data());
		return error == GL_OUT_OF_MEMORY ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
	}
	m_images.swap(images);
	return context.Restore() ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
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
