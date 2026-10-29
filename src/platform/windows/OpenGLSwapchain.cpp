#include "platform/windows/OpenGLSwapchain.h"
#include "Pch.h"

#include "graphics/FramePool.h"
#include "platform/windows/ModuleReference.h"
#include "platform/windows/OpenGLContextScope.h"
#include "platform/windows/OpenGLLibrary.h"
#include <GL/glext.h>

#include <new>

namespace simxr::windows
{
template<class Function>
static Function FindProcedure(const OpenGLLibrary& gl, const char* name) noexcept
{
	const auto procedure = gl.GetProcAddress(name);
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

static XrResult SampleCounts(const OpenGLLibrary& gl, GLenum format, GLenum target, std::uint32_t& output)
{
	const auto query = FindProcedure<PFNGLGETINTERNALFORMATIVPROC>(gl, "glGetInternalformativ");
	if (!query)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	GLint count = 0;
	query(target, format, GL_NUM_SAMPLE_COUNTS, 1, &count);
	std::array<GLint, 32> samples{};
	if (count < 0 || count > static_cast<GLint>(samples.size()))
		return XR_ERROR_RUNTIME_FAILURE;
	if (count)
		query(target, format, GL_SAMPLES, count, samples.data());
	if (gl.GetError() != GL_NO_ERROR)
		return XR_ERROR_RUNTIME_FAILURE;
	output = 1;
	for (const auto sample : samples)
		if (sample > 1 && sample <= 64 && std::has_single_bit(static_cast<std::uint32_t>(sample)))
			output |= static_cast<std::uint32_t>(sample);
	return XR_SUCCESS;
}

struct OpenGLCapabilityContext
{
	ModuleReference module;
	std::shared_ptr<FrameReservation> preservation;
	OpenGLLibrary gl;
	HWND window = nullptr;
	HDC device = nullptr;
	HGLRC context = nullptr;

	OpenGLCapabilityContext()
	{
		static const FramePool s_owners{64};
		preservation = s_owners.Reserve();
		if (!preservation)
			throw std::bad_alloc();
	}

	static void Destroy(OpenGLCapabilityContext* owned) noexcept
	{
		if (!owned->Close())
		{
			std::fputs("SimXR: preserving OpenGL capability context after native cleanup failure\n", stderr);
			return;
		}
		delete owned;
	}

	bool Close() noexcept
	{
		if (context)
		{
			if (gl.GetCurrentContext() == context && !gl.MakeCurrent(nullptr, nullptr))
				return false;
			if (!gl.DeleteContext(context))
				return false;
			context = nullptr;
		}
		if (device)
		{
			if (!ReleaseDC(window, device))
				return false;
			device = nullptr;
		}
		if (window)
		{
			if (!DestroyWindow(window))
				return false;
			window = nullptr;
		}
		return true;
	}
};

struct OpenGLSwapchain::Resources
{
	ModuleReference module;
	std::shared_ptr<FrameReservation> preservation;
	OpenGLLibrary gl;
	const HDC device;
	const HGLRC context;
	const GLenum format;
	std::vector<GLuint> images;

	Resources(HDC device, HGLRC context, GLenum format) : device(device), context(context), format(format)
	{
		static const FramePool s_owners{256};
		preservation = s_owners.Reserve();
		if (!preservation)
			throw std::bad_alloc();
	}
};

XrResult OpenGLSwapchain::MaxSampleCount(std::uint32_t& output)
{
	// Keep only the fixed system's measured limit across XR instance restarts.
	// Recreating a capability context can wait on unrelated pending GPU work.
	static std::atomic<std::uint32_t> s_maxSamples{0};
	if (const auto cached = s_maxSamples.load())
	{
		output = cached;
		return XR_SUCCESS;
	}
	std::unique_ptr<OpenGLCapabilityContext, decltype(&OpenGLCapabilityContext::Destroy)> owner(
		new OpenGLCapabilityContext, &OpenGLCapabilityContext::Destroy);
	auto& owned = *owner;
	const auto& gl = owned.gl;
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
		!(owned.context = gl.CreateContext(owned.device)))
		return XR_ERROR_RUNTIME_FAILURE;
	OpenGLContextScope current(gl, owned.device, owned.context);
	if (!current.Current())
		return XR_ERROR_RUNTIME_FAILURE;
	std::uint32_t common = 127;
	for (const auto format : Formats())
		for (const auto target : {GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_2D_MULTISAMPLE_ARRAY})
		{
			std::uint32_t samples = 1;
			const auto result = SampleCounts(gl, static_cast<GLenum>(format), target, samples);
			if (XR_FAILED(result))
				return result;
			common &= samples;
		}
	if (!current.Restore())
		return XR_ERROR_RUNTIME_FAILURE;
	output = std::bit_floor(common);
	owner.reset();
	s_maxSamples.store(output);
	return XR_SUCCESS;
}

std::span<const std::int64_t> OpenGLSwapchain::Formats() noexcept
{
	static constexpr std::array<std::int64_t, 8> s_formats{0x8C43, 0x8058, 0x881A, 0x8814,
														   0x8059, 0x8CAC, 0x88F0, 0x81A5};
	return s_formats;
}

OpenGLSwapchain::OpenGLSwapchain(HDC device, HGLRC context, GLenum format)
	: m_resources(std::make_unique<Resources>(device, context, format))
{}

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
	auto& resources = *m_resources;
	const auto& gl = resources.gl;
	const auto imageCount = (description.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1u : 3u;
	// Reserve CPU bookkeeping before native acquisition; every generated name is
	// registered immediately so all subsequent failures have the same rollback.
	std::vector<GLuint> images(imageCount, 0);
	OpenGLContextScope context(gl, resources.device, resources.context);
	if (!context.Current())
		return XR_ERROR_RUNTIME_FAILURE;
	// GL has no error-queue save/restore API. Reject one outstanding host error
	// rather than misattribute it to successful storage allocation.
	if (gl.GetError() != GL_NO_ERROR)
		return XR_ERROR_RUNTIME_FAILURE;
	const bool arrayTexture = description.arraySize > 1;
	GLint maxSize = 0;
	GLint maxLayers = 1;
	gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
	if (arrayTexture)
		gl.GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
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
		const auto supported = SampleCounts(gl, resources.format, target, samples);
		if (XR_FAILED(supported))
			return supported;
		if (!(samples & description.sampleCount))
			return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	const auto create = FindProcedure<PFNGLCREATETEXTURESPROC>(gl, "glCreateTextures");
	const auto storage2D = FindProcedure<PFNGLTEXTURESTORAGE2DPROC>(gl, "glTextureStorage2D");
	const auto storage3D = FindProcedure<PFNGLTEXTURESTORAGE3DPROC>(gl, "glTextureStorage3D");
	const auto storage2DMs = FindProcedure<PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC>(gl, "glTextureStorage2DMultisample");
	const auto storage3DMs = FindProcedure<PFNGLTEXTURESTORAGE3DMULTISAMPLEPROC>(gl, "glTextureStorage3DMultisample");
	const auto parameter = FindProcedure<PFNGLTEXTUREPARAMETERIPROC>(gl, "glTextureParameteri");
	if (!create || !storage2D || !storage3D || !storage2DMs || !storage3DMs || !parameter)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	create(target, static_cast<GLsizei>(images.size()), images.data());
	for (const auto image : images)
	{
		if (multisampled && arrayTexture)
			storage3DMs(image, description.sampleCount, resources.format, description.width, description.height,
						description.arraySize, GL_TRUE);
		else if (multisampled)
			storage2DMs(image, description.sampleCount, resources.format, description.width, description.height,
						GL_TRUE);
		else if (arrayTexture)
			storage3D(image, description.mipCount, resources.format, description.width, description.height,
					  description.arraySize);
		else
			storage2D(image, description.mipCount, resources.format, description.width, description.height);
		if (!multisampled)
		{
			parameter(image, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			parameter(image, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
	}
	const auto error = gl.GetError();
	if (error != GL_NO_ERROR)
	{
		gl.DeleteTextures(static_cast<GLsizei>(images.size()), images.data());
		return error == GL_OUT_OF_MEMORY ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
	}
	resources.images.swap(images);
	return context.Restore() ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

OpenGLSwapchain::~OpenGLSwapchain()
{
	if (!Close() && !m_resources->images.empty())
	{
		std::fputs("SimXR: preserving OpenGL swapchain after native cleanup failure\n", stderr);
		m_resources.release(); // Keep native images, dependency and reservation when cleanup cannot acquire WGL.
	}
}

std::span<const GLuint> OpenGLSwapchain::Images() const noexcept
{
	return m_resources->images;
}

GLenum OpenGLSwapchain::Format() const noexcept
{
	return m_resources->format;
}

bool OpenGLSwapchain::Close() noexcept
{
	auto& resources = *m_resources;
	const auto& gl = resources.gl;
	if (resources.images.empty())
		return true;
	OpenGLContextScope context(gl, resources.device, resources.context);
	if (!context.Current())
		return false;
	gl.DeleteTextures(static_cast<GLsizei>(resources.images.size()), resources.images.data());
	resources.images.clear();
	return context.Restore();
}
} // namespace simxr::windows
