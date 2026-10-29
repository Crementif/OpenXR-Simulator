#include "graphics/backends/opengl/OpenGLBackend.h"
#include "Pch.h"

#include "CompositorGlsl.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "platform/windows/ModuleReference.h"
#include "platform/windows/OpenGLContextScope.h"
#include "platform/windows/OpenGLLibrary.h"
#include <GL/glext.h>
#include <GL/wglext.h>
#include <new>

namespace simxr
{
template<typename Function>
static Function Procedure(const windows::OpenGLLibrary& gl, const char* name)
{
	const auto address = gl.GetProcAddress(name);
	const auto value = std::bit_cast<std::uintptr_t>(address);
	if (value <= 3 || value == UINTPTR_MAX)
		throw std::runtime_error(std::string("Required OpenGL command unavailable: ") + name);
	return std::bit_cast<Function>(address);
}

static bool IsDepthFormat(GLenum format)
{
	return format == GL_DEPTH_COMPONENT16 || format == GL_DEPTH_COMPONENT24 || format == GL_DEPTH_COMPONENT32F ||
		   format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8;
}

static void CheckGL(const windows::OpenGLLibrary& gl, const char* operation)
{
	if (gl.GetError() != GL_NO_ERROR)
		throw std::runtime_error(operation);
}

static bool HasExtension(std::string_view extensions, std::string_view name)
{
	while (!extensions.empty())
	{
		const auto end = extensions.find(' ');
		if (extensions.substr(0, end) == name)
			return true;
		if (end == std::string_view::npos)
			break;
		extensions.remove_prefix(end + 1);
	}
	return false;
}

static void RequireApplicationCapabilities(const windows::OpenGLLibrary& gl, HDC device)
{
	GLint major = 0, minor = 0, count = 0;
	gl.GetIntegerv(GL_MAJOR_VERSION, &major);
	gl.GetIntegerv(GL_MINOR_VERSION, &minor);
	gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
	CheckGL(gl, "Cannot query application OpenGL capabilities");
	if (major < 3 || (major == 3 && minor < 3))
		throw std::runtime_error("Application OpenGL context must support version 3.3 or newer");
	if (count < 0)
		throw std::runtime_error("Invalid application OpenGL extension count");
	const auto getString = Procedure<PFNGLGETSTRINGIPROC>(gl, "glGetStringi");
	const auto supports = [&](std::string_view required) {
		for (GLint index = 0; index < count; ++index)
		{
			const auto extension = getString(GL_EXTENSIONS, index);
			if (!extension)
				throw std::runtime_error("Cannot query application OpenGL extension");
			if (required == reinterpret_cast<const char*>(extension))
				return true;
		}
		return false;
	};
	const auto require = [&](GLint coreMajor, GLint coreMinor, const char* extension) {
		if (major > coreMajor || (major == coreMajor && minor >= coreMinor) || supports(extension))
			return;
		throw std::runtime_error(std::string("Application OpenGL context requires ") + extension);
	};
	// Swapchains are allocated in the application context; shaders run in our 4.5 context.
	require(4, 5, "GL_ARB_direct_state_access");
	require(4, 2, "GL_ARB_texture_storage");
	require(4, 3, "GL_ARB_texture_storage_multisample");
	require(4, 2, "GL_ARB_internalformat_query");
	require(4, 3, "GL_ARB_internalformat_query2");
	if (!supports("GL_EXT_memory_object") && !supports("GL_EXT_semaphore"))
		throw std::runtime_error(
			"Application OpenGL context requires GL_EXT_memory_object or GL_EXT_semaphore for device identification");
	CheckGL(gl, "Cannot query application OpenGL extensions");

	const auto getWglExtensions = Procedure<PFNWGLGETEXTENSIONSSTRINGARBPROC>(gl, "wglGetExtensionsStringARB");
	const auto extensions = getWglExtensions(device);
	if (!extensions)
		throw std::runtime_error("Cannot query application WGL extensions");
	for (const std::string_view required :
		 {"WGL_ARB_create_context", "WGL_ARB_create_context_profile", "WGL_ARB_pixel_format", "WGL_ARB_pbuffer"})
	{
		if (!HasExtension(extensions, required))
			throw std::runtime_error(std::string("Application OpenGL context requires ") + std::string(required));
	}
}

// A private share-group context keeps native cleanup independent of application
// context lifetime and isolates every framebuffer, program and binding mutation.
class OpenGLContext final
{
public:
	static std::shared_ptr<OpenGLContext> Create(HDC applicationDevice, HGLRC applicationContext)
	{
		auto context = std::shared_ptr<OpenGLContext>(new OpenGLContext, [](OpenGLContext* context) {
			if (context->Close())
				delete context;
			else
				std::fputs("SimXR: preserving OpenGL context after native cleanup failure\n", stderr);
		});
		context->Initialize(applicationDevice, applicationContext);
		return context;
	}

	windows::ModuleReference m_module;
	std::shared_ptr<FrameReservation> m_preservation;
	windows::OpenGLLibrary m_gl;
	std::mutex m_mutex;
	HDC m_device = nullptr;
	HGLRC m_handle = nullptr;
	GLuint m_program = 0, m_depthMsProgram = 0, m_vertexArray = 0;
	PFNGLDELETESYNCPROC m_deleteSync = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC m_deleteFramebuffers = nullptr;
	PFNGLDELETEMEMORYOBJECTSEXTPROC m_deleteMemory = nullptr;
	PFNGLDELETESEMAPHORESEXTPROC m_deleteSemaphores = nullptr;
	PFNGLCLIENTWAITSYNCPROC m_wait = nullptr;

private:
	OpenGLContext()
	{
		static const FramePool s_owners{64};
		m_preservation = s_owners.Reserve();
		if (!m_preservation)
			throw std::bad_alloc();
	}

	void Initialize(HDC applicationDevice, HGLRC applicationContext)
	{
		const auto& gl = m_gl;
		windows::OpenGLContextScope application(gl, applicationDevice, applicationContext);
		if (!application.Current())
			throw std::runtime_error("Cannot acquire application OpenGL context");
		RequireApplicationCapabilities(gl, applicationDevice);
		const auto create = Procedure<PFNWGLCREATECONTEXTATTRIBSARBPROC>(gl, "wglCreateContextAttribsARB");
		m_releaseDevice = Procedure<PFNWGLRELEASEPBUFFERDCARBPROC>(gl, "wglReleasePbufferDCARB");
		m_destroy = Procedure<PFNWGLDESTROYPBUFFERARBPROC>(gl, "wglDestroyPbufferARB");
		const int pixelAttributes[]{WGL_DRAW_TO_PBUFFER_ARB,
									TRUE,
									WGL_SUPPORT_OPENGL_ARB,
									TRUE,
									WGL_PIXEL_TYPE_ARB,
									WGL_TYPE_RGBA_ARB,
									WGL_COLOR_BITS_ARB,
									24,
									WGL_ALPHA_BITS_ARB,
									8,
									0};
		int format = 0;
		UINT count = 0;
		if (!Procedure<PFNWGLCHOOSEPIXELFORMATARBPROC>(gl, "wglChoosePixelFormatARB")(
				applicationDevice, pixelAttributes, nullptr, 1, &format, &count) ||
			!count)
			throw std::runtime_error("Cannot select OpenGL offscreen pixel format");
		const int pbufferAttributes[]{0};
		m_pbuffer = Procedure<PFNWGLCREATEPBUFFERARBPROC>(gl, "wglCreatePbufferARB")(applicationDevice, format, 1, 1,
																					 pbufferAttributes);
		if (!m_pbuffer)
			throw std::runtime_error("Cannot create OpenGL offscreen drawable");

		m_device = Procedure<PFNWGLGETPBUFFERDCARBPROC>(gl, "wglGetPbufferDCARB")(m_pbuffer);
		if (!m_device)
			throw std::runtime_error("Cannot acquire OpenGL offscreen device");
		std::vector<int> attributes{WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
									WGL_CONTEXT_MINOR_VERSION_ARB, 5,
									WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB};
		const auto extensions =
			Procedure<PFNWGLGETEXTENSIONSSTRINGARBPROC>(gl, "wglGetExtensionsStringARB")(applicationDevice);
		if (extensions && HasExtension(extensions, "WGL_ARB_context_flush_control"))
		{
			// Submission flushes each GPU handoff explicitly; polling and preparation need no implicit flush.
			attributes.push_back(WGL_CONTEXT_RELEASE_BEHAVIOR_ARB);
			attributes.push_back(WGL_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB);
		}
		attributes.push_back(0);
		m_handle = create(m_device, applicationContext, attributes.data());
		if (!m_handle)
			throw std::runtime_error("Cannot create shared OpenGL 4.5 context");
		windows::OpenGLContextScope current(gl, m_device, m_handle);
		if (!current.Current())
			throw std::runtime_error("Cannot activate compositor OpenGL context");
		const auto getString = Procedure<PFNGLGETSTRINGIPROC>(gl, "glGetStringi");
		GLint extensionCount = 0;
		gl.GetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
		for (const auto required : {"GL_EXT_memory_object_win32", "GL_EXT_semaphore_win32"})
		{
			bool found = false;
			for (GLint index = 0; index < extensionCount; ++index)
				found |= std::strcmp(reinterpret_cast<const char*>(getString(GL_EXTENSIONS, index)), required) == 0;
			if (!found)
				throw std::runtime_error(std::string("Required OpenGL sharing extension unavailable: ") + required);
		}
		// Validate cleanup entry points before any frame can partially allocate.
		m_deleteSync = Procedure<PFNGLDELETESYNCPROC>(gl, "glDeleteSync");
		m_deleteFramebuffers = Procedure<PFNGLDELETEFRAMEBUFFERSPROC>(gl, "glDeleteFramebuffers");
		m_deleteMemory = Procedure<PFNGLDELETEMEMORYOBJECTSEXTPROC>(gl, "glDeleteMemoryObjectsEXT");
		m_deleteSemaphores = Procedure<PFNGLDELETESEMAPHORESEXTPROC>(gl, "glDeleteSemaphoresEXT");
		m_wait = Procedure<PFNGLCLIENTWAITSYNCPROC>(gl, "glClientWaitSync");
		m_deleteProgram = Procedure<PFNGLDELETEPROGRAMPROC>(gl, "glDeleteProgram");
		m_deleteVertexArrays = Procedure<PFNGLDELETEVERTEXARRAYSPROC>(gl, "glDeleteVertexArrays");
		m_program = CreateProgram(shaders::CompositorVertexGlsl, shaders::CompositorPixelGlsl);
		m_depthMsProgram = CreateProgram(shaders::CompositorVertexGlsl, shaders::CompositorDepthMsGlsl);
		Procedure<PFNGLCREATEVERTEXARRAYSPROC>(gl, "glCreateVertexArrays")(1, &m_vertexArray);
		CheckGL(gl, "Initialize canonical OpenGL compositor");
		gl.Flush();
		if (!current.Restore() || !application.Restore())
			throw std::runtime_error("Cannot restore OpenGL context after compositor initialization");
	}

	GLuint CreateProgram(const char* vertex, const char* pixel)
	{
		const auto& gl = m_gl;
		const auto createShader = Procedure<PFNGLCREATESHADERPROC>(gl, "glCreateShader");
		const auto shaderSource = Procedure<PFNGLSHADERSOURCEPROC>(gl, "glShaderSource");
		const auto compileShader = Procedure<PFNGLCOMPILESHADERPROC>(gl, "glCompileShader");
		const auto getShader = Procedure<PFNGLGETSHADERIVPROC>(gl, "glGetShaderiv");
		const auto getShaderLog = Procedure<PFNGLGETSHADERINFOLOGPROC>(gl, "glGetShaderInfoLog");
		const auto deleteShader = Procedure<PFNGLDELETESHADERPROC>(gl, "glDeleteShader");
		const auto createProgram = Procedure<PFNGLCREATEPROGRAMPROC>(gl, "glCreateProgram");
		const auto attachShader = Procedure<PFNGLATTACHSHADERPROC>(gl, "glAttachShader");
		const auto linkProgram = Procedure<PFNGLLINKPROGRAMPROC>(gl, "glLinkProgram");
		const auto getProgram = Procedure<PFNGLGETPROGRAMIVPROC>(gl, "glGetProgramiv");
		const auto compile = [&](GLenum stage, const char* source) {
			const auto shader = createShader(stage);
			shaderSource(shader, 1, &source, nullptr);
			compileShader(shader);
			GLint compiled = 0;
			getShader(shader, GL_COMPILE_STATUS, &compiled);
			if (!compiled)
			{
				char message[2048]{};
				getShaderLog(shader, sizeof(message), nullptr, message);
				deleteShader(shader);
				throw std::runtime_error(message);
			}
			return shader;
		};
		const auto vs = compile(GL_VERTEX_SHADER, vertex);
		GLuint ps = 0, linked = 0;
		try
		{
			ps = compile(GL_FRAGMENT_SHADER, pixel);
			linked = createProgram();
			attachShader(linked, vs);
			attachShader(linked, ps);
			linkProgram(linked);
			GLint success = 0;
			getProgram(linked, GL_LINK_STATUS, &success);
			if (!success)
				throw std::runtime_error("Cannot link canonical OpenGL compositor");
		}
		catch (...)
		{
			if (linked)
				m_deleteProgram(linked);
			if (ps)
				deleteShader(ps);
			deleteShader(vs);
			throw;
		}
		deleteShader(vs);
		deleteShader(ps);
		return linked;
	}

	bool Close() noexcept
	{
		const auto& gl = m_gl;
		if (m_program || m_depthMsProgram || m_vertexArray)
		{
			windows::OpenGLContextScope current(gl, m_device, m_handle);
			if (!current.Current())
				return false;
			if (m_program)
				m_deleteProgram(std::exchange(m_program, 0));
			if (m_depthMsProgram)
				m_deleteProgram(std::exchange(m_depthMsProgram, 0));
			if (m_vertexArray)
			{
				m_deleteVertexArrays(1, &m_vertexArray);
				m_vertexArray = 0;
			}
			gl.Flush();
			if (!current.Restore())
				return false;
		}
		if (m_handle)
		{
			if (gl.GetCurrentContext() == m_handle && !gl.MakeCurrent(nullptr, nullptr))
				return false;
			if (!gl.DeleteContext(m_handle))
				return false;
			m_handle = nullptr;
		}
		if (m_device)
		{
			if (!m_releaseDevice(m_pbuffer, m_device))
				return false;
			m_device = nullptr;
		}
		if (m_pbuffer)
		{
			if (!m_destroy(m_pbuffer))
				return false;
			m_pbuffer = nullptr;
		}
		return true;
	}

	PFNGLDELETEPROGRAMPROC m_deleteProgram = nullptr;
	PFNGLDELETEVERTEXARRAYSPROC m_deleteVertexArrays = nullptr;

	HPBUFFERARB m_pbuffer = nullptr;
	PFNWGLRELEASEPBUFFERDCARBPROC m_releaseDevice = nullptr;
	PFNWGLDESTROYPBUFFERARBPROC m_destroy = nullptr;
};

class OpenGLSharedImage final
{
public:
	std::shared_ptr<OpenGLContext> context;
	std::shared_ptr<VulkanExternalImage> image;
	GLuint texture = 0, memory = 0, framebuffer = 0;
	GLenum format = 0;
	bool ready = false;

	static std::shared_ptr<OpenGLSharedImage> Create(std::shared_ptr<OpenGLContext> context)
	{
		auto result = std::shared_ptr<OpenGLSharedImage>(new OpenGLSharedImage, [](OpenGLSharedImage* image) {
			if (image->Close())
				delete image;
			else
				std::fputs("SimXR: preserving shared OpenGL image after native cleanup failure\n", stderr);
		});
		result->context = std::move(context);
		return result;
	}

private:
	bool Close() noexcept
	{
		if (!texture && !memory && !framebuffer)
			return true;
		std::lock_guard lock(context->m_mutex);
		const auto& gl = context->m_gl;
		windows::OpenGLContextScope current(gl, context->m_device, context->m_handle);
		if (!current.Current())
			return false;
		context->m_deleteFramebuffers(1, &framebuffer);
		gl.DeleteTextures(1, &texture);
		if (memory)
			context->m_deleteMemory(1, &memory);
		framebuffer = texture = memory = 0;
		return current.Restore();
	}
};

struct OpenGLFrame::Resources
{
	windows::ModuleReference m_module;
	std::shared_ptr<OpenGLContext> context;
	std::shared_ptr<OpenGLSharedImage> storage;
	std::shared_ptr<VulkanExternalSemaphore> available, ready;
	std::shared_ptr<FrameReservation> reservation;
	std::shared_ptr<FrameReservation> preservation;
	CompositionPlan plan;
	std::optional<LayerImageDescription> capture;
	std::vector<OpenGLCompositionSource> sources;
	std::vector<GLuint> copies;
	// Container attachments keep every source texture alive through native retirement.
	std::vector<GLuint> sourceFramebuffers;
	GLuint copyFramebuffer = 0;
	GLuint availableGL = 0, readyGL = 0;
	GLsync completion = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer initialize = VK_NULL_HANDLE;
	VkFence initialized = VK_NULL_HANDLE;
	bool submitted = false;
	bool sourcesUsed = false;
	bool retainSource = false;
};

OpenGLFrame::OpenGLFrame(std::shared_ptr<OpenGLContext> context, std::span<const FrameView> views)
	: FrameOutput(views), m_resources(std::make_unique<Resources>())
{
	m_resources->context = std::move(context);
}

OpenGLFrame::~OpenGLFrame()
{
	auto& resources = *m_resources;
	const auto& gl = resources.context->m_gl;
	std::lock_guard lock(resources.context->m_mutex);
	windows::OpenGLContextScope current(gl, resources.context->m_device, resources.context->m_handle);
	if (!current.Current())
	{
		m_resources.release(); // Retain context and GPU owners if native cleanup cannot be acquired.
		return;
	}
	if (resources.completion)
		resources.context->m_deleteSync(resources.completion);
	const auto deleteFramebuffers = resources.context->m_deleteFramebuffers;
	deleteFramebuffers(1, &resources.copyFramebuffer);
	deleteFramebuffers(static_cast<GLsizei>(resources.sourceFramebuffers.size()), resources.sourceFramebuffers.data());
	gl.DeleteTextures(static_cast<GLsizei>(resources.copies.size()), resources.copies.data());
	for (const auto semaphore : {resources.availableGL, resources.readyGL})
		if (semaphore)
			resources.context->m_deleteSemaphores(1, &semaphore);
	if (resources.storage && resources.storage->image)
	{
		const auto device = resources.storage->image->Image()->device->binding.device;
		if (resources.initialized)
			vkDestroyFence(device, resources.initialized, nullptr);
		if (resources.pool)
			vkDestroyCommandPool(device, resources.pool, nullptr);
	}
}

CompletionStatus OpenGLFrame::PollCompletion() const noexcept
{
	auto& resources = *m_resources;
	const auto& gl = resources.context->m_gl;
	std::lock_guard lock(resources.context->m_mutex);
	if (!resources.submitted || !resources.completion ||
		vkGetFenceStatus(resources.storage->image->Image()->device->binding.device, resources.initialized) !=
			VK_SUCCESS)
		return CompletionStatus::Pending;
	windows::OpenGLContextScope current(gl, resources.context->m_device, resources.context->m_handle);
	if (!current.Current())
		return CompletionStatus::Pending;
	const auto wait = resources.context->m_wait;
	const auto result = wait(resources.completion, 0, 0);
	if (!current.Restore())
		return CompletionStatus::Pending;
	return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED ? CompletionStatus::Complete
																			 : CompletionStatus::Pending;
}

bool OpenGLFrame::SourcesUsed() const noexcept
{
	std::lock_guard lock(m_resources->context->m_mutex);
	return m_resources->sourcesUsed;
}

std::shared_ptr<VulkanImage> OpenGLFrame::Image() const noexcept
{
	return std::shared_ptr<VulkanImage>(m_resources->storage, m_resources->storage->image->Image().get());
}

VkSemaphore OpenGLFrame::Ready() const noexcept
{
	return m_resources->ready->Get();
}

VulkanExternalIdentity OpenGLBackend::Identity(HDC device, HGLRC context)
{
	const windows::OpenGLLibrary gl;
	windows::OpenGLContextScope current(gl, device, context);
	if (!current.Current())
		throw std::runtime_error("Cannot identify application OpenGL context");
	RequireApplicationCapabilities(gl, device);
	const auto get = Procedure<PFNGLGETUNSIGNEDBYTEVEXTPROC>(gl, "glGetUnsignedBytevEXT");
	const auto getIndexed = Procedure<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>(gl, "glGetUnsignedBytei_vEXT");
	VulkanExternalIdentity identity;
	get(GL_DRIVER_UUID_EXT, identity.driverUuid.data());
	getIndexed(GL_DEVICE_UUID_EXT, 0, identity.deviceUuid.data());
	CheckGL(gl, "Cannot query application OpenGL device identity");
	if (!current.Restore())
		throw std::runtime_error("Cannot restore OpenGL context after device identification");
	return identity;
}

struct OpenGLBackend::PendingFrames
{
	windows::ModuleReference m_module;
	std::shared_ptr<FrameReservation> preservation;
	std::vector<std::shared_ptr<OpenGLFrame>> frames;

	explicit PendingFrames(std::size_t capacity)
	{
		static const FramePool s_owners{64};
		preservation = s_owners.Reserve();
		if (!preservation)
			throw std::bad_alloc();
		frames.reserve(capacity);
	}
};

OpenGLBackend::OpenGLBackend(HDC device, HGLRC context, std::shared_ptr<VulkanDevice> delivery,
							 std::span<const char* const> extensions)
	: m_applicationDevice(device), m_applicationContext(context), m_context(OpenGLContext::Create(device, context)),
	  m_external(std::move(delivery), extensions, VK_API_VERSION_1_1),
	  m_pending(std::make_unique<PendingFrames>(m_frames.Capacity() + m_retainedFrames.Capacity()))
{
	if (Identity(device, context) != m_external.Identity())
		throw std::runtime_error("OpenGL and Vulkan devices differ");
}

OpenGLBackend::~OpenGLBackend()
{
	const auto& gl = m_context->m_gl;
	if (m_pending->frames.empty())
		return;
	{
		std::lock_guard lock(m_context->m_mutex);
		windows::OpenGLContextScope current(gl, m_context->m_device, m_context->m_handle);
		const auto completion = m_pending->frames.back()->m_resources->completion;
		if (current.Current() && completion)
			m_context->m_wait(completion, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
	}
	if (std::any_of(m_pending->frames.begin(), m_pending->frames.end(),
					[](const auto& frame) { return frame->PollCompletion() == CompletionStatus::Pending; }))
		m_pending.release(); // Preserve the process-wide reservation with unfinished native work.
}

XrResult OpenGLBackend::Prepare(const CompositionPlan& plan, std::span<const OpenGLCompositionSource> sources,
								std::shared_ptr<OpenGLFrame>& output, std::uint32_t outputLayers)
{
	if (!outputLayers || outputLayers > 2 || !plan.width || !plan.height || sources.size() != plan.draws.size())
		return XR_ERROR_LAYER_INVALID;
	for (std::size_t index = 0; index < sources.size(); ++index)
	{
		const auto& source = sources[index];
		if (!source.texture || !source.width || !source.height || !source.mipLevels ||
			plan.draws[index].eye >= outputLayers || plan.draws[index].source.arraySlice >= source.layers)
			return XR_ERROR_LAYER_INVALID;
		if (!std::has_single_bit(source.samples) || (source.samples > 1 && source.mipLevels != 1))
			return XR_ERROR_VALIDATION_FAILURE;
		if ((plan.draws[index].constants.control[0] == 2) != IsDepthFormat(source.format))
			return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	VulkanExternalImageSpec description;
	description.extent = {plan.width, plan.height, 1};
	description.arrayLayers = outputLayers;
	return PrepareImage(plan, sources, description, GL_SRGB8_ALPHA8, false, output);
}

XrResult OpenGLBackend::PrepareCapture(const LayerImageDescription& image, OpenGLCompositionSource source,
									   std::shared_ptr<OpenGLFrame>& output, bool retainMipChain)
{
	const auto& rectangle = image.rectangle;
	if (!source.texture || !source.mipLevels || image.arraySlice >= source.layers || rectangle.offset.x < 0 ||
		rectangle.offset.y < 0 || rectangle.extent.width <= 0 || rectangle.extent.height <= 0 ||
		static_cast<std::uint64_t>(rectangle.offset.x) + rectangle.extent.width > source.width ||
		static_cast<std::uint64_t>(rectangle.offset.y) + rectangle.extent.height > source.height)
		return XR_ERROR_LAYER_INVALID;
	if (!std::has_single_bit(source.samples) || (source.samples > 1 && source.mipLevels != 1))
		return XR_ERROR_VALIDATION_FAILURE;
	VulkanExternalImageSpec description;
	switch (source.format)
	{
	case GL_RGBA8:
		description.format = VK_FORMAT_R8G8B8A8_UNORM;
		break;
	case GL_SRGB8_ALPHA8:
		description.format = VK_FORMAT_R8G8B8A8_SRGB;
		break;
	case GL_RGBA16F:
		description.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		break;
	case GL_RGBA32F:
		description.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		break;
	case GL_RGB10_A2:
		description.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		break;
	default:
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	description.mipLevels = retainMipChain ? source.mipLevels : 1;
	description.arrayLayers = 1;
	description.extent = retainMipChain ? VkExtent3D{source.width, source.height, 1}
										: VkExtent3D{static_cast<std::uint32_t>(rectangle.extent.width),
													 static_cast<std::uint32_t>(rectangle.extent.height), 1};
	CompositionPlan plan{description.extent.width, description.extent.height, {}, {}};
	std::shared_ptr<OpenGLFrame> frame;
	const auto result = PrepareImage(plan, {}, description, source.format, retainMipChain, frame);
	if (XR_FAILED(result))
		return result;
	frame->m_resources->capture = image;
	frame->m_resources->sources.push_back(source);
	output = std::move(frame);
	return XR_SUCCESS;
}

XrResult OpenGLBackend::PrepareImage(const CompositionPlan& plan, std::span<const OpenGLCompositionSource> sources,
									 const VulkanExternalImageSpec& description, GLenum format, bool retained,
									 std::shared_ptr<OpenGLFrame>& output)
{
	std::erase_if(m_pending->frames,
				  [](const auto& frame) { return frame->PollCompletion() != CompletionStatus::Pending; });
	auto& pool = retained ? m_retainedFrames : m_frames;
	auto reservation = pool.Reserve();
	if (!reservation)
		return XR_ERROR_LIMIT_REACHED;
	auto frame = std::shared_ptr<OpenGLFrame>(new OpenGLFrame(m_context, plan.views));
	auto& resources = *frame->m_resources;
	const auto& gl = m_context->m_gl;
	resources.preservation = m_pending->preservation;
	resources.plan = plan;
	resources.retainSource = retained;
	resources.sources.assign(sources.begin(), sources.end());
	resources.copies.resize(sources.size());
	resources.sourceFramebuffers.resize(std::max<std::size_t>(sources.size(), 1));
	for (const auto& image : m_images)
		if (image && image.use_count() == 1 && image->ready && image->format == format &&
			image->image->Descriptor().image == description)
		{
			resources.storage = image;
			break;
		}
	if (!resources.storage)
	{
		resources.storage = OpenGLSharedImage::Create(m_context);
		resources.storage->format = format;
		resources.storage->image = m_external.CreateExportable(description);
	}

	resources.available = m_external.CreateExportableSemaphore();
	resources.ready = m_external.CreateExportableSemaphore();
	{
		std::lock_guard lock(m_context->m_mutex);
		windows::OpenGLContextScope current(gl, m_context->m_device, m_context->m_handle);
		if (!current.Current())
			return XR_ERROR_RUNTIME_FAILURE;
		if (!resources.storage->ready)
		{
			Procedure<PFNGLCREATEMEMORYOBJECTSEXTPROC>(gl, "glCreateMemoryObjectsEXT")(1, &resources.storage->memory);
			const GLint dedicated = GL_TRUE;
			Procedure<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(gl, "glMemoryObjectParameterivEXT")(
				resources.storage->memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
			auto memory = resources.storage->image->ExportMemory();
			Procedure<PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC>(gl, "glImportMemoryWin32HandleEXT")(
				resources.storage->memory, resources.storage->image->Descriptor().allocationSize,
				GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, memory.Get());
			Procedure<PFNGLCREATETEXTURESPROC>(gl, "glCreateTextures")(GL_TEXTURE_2D_ARRAY, 1,
																	   &resources.storage->texture);
			Procedure<PFNGLTEXTUREPARAMETERIPROC>(gl, "glTextureParameteri")(
				resources.storage->texture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
			Procedure<PFNGLTEXTURESTORAGEMEM3DEXTPROC>(gl, "glTextureStorageMem3DEXT")(
				resources.storage->texture, description.mipLevels, format, plan.width, plan.height,
				description.arrayLayers, resources.storage->memory, 0);
		}

		const auto import = [&](const std::shared_ptr<VulkanExternalSemaphore>& source, GLuint& target) {
			auto handle = source->ExportOnce();
			Procedure<PFNGLGENSEMAPHORESEXTPROC>(gl, "glGenSemaphoresEXT")(1, &target);
			Procedure<PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC>(gl, "glImportSemaphoreWin32HandleEXT")(
				target, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle.Get());
		};
		import(resources.available, resources.availableGL);
		import(resources.ready, resources.readyGL);
		if (!resources.storage->ready)
			Procedure<PFNGLCREATEFRAMEBUFFERSPROC>(gl, "glCreateFramebuffers")(1, &resources.storage->framebuffer);
		Procedure<PFNGLCREATEFRAMEBUFFERSPROC>(gl, "glCreateFramebuffers")(
			static_cast<GLsizei>(resources.sourceFramebuffers.size()), resources.sourceFramebuffers.data());
		Procedure<PFNGLCREATEFRAMEBUFFERSPROC>(gl, "glCreateFramebuffers")(1, &resources.copyFramebuffer);
		for (std::size_t index = 0; index < sources.size(); ++index)
		{
			const auto& source = sources[index];
			auto& copy = resources.copies[index];
			const bool depthMs = source.samples > 1 && IsDepthFormat(source.format);
			Procedure<PFNGLCREATETEXTURESPROC>(gl, "glCreateTextures")(
				depthMs ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 1, &copy);
			if (depthMs)
			{
				Procedure<PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC>(gl, "glTextureStorage2DMultisample")(
					copy, source.samples, source.format, source.width, source.height, GL_TRUE);
				continue;
			}
			Procedure<PFNGLTEXTURESTORAGE2DPROC>(gl, "glTextureStorage2D")(copy, source.mipLevels, source.format,
																		   source.width, source.height);
			const auto parameter = Procedure<PFNGLTEXTUREPARAMETERIPROC>(gl, "glTextureParameteri");
			parameter(copy, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			parameter(copy, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			parameter(copy, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			parameter(copy, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		CheckGL(gl, "Prepare shared OpenGL compositor images");
		resources.storage->ready = true;
		if (!current.Restore())
			return XR_ERROR_RUNTIME_FAILURE;
	}
	const auto device = m_external.Device()->binding.device;
	VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	commands.queueFamilyIndex = m_external.Device()->binding.queueFamily;
	CheckVulkan(vkCreateCommandPool(device, &commands, nullptr, &resources.pool), "Create GL handoff command pool");
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = resources.pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	CheckVulkan(vkAllocateCommandBuffers(device, &allocate, &resources.initialize), "Allocate GL handoff command");
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(device, &fence, nullptr, &resources.initialized), "Create GL handoff fence");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CheckVulkan(vkBeginCommandBuffer(resources.initialize, &begin), "Begin GL handoff");
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = commands.queueFamilyIndex;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	barrier.image = resources.storage->image->Image()->image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, description.mipLevels, 0, description.arrayLayers};
	vkCmdPipelineBarrier(resources.initialize, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &barrier);
	CheckVulkan(vkEndCommandBuffer(resources.initialize), "End GL handoff");
	if (!pool.Consume(reservation))
		return XR_ERROR_CALL_ORDER_INVALID;
	resources.reservation = std::move(reservation);
	VkDeviceSize cachedBytes = 0;
	bool cached = false;
	for (const auto& image : m_images)
		if (image)
		{
			cachedBytes += image->image->Descriptor().allocationSize;
			cached |= image == resources.storage;
		}
	const auto bytes = resources.storage->image->Descriptor().allocationSize;
	constexpr VkDeviceSize limit = 64ULL * 1024 * 1024;
	if (!cached && bytes <= limit && cachedBytes <= limit - bytes)
		for (auto& image : m_images)
			if (!image)
			{
				image = resources.storage;
				break;
			}
	output = std::move(frame);
	return XR_SUCCESS;
}

XrResult OpenGLBackend::Submit(const std::shared_ptr<OpenGLFrame>& frame)
{
	if (!frame || frame->m_resources->context != m_context || frame->m_resources->submitted)
		return XR_ERROR_CALL_ORDER_INVALID;
	auto& resources = *frame->m_resources;
	const auto& gl = m_context->m_gl;
	std::lock_guard lock(m_context->m_mutex);
	windows::OpenGLContextScope application(gl, m_applicationDevice, m_applicationContext);
	if (!application.Current())
		return XR_ERROR_RUNTIME_FAILURE;
	const auto fence = Procedure<PFNGLFENCESYNCPROC>(gl, "glFenceSync");
	const auto wait = Procedure<PFNGLWAITSYNCPROC>(gl, "glWaitSync");
	const auto remove = Procedure<PFNGLDELETESYNCPROC>(gl, "glDeleteSync");
	const auto rendered = fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!rendered)
		return XR_ERROR_RUNTIME_FAILURE;
	gl.Flush();
	m_pending->frames.push_back(frame);
	resources.submitted = true;
	windows::OpenGLContextScope current(gl, m_context->m_device, m_context->m_handle);
	if (!current.Current())
	{
		remove(rendered);
		return XR_ERROR_RUNTIME_FAILURE;
	}
	VkSubmitInfo initialize{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	initialize.commandBufferCount = 1;
	initialize.pCommandBuffers = &resources.initialize;
	const auto available = resources.available->Get();
	initialize.signalSemaphoreCount = 1;
	initialize.pSignalSemaphores = &available;
	const auto submitted = vkQueueSubmit(m_external.Device()->queue, 1, &initialize, resources.initialized);
	if (submitted != VK_SUCCESS)
	{
		remove(rendered);
		return VulkanGraphicsFailure(submitted);
	}
	try
	{
		wait(rendered, 0, GL_TIMEOUT_IGNORED);
		remove(rendered);
		const GLenum layout = GL_LAYOUT_GENERAL_EXT;
		Procedure<PFNGLWAITSEMAPHOREEXTPROC>(gl, "glWaitSemaphoreEXT")(resources.availableGL, 0, nullptr, 1,
																	   &resources.storage->texture, &layout);
		gl.Enable(GL_FRAMEBUFFER_SRGB);
		if (resources.capture)
		{
			const auto& source = resources.sources.front();
			const auto sourceFramebuffer = resources.sourceFramebuffers.front();
			const auto& image = *resources.capture;
			const auto& description = resources.storage->image->Descriptor().image;
			for (std::uint32_t mip = 0; mip < description.mipLevels; ++mip)
			{
				if (source.layers == 1)
					Procedure<PFNGLNAMEDFRAMEBUFFERTEXTUREPROC>(gl, "glNamedFramebufferTexture")(
						sourceFramebuffer, GL_COLOR_ATTACHMENT0, source.texture, mip);
				else
					Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>(gl, "glNamedFramebufferTextureLayer")(
						sourceFramebuffer, GL_COLOR_ATTACHMENT0, source.texture, mip, image.arraySlice);
				Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>(gl, "glNamedFramebufferTextureLayer")(
					resources.storage->framebuffer, GL_COLOR_ATTACHMENT0, resources.storage->texture, mip, 0);
				for (const auto framebuffer : {sourceFramebuffer, resources.storage->framebuffer})
					if (Procedure<PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC>(gl, "glCheckNamedFramebufferStatus")(
							framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
						throw std::runtime_error("OpenGL capture framebuffer incomplete");
				const auto width = std::max(1u, description.extent.width >> mip);
				const auto height = std::max(1u, description.extent.height >> mip);
				const auto sourceHeight = std::max(1u, source.height >> mip);
				const auto x = resources.retainSource ? 0 : image.rectangle.offset.x;
				const auto y = resources.retainSource ? 0 : image.rectangle.offset.y;
				const auto blit = Procedure<PFNGLBLITNAMEDFRAMEBUFFERPROC>(gl, "glBlitNamedFramebuffer");
				resources.sourcesUsed = true;
				blit(sourceFramebuffer, resources.storage->framebuffer, x, sourceHeight - y, x + width,
					 sourceHeight - y - height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
		}
		else
		{
			for (std::size_t index = 0; index < resources.sources.size(); ++index)
			{
				const auto& source = resources.sources[index];
				const auto sourceFramebuffer = resources.sourceFramebuffers[index];
				const auto slice = resources.plan.draws[index].source.arraySlice;
				const bool depth = IsDepthFormat(source.format);
				const GLenum attachment = depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
				const auto attach = Procedure<PFNGLNAMEDFRAMEBUFFERTEXTUREPROC>(gl, "glNamedFramebufferTexture");
				for (const auto framebuffer : {sourceFramebuffer, resources.copyFramebuffer})
				{
					attach(framebuffer, GL_DEPTH_ATTACHMENT, 0, 0);
					attach(framebuffer, GL_COLOR_ATTACHMENT0, 0, 0);
					Procedure<PFNGLNAMEDFRAMEBUFFERREADBUFFERPROC>(gl, "glNamedFramebufferReadBuffer")(
						framebuffer, depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
					Procedure<PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC>(gl, "glNamedFramebufferDrawBuffer")(
						framebuffer, depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
				}
				for (std::uint32_t mip = 0; mip < source.mipLevels; ++mip)
				{
					if (source.layers == 1)
						attach(sourceFramebuffer, attachment, source.texture, mip);
					else
						Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>(gl, "glNamedFramebufferTextureLayer")(
							sourceFramebuffer, attachment, source.texture, mip, slice);
					attach(resources.copyFramebuffer, attachment, resources.copies[index], mip);
					for (const auto framebuffer : {sourceFramebuffer, resources.copyFramebuffer})
						if (Procedure<PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC>(gl, "glCheckNamedFramebufferStatus")(
								framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
							throw std::runtime_error("OpenGL source copy framebuffer incomplete");
					const auto width = std::max(1u, source.width >> mip), height = std::max(1u, source.height >> mip);
					const auto blit = Procedure<PFNGLBLITNAMEDFRAMEBUFFERPROC>(gl, "glBlitNamedFramebuffer");
					resources.sourcesUsed = true;
					blit(sourceFramebuffer, resources.copyFramebuffer, 0, height, width, 0, 0, 0, width, height,
						 depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
				}
			}
			Procedure<PFNGLCLIPCONTROLPROC>(gl, "glClipControl")(GL_UPPER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
			Procedure<PFNGLBINDVERTEXARRAYPROC>(gl, "glBindVertexArray")(m_context->m_vertexArray);
			Procedure<PFNGLBINDFRAMEBUFFERPROC>(gl, "glBindFramebuffer")(GL_FRAMEBUFFER,
																		 resources.storage->framebuffer);
			gl.Viewport(0, 0, resources.plan.width, resources.plan.height);
			gl.Enable(GL_FRAMEBUFFER_SRGB);
			gl.Enable(GL_BLEND);
			gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			gl.ClearColor(0, 0, 0, 1);
			for (GLuint eye = 0; eye < resources.storage->image->Descriptor().image.arrayLayers; ++eye)
			{
				Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>(gl, "glNamedFramebufferTextureLayer")(
					resources.storage->framebuffer, GL_COLOR_ATTACHMENT0, resources.storage->texture, 0, eye);
				if (Procedure<PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC>(gl, "glCheckNamedFramebufferStatus")(
						resources.storage->framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
					throw std::runtime_error("Shared OpenGL target incomplete");
				gl.Clear(GL_COLOR_BUFFER_BIT);
				for (std::size_t index = 0; index < resources.plan.draws.size(); ++index)
				{
					const auto& draw = resources.plan.draws[index];
					if (draw.eye != eye)
						continue;
					const auto& source = resources.sources[index];
					const auto program = source.samples > 1 && IsDepthFormat(source.format)
											 ? m_context->m_depthMsProgram
											 : m_context->m_program;
					Procedure<PFNGLUSEPROGRAMPROC>(gl, "glUseProgram")(program);
					Procedure<PFNGLBINDTEXTUREUNITPROC>(gl, "glBindTextureUnit")(0, resources.copies[index]);
					const auto location = [&](const char* name) {
						return Procedure<PFNGLGETUNIFORMLOCATIONPROC>(gl, "glGetUniformLocation")(program, name);
					};
					const auto set = [&](const char* name, const auto& value) {
						Procedure<PFNGLUNIFORM4FVPROC>(gl, "glUniform4fv")(location(name), 1, value.data());
					};
					set("layer.eyeTangents", draw.constants.eyeTangents);
					set("layer.sourceTangents", draw.constants.sourceTangents);
					set("layer.sourceFromEyeRotation", draw.constants.sourceFromEyeRotation);
					set("layer.sourceFromEyePosition", draw.constants.sourceFromEyePosition);
					set("layer.imageRectangle", draw.constants.imageRectangle);
					set("layer.quadParameters", draw.constants.quadParameters);
					Procedure<PFNGLUNIFORM4UIVPROC>(gl, "glUniform4uiv")(location("layer.control"), 1,
																		 draw.constants.control.data());
					gl.DrawArrays(GL_TRIANGLES, 0, 3);
				}
			}
		}
		Procedure<PFNGLSIGNALSEMAPHOREEXTPROC>(gl, "glSignalSemaphoreEXT")(resources.readyGL, 0, nullptr, 1,
																		   &resources.storage->texture, &layout);
		resources.completion = fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		gl.Flush();
		CheckGL(gl, "Submit canonical OpenGL composition");
		if (!resources.completion || !current.Restore())
			throw std::runtime_error("Cannot restore application OpenGL context");
		wait(resources.completion, 0, GL_TIMEOUT_IGNORED);
		gl.Flush();
		if (!application.Restore())
			throw std::runtime_error("Cannot restore OpenGL context after compositor submission");
		return XR_SUCCESS;
	}
	catch (...)
	{
		// Exceptional failure must complete source reads before allowing app reuse.
		windows::OpenGLContextScope cleanup(gl, m_context->m_device, m_context->m_handle);
		if (cleanup.Current())
		{
			gl.Finish();
			if (!resources.completion)
				resources.completion = fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			gl.Flush();
		}
		throw;
	}
}
} // namespace simxr
