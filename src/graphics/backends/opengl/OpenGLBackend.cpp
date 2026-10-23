#include "Pch.h"
#include "graphics/backends/opengl/OpenGLBackend.h"
#include "platform/windows/OpenGLContextScope.h"
#include "CompositorGlsl.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include <GL/glext.h>
#include <GL/wglext.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace simxr
{
namespace
{
template<typename Function> Function Procedure(const char* name)
{
	const auto address = wglGetProcAddress(name);
	const auto value = std::bit_cast<std::uintptr_t>(address);
	if (value <= 3 || value == UINTPTR_MAX)
		throw std::runtime_error(std::string("Required OpenGL command unavailable: ") + name);
	return std::bit_cast<Function>(address);
}
bool IsDepthFormat(GLenum format)
{
	return format == GL_DEPTH_COMPONENT16 || format == GL_DEPTH_COMPONENT24 ||
		format == GL_DEPTH_COMPONENT32F || format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8;
}
void CheckGL(const char* operation)
{
	if (glGetError() != GL_NO_ERROR)
		throw std::runtime_error(operation);
}
}

// A private share-group context keeps native cleanup independent of application
// context lifetime and isolates every framebuffer, program and binding mutation.
class OpenGLContext final
{
  public:
	OpenGLContext(HDC applicationDevice, HGLRC applicationContext)
	{
		windows::OpenGLContextScope application(applicationDevice, applicationContext);
		if (!application.Current())
			throw std::runtime_error("Cannot acquire application OpenGL context");
		const auto create = Procedure<PFNWGLCREATECONTEXTATTRIBSARBPROC>("wglCreateContextAttribsARB");
		m_releaseDevice = Procedure<PFNWGLRELEASEPBUFFERDCARBPROC>("wglReleasePbufferDCARB");
		m_destroy = Procedure<PFNWGLDESTROYPBUFFERARBPROC>("wglDestroyPbufferARB");
		const int pixelAttributes[]{WGL_DRAW_TO_PBUFFER_ARB, TRUE, WGL_SUPPORT_OPENGL_ARB, TRUE,
			WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, 24, WGL_ALPHA_BITS_ARB, 8, 0};
		int format = 0;
		UINT count = 0;
		if (!Procedure<PFNWGLCHOOSEPIXELFORMATARBPROC>("wglChoosePixelFormatARB")(
			applicationDevice, pixelAttributes, nullptr, 1, &format, &count) || !count)
			throw std::runtime_error("Cannot select OpenGL offscreen pixel format");
		const int pbufferAttributes[]{0};
		m_pbuffer = Procedure<PFNWGLCREATEPBUFFERARBPROC>("wglCreatePbufferARB")(
			applicationDevice, format, 1, 1, pbufferAttributes);
		if (!m_pbuffer)
			throw std::runtime_error("Cannot create OpenGL offscreen drawable");
		try
		{
			device = Procedure<PFNWGLGETPBUFFERDCARBPROC>("wglGetPbufferDCARB")(m_pbuffer);
			if (!device)
				throw std::runtime_error("Cannot acquire OpenGL offscreen device");
			const int attributes[]{WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 5,
				WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
			handle = create(device, applicationContext, attributes);
			if (!handle)
				throw std::runtime_error("Cannot create shared OpenGL 4.5 context");
			windows::OpenGLContextScope current(device, handle);
			if (!current.Current())
				throw std::runtime_error("Cannot activate compositor OpenGL context");
			const auto getString = Procedure<PFNGLGETSTRINGIPROC>("glGetStringi");
			GLint count = 0;
			glGetIntegerv(GL_NUM_EXTENSIONS, &count);
			for (const auto required : {"GL_EXT_memory_object_win32", "GL_EXT_semaphore_win32"})
			{
				bool found = false;
				for (GLint index = 0; index < count; ++index)
					found |= std::strcmp(reinterpret_cast<const char*>(getString(GL_EXTENSIONS, index)), required) == 0;
				if (!found)
					throw std::runtime_error(std::string("Required OpenGL sharing extension unavailable: ") + required);
			}
			// Validate cleanup entry points before any frame can partially allocate.
			Procedure<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT");
			Procedure<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT");
			program = CreateProgram(shaders::CompositorVertexGlsl, shaders::CompositorPixelGlsl);
			Procedure<PFNGLCREATEVERTEXARRAYSPROC>("glCreateVertexArrays")(1, &vertexArray);
			CheckGL("Initialize canonical OpenGL compositor");
		}
		catch (...)
		{
			Close();
			throw;
		}
	}
	~OpenGLContext()
	{
		Close();
	}
	std::mutex mutex;
	HDC device = nullptr;
	HGLRC handle = nullptr;
	GLuint program = 0, vertexArray = 0;

  private:
	GLuint CreateProgram(const char* vertex, const char* pixel)
	{
		const auto compile = [](GLenum stage, const char* source)
		{
			const auto shader = Procedure<PFNGLCREATESHADERPROC>("glCreateShader")(stage);
			Procedure<PFNGLSHADERSOURCEPROC>("glShaderSource")(shader, 1, &source, nullptr);
			Procedure<PFNGLCOMPILESHADERPROC>("glCompileShader")(shader);
			GLint compiled = 0;
			Procedure<PFNGLGETSHADERIVPROC>("glGetShaderiv")(shader, GL_COMPILE_STATUS, &compiled);
			if (!compiled)
			{
				char message[2048]{};
				Procedure<PFNGLGETSHADERINFOLOGPROC>("glGetShaderInfoLog")(shader, sizeof(message), nullptr, message);
				Procedure<PFNGLDELETESHADERPROC>("glDeleteShader")(shader);
				throw std::runtime_error(message);
			}
			return shader;
		};
		const auto vs = compile(GL_VERTEX_SHADER, vertex);
		GLuint ps = 0, linked = 0;
		try
		{
			ps = compile(GL_FRAGMENT_SHADER, pixel);
			linked = Procedure<PFNGLCREATEPROGRAMPROC>("glCreateProgram")();
			Procedure<PFNGLATTACHSHADERPROC>("glAttachShader")(linked, vs);
			Procedure<PFNGLATTACHSHADERPROC>("glAttachShader")(linked, ps);
			Procedure<PFNGLLINKPROGRAMPROC>("glLinkProgram")(linked);
			GLint success = 0;
			Procedure<PFNGLGETPROGRAMIVPROC>("glGetProgramiv")(linked, GL_LINK_STATUS, &success);
			if (!success)
				throw std::runtime_error("Cannot link canonical OpenGL compositor");
		}
		catch (...)
		{
			if (linked)
				Procedure<PFNGLDELETEPROGRAMPROC>("glDeleteProgram")(linked);
			if (ps)
				Procedure<PFNGLDELETESHADERPROC>("glDeleteShader")(ps);
			Procedure<PFNGLDELETESHADERPROC>("glDeleteShader")(vs);
			throw;
		}
		Procedure<PFNGLDELETESHADERPROC>("glDeleteShader")(vs);
		Procedure<PFNGLDELETESHADERPROC>("glDeleteShader")(ps);
		return linked;
	}
	void Close() noexcept
	{
		if (handle)
		{
			windows::OpenGLContextScope current(device, handle);
			if (current.Current())
			{
				if (program)
					std::bit_cast<PFNGLDELETEPROGRAMPROC>(wglGetProcAddress("glDeleteProgram"))(program);
				if (vertexArray)
					std::bit_cast<PFNGLDELETEVERTEXARRAYSPROC>(wglGetProcAddress("glDeleteVertexArrays"))(1, &vertexArray);
			}
		}
		if (handle)
			wglDeleteContext(handle);
		if (device)
			m_releaseDevice(m_pbuffer, device);
		if (m_pbuffer)
			m_destroy(m_pbuffer);
		handle = nullptr;
		device = nullptr;
		m_pbuffer = nullptr;
	}
	HPBUFFERARB m_pbuffer = nullptr;
	PFNWGLRELEASEPBUFFERDCARBPROC m_releaseDevice = nullptr;
	PFNWGLDESTROYPBUFFERARBPROC m_destroy = nullptr;
};

struct OpenGLFrame::Resources
{
	std::shared_ptr<OpenGLContext> context;
	std::shared_ptr<VulkanExternalImage> image;
	std::shared_ptr<VulkanExternalSemaphore> available, ready;
	std::shared_ptr<FrameReservation> reservation;
	std::shared_ptr<FrameReservation> preservation;
	CompositionPlan plan;
	std::optional<LayerImageDescription> capture;
	std::vector<OpenGLCompositionSource> sources;
	std::vector<GLuint> copies;
	GLuint texture = 0, memory = 0, framebuffer = 0, sourceFramebuffer = 0, copyFramebuffer = 0;
	GLuint availableGL = 0, readyGL = 0;
	GLsync completion = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer initialize = VK_NULL_HANDLE;
	VkFence initialized = VK_NULL_HANDLE;
	bool submitted = false;
};
OpenGLFrame::OpenGLFrame(std::shared_ptr<OpenGLContext> context, std::span<const FrameView> views)
	: FrameOutput(views), m_resources(std::make_unique<Resources>())
{
	m_resources->context = std::move(context);
}
OpenGLFrame::~OpenGLFrame()
{
	auto& resources = *m_resources;
	std::lock_guard lock(resources.context->mutex);
	windows::OpenGLContextScope current(resources.context->device, resources.context->handle);
	if (!current.Current())
	{
		m_resources.release(); // Retain context and GPU owners if native cleanup cannot be acquired.
		return;
	}
	if (resources.completion)
		Procedure<PFNGLDELETESYNCPROC>("glDeleteSync")(resources.completion);
	const GLuint framebuffers[]{resources.framebuffer, resources.sourceFramebuffer, resources.copyFramebuffer};
	Procedure<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers")(3, framebuffers);
	glDeleteTextures(1, &resources.texture);
	glDeleteTextures(static_cast<GLsizei>(resources.copies.size()), resources.copies.data());
	if (resources.memory)
		Procedure<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT")(1, &resources.memory);
	for (const auto semaphore : {resources.availableGL, resources.readyGL})
		if (semaphore)
			Procedure<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT")(1, &semaphore);
	if (resources.image)
	{
		const auto device = resources.image->Image()->device->binding.device;
		if (resources.initialized)
			vkDestroyFence(device, resources.initialized, nullptr);
		if (resources.pool)
			vkDestroyCommandPool(device, resources.pool, nullptr);
	}
}
CompletionStatus OpenGLFrame::PollCompletion() const noexcept
{
	auto& resources = *m_resources;
	if (!resources.submitted || !resources.completion ||
		vkGetFenceStatus(resources.image->Image()->device->binding.device, resources.initialized) != VK_SUCCESS)
		return CompletionStatus::Pending;
	std::lock_guard lock(resources.context->mutex);
	windows::OpenGLContextScope current(resources.context->device, resources.context->handle);
	if (!current.Current())
		return CompletionStatus::Pending;
	const auto wait = std::bit_cast<PFNGLCLIENTWAITSYNCPROC>(wglGetProcAddress("glClientWaitSync"));
	const auto result = wait(resources.completion, 0, 0);
	return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED ? CompletionStatus::Complete : CompletionStatus::Pending;
}
std::shared_ptr<VulkanImage> OpenGLFrame::Image() const noexcept
{
	return m_resources->image->Image();
}
VkSemaphore OpenGLFrame::Ready() const noexcept
{
	return m_resources->ready->Get();
}

VulkanExternalIdentity OpenGLBackend::Identity(HDC device, HGLRC context)
{
	windows::OpenGLContextScope current(device, context);
	if (!current.Current())
		throw std::runtime_error("Cannot identify application OpenGL context");
	const auto get = Procedure<PFNGLGETUNSIGNEDBYTEVEXTPROC>("glGetUnsignedBytevEXT");
	const auto getIndexed = Procedure<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>("glGetUnsignedBytei_vEXT");
	VulkanExternalIdentity identity;
	get(GL_DRIVER_UUID_EXT, identity.driverUuid.data());
	getIndexed(GL_DEVICE_UUID_EXT, 0, identity.deviceUuid.data());
	return identity;
}
struct OpenGLBackend::PendingFrames
{
	std::shared_ptr<FrameReservation> preservation;
	std::vector<std::shared_ptr<OpenGLFrame>> frames;
	explicit PendingFrames(std::size_t capacity)
	{
		static const FramePool owners{64};
		preservation = owners.Reserve();
		if (!preservation)
			throw std::bad_alloc();
		frames.reserve(capacity);
	}
};

OpenGLBackend::OpenGLBackend(HDC device, HGLRC context, std::shared_ptr<VulkanDevice> delivery,
	std::span<const char* const> extensions)
	: m_applicationDevice(device), m_applicationContext(context), m_context(std::make_shared<OpenGLContext>(device, context)),
	m_external(std::move(delivery), extensions, VK_API_VERSION_1_1),
	m_pending(std::make_unique<PendingFrames>(m_frames.Capacity() + m_retainedFrames.Capacity()))
{
	if (Identity(device, context) != m_external.Identity())
		throw std::runtime_error("OpenGL and Vulkan devices differ");
}
OpenGLBackend::~OpenGLBackend()
{
	if (m_pending->frames.empty())
		return;
	{
		std::lock_guard lock(m_context->mutex);
		windows::OpenGLContextScope current(m_context->device, m_context->handle);
		const auto completion = m_pending->frames.back()->m_resources->completion;
		if (current.Current() && completion)
			Procedure<PFNGLCLIENTWAITSYNCPROC>("glClientWaitSync")(
				completion, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
	}
	if (std::any_of(m_pending->frames.begin(), m_pending->frames.end(), [](const auto& frame)
		{ return frame->PollCompletion() == CompletionStatus::Pending; }))
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
		if (source.samples != 1)
			return XR_ERROR_FEATURE_UNSUPPORTED;
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
	if (!source.texture || !source.mipLevels || image.arraySlice >= source.layers ||
		rectangle.offset.x < 0 || rectangle.offset.y < 0 || rectangle.extent.width <= 0 || rectangle.extent.height <= 0 ||
		static_cast<std::uint64_t>(rectangle.offset.x) + rectangle.extent.width > source.width ||
		static_cast<std::uint64_t>(rectangle.offset.y) + rectangle.extent.height > source.height)
		return XR_ERROR_LAYER_INVALID;
	if (source.samples != 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	VulkanExternalImageSpec description;
	switch (source.format)
	{
	case GL_RGBA8: description.format = VK_FORMAT_R8G8B8A8_UNORM; break;
	case GL_SRGB8_ALPHA8: description.format = VK_FORMAT_R8G8B8A8_SRGB; break;
	case GL_RGBA16F:
		if (!retainMipChain) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		description.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		break;
	case GL_RGBA32F:
		if (!retainMipChain) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		description.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		break;
	case GL_RGB10_A2:
		if (!retainMipChain) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		description.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		break;
	default: return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	description.mipLevels = retainMipChain ? source.mipLevels : 1;
	description.arrayLayers = 1;
	description.extent = description.mipLevels > 1 ? VkExtent3D{source.width, source.height, 1} :
		VkExtent3D{static_cast<std::uint32_t>(rectangle.extent.width), static_cast<std::uint32_t>(rectangle.extent.height), 1};
	CompositionPlan plan{description.extent.width, description.extent.height, {}, {}};
	std::shared_ptr<OpenGLFrame> frame;
	const auto result = PrepareImage(plan, {}, description, source.format, retainMipChain, frame);
	if (XR_FAILED(result)) return result;
	frame->m_resources->capture = image;
	frame->m_resources->sources.push_back(source);
	output = std::move(frame);
	return XR_SUCCESS;
}

XrResult OpenGLBackend::PrepareImage(const CompositionPlan& plan, std::span<const OpenGLCompositionSource> sources,
	const VulkanExternalImageSpec& description, GLenum format, bool retained, std::shared_ptr<OpenGLFrame>& output)
{
	std::erase_if(m_pending->frames, [](const auto& frame) { return frame->PollCompletion() != CompletionStatus::Pending; });
	auto& pool = retained ? m_retainedFrames : m_frames;
	auto reservation = pool.Reserve();
	if (!reservation)
		return XR_ERROR_LIMIT_REACHED;
	auto frame = std::shared_ptr<OpenGLFrame>(new OpenGLFrame(m_context, plan.views));
	auto& resources = *frame->m_resources;
	resources.preservation = m_pending->preservation;
	resources.plan = plan;
	resources.sources.assign(sources.begin(), sources.end());
	resources.copies.resize(sources.size());
	resources.image = m_external.CreateExportable(description);
	resources.available = m_external.CreateExportableSemaphore();
	resources.ready = m_external.CreateExportableSemaphore();
	{
		std::lock_guard lock(m_context->mutex);
		windows::OpenGLContextScope current(m_context->device, m_context->handle);
		if (!current.Current())
			return XR_ERROR_RUNTIME_FAILURE;
		Procedure<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT")(1, &resources.memory);
		const GLint dedicated = GL_TRUE;
		Procedure<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>("glMemoryObjectParameterivEXT")(
			resources.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
		auto memory = resources.image->ExportMemory();
		Procedure<PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC>("glImportMemoryWin32HandleEXT")(
			resources.memory, resources.image->Descriptor().allocationSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, memory.Get());
		Procedure<PFNGLCREATETEXTURESPROC>("glCreateTextures")(GL_TEXTURE_2D_ARRAY, 1, &resources.texture);
		Procedure<PFNGLTEXTUREPARAMETERIPROC>("glTextureParameteri")(resources.texture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
		Procedure<PFNGLTEXTURESTORAGEMEM3DEXTPROC>("glTextureStorageMem3DEXT")(
			resources.texture, description.mipLevels, format, plan.width, plan.height, description.arrayLayers, resources.memory, 0);
		const auto import = [](const std::shared_ptr<VulkanExternalSemaphore>& source, GLuint& target)
		{
			auto handle = source->ExportOnce();
			Procedure<PFNGLGENSEMAPHORESEXTPROC>("glGenSemaphoresEXT")(1, &target);
			Procedure<PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC>("glImportSemaphoreWin32HandleEXT")(
				target, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle.Get());
		};
		import(resources.available, resources.availableGL);
		import(resources.ready, resources.readyGL);
		Procedure<PFNGLCREATEFRAMEBUFFERSPROC>("glCreateFramebuffers")(1, &resources.framebuffer);
		Procedure<PFNGLCREATEFRAMEBUFFERSPROC>("glCreateFramebuffers")(1, &resources.sourceFramebuffer);
		Procedure<PFNGLCREATEFRAMEBUFFERSPROC>("glCreateFramebuffers")(1, &resources.copyFramebuffer);
		for (std::size_t index = 0; index < sources.size(); ++index)
		{
			const auto& source = sources[index];
			auto& copy = resources.copies[index];
			Procedure<PFNGLCREATETEXTURESPROC>("glCreateTextures")(GL_TEXTURE_2D, 1, &copy);
			Procedure<PFNGLTEXTURESTORAGE2DPROC>("glTextureStorage2D")(copy, source.mipLevels, source.format, source.width, source.height);
			const auto parameter = Procedure<PFNGLTEXTUREPARAMETERIPROC>("glTextureParameteri");
			parameter(copy, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			parameter(copy, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			parameter(copy, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			parameter(copy, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		CheckGL("Prepare shared OpenGL compositor images");
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
	barrier.image = resources.image->Image()->image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, description.mipLevels, 0, description.arrayLayers};
	vkCmdPipelineBarrier(resources.initialize, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
	CheckVulkan(vkEndCommandBuffer(resources.initialize), "End GL handoff");
	if (!pool.Consume(reservation))
		return XR_ERROR_CALL_ORDER_INVALID;
	resources.reservation = std::move(reservation);
	output = std::move(frame);
	return XR_SUCCESS;
}

XrResult OpenGLBackend::Submit(const std::shared_ptr<OpenGLFrame>& frame)
{
	if (!frame || frame->m_resources->context != m_context || frame->m_resources->submitted)
		return XR_ERROR_CALL_ORDER_INVALID;
	auto& resources = *frame->m_resources;
	std::lock_guard lock(m_context->mutex);
	windows::OpenGLContextScope application(m_applicationDevice, m_applicationContext);
	if (!application.Current())
		return XR_ERROR_RUNTIME_FAILURE;
	const auto fence = Procedure<PFNGLFENCESYNCPROC>("glFenceSync");
	const auto wait = Procedure<PFNGLWAITSYNCPROC>("glWaitSync");
	const auto remove = Procedure<PFNGLDELETESYNCPROC>("glDeleteSync");
	const auto rendered = fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!rendered)
		return XR_ERROR_RUNTIME_FAILURE;
	glFlush();
	m_pending->frames.push_back(frame);
	resources.submitted = true;
	windows::OpenGLContextScope current(m_context->device, m_context->handle);
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
		Procedure<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT")(
			resources.availableGL, 0, nullptr, 1, &resources.texture, &layout);
		glEnable(GL_FRAMEBUFFER_SRGB);
		if (resources.capture)
		{
			const auto& source = resources.sources.front();
			const auto& image = *resources.capture;
			const auto& description = resources.image->Descriptor().image;
			for (std::uint32_t mip = 0; mip < description.mipLevels; ++mip)
			{
				if (source.layers == 1)
					Procedure<PFNGLNAMEDFRAMEBUFFERTEXTUREPROC>("glNamedFramebufferTexture")(
						resources.sourceFramebuffer, GL_COLOR_ATTACHMENT0, source.texture, mip);
				else
					Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>("glNamedFramebufferTextureLayer")(
						resources.sourceFramebuffer, GL_COLOR_ATTACHMENT0, source.texture, mip, image.arraySlice);
				Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>("glNamedFramebufferTextureLayer")(
					resources.framebuffer, GL_COLOR_ATTACHMENT0, resources.texture, mip, 0);
				const auto width = std::max(1u, description.extent.width >> mip);
				const auto height = std::max(1u, description.extent.height >> mip);
				const auto sourceHeight = std::max(1u, source.height >> mip);
				const auto x = description.mipLevels > 1 ? 0 : image.rectangle.offset.x;
				const auto y = description.mipLevels > 1 ? 0 : image.rectangle.offset.y;
				Procedure<PFNGLBLITNAMEDFRAMEBUFFERPROC>("glBlitNamedFramebuffer")(
					resources.sourceFramebuffer, resources.framebuffer, x, sourceHeight - y, x + width,
					sourceHeight - y - height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
		}
		else
		{
			for (std::size_t index = 0; index < resources.sources.size(); ++index)
			{
				const auto& source = resources.sources[index];
				const auto slice = resources.plan.draws[index].source.arraySlice;
				const bool depth = IsDepthFormat(source.format);
				const GLenum attachment = depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
				const auto attach = Procedure<PFNGLNAMEDFRAMEBUFFERTEXTUREPROC>("glNamedFramebufferTexture");
				for (const auto framebuffer : {resources.sourceFramebuffer, resources.copyFramebuffer})
				{
					attach(framebuffer, GL_DEPTH_ATTACHMENT, 0, 0);
					attach(framebuffer, GL_COLOR_ATTACHMENT0, 0, 0);
					Procedure<PFNGLNAMEDFRAMEBUFFERREADBUFFERPROC>("glNamedFramebufferReadBuffer")(
						framebuffer, depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
					Procedure<PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC>("glNamedFramebufferDrawBuffer")(
						framebuffer, depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
				}
				for (std::uint32_t mip = 0; mip < source.mipLevels; ++mip)
				{
					if (source.layers == 1)
						attach(resources.sourceFramebuffer, attachment, source.texture, mip);
					else
						Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>("glNamedFramebufferTextureLayer")(
							resources.sourceFramebuffer, attachment, source.texture, mip, slice);
					attach(resources.copyFramebuffer, attachment, resources.copies[index], mip);
					for (const auto framebuffer : {resources.sourceFramebuffer, resources.copyFramebuffer})
						if (Procedure<PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC>("glCheckNamedFramebufferStatus")(
							framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
							throw std::runtime_error("OpenGL source copy framebuffer incomplete");
					const auto width = std::max(1u, source.width >> mip), height = std::max(1u, source.height >> mip);
					Procedure<PFNGLBLITNAMEDFRAMEBUFFERPROC>("glBlitNamedFramebuffer")(
						resources.sourceFramebuffer, resources.copyFramebuffer, 0, height, width, 0, 0, 0, width, height,
						depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
				}
			}
			Procedure<PFNGLCLIPCONTROLPROC>("glClipControl")(GL_UPPER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
			Procedure<PFNGLUSEPROGRAMPROC>("glUseProgram")(m_context->program);
			Procedure<PFNGLBINDVERTEXARRAYPROC>("glBindVertexArray")(m_context->vertexArray);
			Procedure<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer")(GL_FRAMEBUFFER, resources.framebuffer);
			glViewport(0, 0, resources.plan.width, resources.plan.height);
			glEnable(GL_FRAMEBUFFER_SRGB);
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			glClearColor(0, 0, 0, 1);
			for (GLuint eye = 0; eye < resources.image->Descriptor().image.arrayLayers; ++eye)
			{
				Procedure<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>("glNamedFramebufferTextureLayer")(
					resources.framebuffer, GL_COLOR_ATTACHMENT0, resources.texture, 0, eye);
				if (Procedure<PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC>("glCheckNamedFramebufferStatus")(
					resources.framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
					throw std::runtime_error("Shared OpenGL target incomplete");
				glClear(GL_COLOR_BUFFER_BIT);
				for (std::size_t index = 0; index < resources.plan.draws.size(); ++index)
				{
					const auto& draw = resources.plan.draws[index];
					if (draw.eye != eye)
						continue;
					Procedure<PFNGLBINDTEXTUREUNITPROC>("glBindTextureUnit")(0, resources.copies[index]);
					const auto location = [&](const char* name)
						{ return Procedure<PFNGLGETUNIFORMLOCATIONPROC>("glGetUniformLocation")(m_context->program, name); };
					const auto set = [&](const char* name, const auto& value)
						{ Procedure<PFNGLUNIFORM4FVPROC>("glUniform4fv")(location(name), 1, value.data()); };
					set("layer.eyeTangents", draw.constants.eyeTangents);
					set("layer.sourceTangents", draw.constants.sourceTangents);
					set("layer.sourceFromEyeRotation", draw.constants.sourceFromEyeRotation);
					set("layer.sourceFromEyePosition", draw.constants.sourceFromEyePosition);
					set("layer.imageRectangle", draw.constants.imageRectangle);
					set("layer.quadParameters", draw.constants.quadParameters);
					Procedure<PFNGLUNIFORM4UIVPROC>("glUniform4uiv")(location("layer.control"), 1, draw.constants.control.data());
					glDrawArrays(GL_TRIANGLES, 0, 3);
				}
			}
		}
		Procedure<PFNGLSIGNALSEMAPHOREEXTPROC>("glSignalSemaphoreEXT")(
			resources.readyGL, 0, nullptr, 1, &resources.texture, &layout);
		resources.completion = fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glFlush();
		CheckGL("Submit canonical OpenGL composition");
		if (!resources.completion || !current.Restore())
			throw std::runtime_error("Cannot restore application OpenGL context");
		wait(resources.completion, 0, GL_TIMEOUT_IGNORED);
		glFlush();
		return XR_SUCCESS;
	}
	catch (...)
	{
		// Exceptional failure must complete source reads before allowing app reuse.
		windows::OpenGLContextScope cleanup(m_context->device, m_context->handle);
		if (cleanup.Current())
		{
			glFinish();
			if (!resources.completion)
				resources.completion = fence(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glFlush();
		}
		throw;
	}
}
}
