#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>

#include "VulkanProbeSupport.h"
#include "VulkanProcessPeer.h"

#include <cstdlib>

namespace
{
using namespace simxr::probe;
constexpr VkImageUsageFlags SharedUsage = ImageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
std::atomic_uint32_t s_glErrors{0};

void CheckGL(const char* operation)
{
	const auto error = glGetError();
	if (error != GL_NO_ERROR)
		throw std::runtime_error(std::string(operation) + " returned GL error " + std::to_string(error));
}

void APIENTRY Debug(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*)
{
	if (type == GL_DEBUG_TYPE_ERROR)
	{
		++s_glErrors;
		std::fprintf(stderr, "GL VALIDATION: %s\n", message);
	}
}

template<typename Function>
Function GLFunction(const char* name)
{
	auto function = reinterpret_cast<Function>(eglGetProcAddress(name));
	if (!function)
		throw std::runtime_error(std::string("Enabled GL extension command missing: ") + name);
	return function;
}

class Context
{
public:
	~Context()
	{
		if (m_context != EGL_NO_CONTEXT)
		{
			WaitForCompletion();
			eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroyContext(m_display, m_context);
		}
		if (m_surface != EGL_NO_SURFACE)
			eglDestroySurface(m_display, m_surface);
		if (m_initialized)
			eglTerminate(m_display);
	}

	bool Create(EGLDeviceEXT device)
	{
		auto platform = GLFunction<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
		m_display = platform(EGL_PLATFORM_DEVICE_EXT, device, nullptr);
		EGLint major = 0, minor = 0;
		if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, &major, &minor))
		{
			std::printf("UNAVAILABLE: EGL device initialization (0x%x)\n", eglGetError());
			return false;
		}
		m_initialized = true;
		if (!eglBindAPI(EGL_OPENGL_API))
			return false;
		const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
		EGLConfig config{};
		EGLint count = 0;
		if (!eglChooseConfig(m_display, attributes, &config, 1, &count) || !count)
			return false;
		const EGLint contextAttributes[]{EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
			EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
			EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE};
		m_context = eglCreateContext(m_display, config, EGL_NO_CONTEXT, contextAttributes);
		const EGLint surfaceAttributes[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		m_surface = eglCreatePbufferSurface(m_display, config, surfaceAttributes);
		if (m_context == EGL_NO_CONTEXT || m_surface == EGL_NO_SURFACE ||
			!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
		{
			std::printf("UNAVAILABLE: EGL OpenGL 4.5 debug context (0x%x)\n", eglGetError());
			return false;
		}
		m_current = true;
		std::printf("GL renderer=%s version=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(Debug, nullptr);
		return true;
	}

	bool SupportsSharing() const
	{
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (const auto required : {"GL_EXT_memory_object", "GL_EXT_memory_object_fd", "GL_EXT_semaphore", "GL_EXT_semaphore_fd"})
		{
			bool found = false;
			for (GLint index = 0; index < count; ++index)
				found |= std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, index)), required) == 0;
			if (!found)
			{
				std::printf("UNAVAILABLE: %s\n", required);
				return false;
			}
		}
		CheckGL("query GL sharing capabilities");
		return true;
	}

	bool Matches(VkPhysicalDevice physicalDevice) const
	{
		const auto identity = Identity(physicalDevice);
		GLint count = 0;
		glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &count);
		std::array<GLubyte, GL_UUID_SIZE_EXT> driver{};
		GLFunction<PFNGLGETUNSIGNEDBYTEVEXTPROC>("glGetUnsignedBytevEXT")(GL_DRIVER_UUID_EXT, driver.data());
		if (std::memcmp(driver.data(), identity.driverUUID, VK_UUID_SIZE))
			return false;
		for (GLint index = 0; index < count; ++index)
		{
			std::array<GLubyte, GL_UUID_SIZE_EXT> device{};
			GLFunction<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>("glGetUnsignedBytei_vEXT")(GL_DEVICE_UUID_EXT, index, device.data());
			CheckGL("query GL adapter and driver identity");
			if (!std::memcmp(device.data(), identity.deviceUUID, VK_UUID_SIZE))
				return true;
		}
		return false;
	}

	void Fence()
	{
		if (m_completion)
			glDeleteSync(m_completion);
		m_completion = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		if (!m_completion)
			throw std::runtime_error("GL completion fence creation failed");
		glFlush();
		CheckGL("flush GL producer readiness and fence");
	}

	void WaitForCompletion() noexcept
	{
		if (!m_current || !m_completion)
			return;
		auto status = glClientWaitSync(m_completion, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
		if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
		{
			std::fprintf(stderr, "FAIL: GL completion not established (0x%x); pending resources remain owned\n", status);
			std::_Exit(1);
		}
		glDeleteSync(m_completion);
		m_completion = nullptr;
	}

private:
	EGLDisplay m_display = EGL_NO_DISPLAY;
	EGLContext m_context = EGL_NO_CONTEXT;
	EGLSurface m_surface = EGL_NO_SURFACE;
	bool m_initialized = false;
	bool m_current = false;
	GLsync m_completion = nullptr;
};

class ImportedImage
{
public:
	~ImportedImage()
	{
		glDeleteFramebuffers(1, &framebuffer);
		glDeleteTextures(1, &texture);
		if (memory)
			GLFunction<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT")(1, &memory);
	}

	void Create(Device& owner, VkDeviceMemory allocation, VkDeviceSize size)
	{
		VkMemoryGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
		exportInfo.memory = allocation;
		exportInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		int descriptor = -1;
		Check(owner.Get<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(owner.handle, &exportInfo, &descriptor), "export GL image backing");
		FileDescriptor retained(descriptor);
		GLFunction<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT")(1, &memory);
		const GLint dedicated = GL_TRUE;
		GLFunction<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>("glMemoryObjectParameterivEXT")(memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
		CheckGL("configure dedicated GL memory object before FD transfer");
		GLFunction<PFNGLIMPORTMEMORYFDEXTPROC>("glImportMemoryFdEXT")(memory, size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, descriptor);
		CheckGL("import dedicated optimal Vulkan allocation into GL");
		retained.Release();
		glCreateTextures(GL_TEXTURE_2D, 1, &texture);
		glTextureParameteri(texture, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
		GLFunction<PFNGLTEXTURESTORAGEMEM2DEXTPROC>("glTextureStorageMem2DEXT")(
			texture, 1, GL_RGBA8, ImageExtent.width, ImageExtent.height, memory, 0);
		glCreateFramebuffers(1, &framebuffer);
		glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
		CheckGL("bind imported GL render target");
		if (glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			throw std::runtime_error("Imported GL render target is incomplete");
	}

	GLuint texture = 0;
	GLuint framebuffer = 0;
	GLuint memory = 0;
};

class ImportedSemaphore
{
public:
	~ImportedSemaphore()
	{
		if (handle)
			GLFunction<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT")(1, &handle);
	}

	void Create(Device& owner, VkSemaphore source)
	{
		const int descriptor = ExportSemaphore(owner, source);
		FileDescriptor retained(descriptor);
		GLFunction<PFNGLGENSEMAPHORESEXTPROC>("glGenSemaphoresEXT")(1, &handle);
		CheckGL("create GL semaphore name before FD transfer");
		GLFunction<PFNGLIMPORTSEMAPHOREFDEXTPROC>("glImportSemaphoreFdEXT")(handle, GL_HANDLE_TYPE_OPAQUE_FD_EXT, descriptor);
		CheckGL("import permanent binary semaphore into GL");
		retained.Release();
	}

	GLuint handle = 0;
};

void Prove(Context& context, VkPhysicalDevice physicalDevice, std::uint32_t family, bool verifyPixels)
{
	Device consumer;
	consumer.Create(physicalDevice, family, 0);
	const auto shared = consumer.CreateImage(true, SharedUsage);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(consumer.handle, shared, &requirements);
	const auto memoryType = MemoryType(physicalDevice, requirements.memoryTypeBits);
	const auto backing = consumer.Allocate(shared, memoryType, requirements.size, true);
	ImportedImage image;
	image.Create(consumer, backing, requirements.size);
	const auto available = consumer.CreateSemaphore(true);
	const auto ready = consumer.CreateSemaphore(true);
	ImportedSemaphore glAvailable, glReady;
	glAvailable.Create(consumer, available);
	glReady.Create(consumer, ready);
	struct Drain
	{
		Context& context;
		~Drain()
		{
			try { context.Fence(); context.WaitForCompletion(); }
			catch (...) { std::fputs("FAIL: GL work could not be drained before resource retirement\n", stderr); std::_Exit(1); }
		}
	} drain{context};

	auto initialize = consumer.BeginCommands();
	Barrier(initialize, shared, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		0, 0, family, VK_QUEUE_FAMILY_EXTERNAL);
	consumer.Submit(initialize, VK_NULL_HANDLE, available);
	std::array<VkImage, 3> retained{};
	for (std::uint32_t frame = 0; frame < retained.size(); ++frame)
	{
		const GLenum acquiredLayout = GL_LAYOUT_GENERAL_EXT;
		GLFunction<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT")(glAvailable.handle, 0, nullptr, 1, &image.texture, &acquiredLayout);
		std::array<GLfloat, 4> color{0, 0, 0, 1};
		color[frame] = 1;
		glClearNamedFramebufferfv(image.framebuffer, GL_COLOR, 0, color.data());
		const GLenum releasedLayout = GL_LAYOUT_TRANSFER_SRC_EXT;
		GLFunction<PFNGLSIGNALSEMAPHOREEXTPROC>("glSignalSemaphoreEXT")(glReady.handle, 0, nullptr, 1, &image.texture, &releasedLayout);
		context.Fence();
		retained[frame] = consumer.CreateImage(false);
		VkMemoryRequirements retainedRequirements{};
		vkGetImageMemoryRequirements(consumer.handle, retained[frame], &retainedRequirements);
		consumer.Allocate(retained[frame], MemoryType(physicalDevice, retainedRequirements.memoryTypeBits), retainedRequirements.size, false);
		auto copy = consumer.BeginCommands();
		Barrier(copy, shared, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			0, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, family);
		Barrier(copy, retained[frame], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		VkImageCopy region{};
		region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.extent = ImageExtent;
		vkCmdCopyImage(copy, shared, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, retained[frame], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		Barrier(copy, retained[frame], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
		Barrier(copy, shared, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			VK_ACCESS_TRANSFER_READ_BIT, 0, family, VK_QUEUE_FAMILY_EXTERNAL);
		consumer.Submit(copy, ready, available);
		consumer.WaitForCompletion();
		context.WaitForCompletion();
		std::printf("GL->Vulkan frame=%u copied=private retained=GPU sourceReusable=yes\n", frame + 1);
	}
	if (verifyPixels)
		for (std::uint32_t frame = 0; frame < retained.size(); ++frame)
			consumer.VerifyImage(physicalDevice, retained[frame], frame);
	CheckGL("complete GL image handoffs");
}
void ProveProcess(Context& context, VkPhysicalDevice physicalDevice, std::uint32_t family, std::uint32_t consumerQueue, bool verifyPixels, bool disconnect, const std::string& companion, const std::string& screenshot)
{
	Device owner;
	owner.Create(physicalDevice, family, 0);
	const auto source = owner.CreateImage(true, SharedUsage);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(owner.handle, source, &requirements);
	SharingPacket packet;
	packet.imageUsage = SharedUsage;
	const auto identity = Identity(physicalDevice);
	std::copy_n(identity.deviceUUID, VK_UUID_SIZE, packet.deviceUuid.begin());
	std::copy_n(identity.driverUUID, VK_UUID_SIZE, packet.driverUuid.begin());
	packet.allocationSize = requirements.size;
	packet.memoryType = MemoryType(physicalDevice, requirements.memoryTypeBits);
	packet.family = family;
	packet.consumerQueue = consumerQueue;
	const auto backing = owner.Allocate(source, packet.memoryType, requirements.size, true);
	ImportedImage image;
	image.Create(owner, backing, requirements.size);
	const auto available = owner.CreateSemaphore(true);
	const auto ready = owner.CreateSemaphore(true);
	ImportedSemaphore glAvailable, glReady;
	glAvailable.Create(owner, available);
	glReady.Create(owner, ready);
	struct Drain
	{
		Context& context;
		~Drain()
		{
			try { context.Fence(); context.WaitForCompletion(); }
			catch (...) { std::fputs("FAIL: GL work could not drain before process-resource retirement\n", stderr); std::_Exit(1); }
		}
	} drain{context};
	sharing::Descriptors exported;
	VkMemoryGetFdInfoKHR memoryInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	memoryInfo.memory = backing;
	memoryInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	Check(owner.Get<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(owner.handle, &memoryInfo, &exported.values[0]), "export GL backing to separate Vulkan consumer");
	exported.values[1] = ExportSemaphore(owner, ready);
	exported.values[2] = ExportSemaphore(owner, available);
	ConsumerProcess consumer(verifyPixels, disconnect, companion, screenshot);
	sharing::Send(consumer.Socket(), &packet, sizeof(packet), exported.values);
	auto initialize = owner.BeginCommands();
	Barrier(initialize, source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, family, VK_QUEUE_FAMILY_EXTERNAL);
	owner.Submit(initialize, VK_NULL_HANDLE, available);
	for (std::uint32_t frame = 0; frame < 3; ++frame)
	{
		const GLenum layout = GL_LAYOUT_GENERAL_EXT;
		GLFunction<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT")(glAvailable.handle, 0, nullptr, 1, &image.texture, &layout);
		std::array<GLfloat, 4> color{0, 0, 0, 1};
		color[frame] = 1;
		glClearNamedFramebufferfv(image.framebuffer, GL_COLOR, 0, color.data());
		GLFunction<PFNGLSIGNALSEMAPHOREEXTPROC>("glSignalSemaphoreEXT")(glReady.handle, 0, nullptr, 1, &image.texture, &layout);
		context.Fence();
		sharing::Send(consumer.Socket(), &frame, sizeof(frame));
		std::uint32_t completed = UINT32_MAX;
		sharing::Descriptors acknowledgement;
		consumer.Receive(&completed, sizeof(completed), acknowledgement);
		if (completed != frame) throw std::runtime_error("GL/Vulkan consumer completion identity mismatch");
		context.WaitForCompletion();
		std::printf("GL->Vulkan processFrame=%u consumer=separate retained=GPU completed=true\n", frame + 1);
		if (disconnect)
		{
			consumer.ObserveClosed();
			consumer.Join();
			GLFunction<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT")(glAvailable.handle, 0, nullptr, 1, &image.texture, &layout);
			const std::array<GLfloat, 4> recovered{0, 0, 1, 1};
			glClearNamedFramebufferfv(image.framebuffer, GL_COLOR, 0, recovered.data());
			context.Fence();
			context.WaitForCompletion();
			owner.WaitForCompletion();
			std::puts("GL consumerDisconnected=observed sourceReused=true noFutureConsumerWait=true");
			return;
		}
	}
	std::uint32_t completed = 0;
	sharing::Descriptors acknowledgement;
	consumer.Receive(&completed, sizeof(completed), acknowledgement);
	if (completed != 3) throw std::runtime_error("GL/Vulkan consumer retention proof failed");
	consumer.Join();
	owner.WaitForCompletion();
	CheckGL("complete separate-process GL handoffs");
}

}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	bool verifyPixels = false;
	std::string companion;
	std::string companionScreenshot;
	bool crossProcess = false;
	bool consumer = false;
	bool disconnect = false;
	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--verify-pixels") == 0) verifyPixels = true;
		else if (std::strcmp(argv[index], "--companion") == 0 && index + 1 < argc) { companion = argv[++index]; crossProcess = true; }
		else if (std::strcmp(argv[index], "--companion-screenshot") == 0 && index + 1 < argc) companionScreenshot = argv[++index];
		else if (std::strcmp(argv[index], "--cross-process") == 0) crossProcess = true;
		else if (std::strcmp(argv[index], "--verify-disconnect") == 0) { crossProcess = true; disconnect = true; }
		else if (std::strcmp(argv[index], "--consumer-disconnect") == 0) { consumer = true; disconnect = true; }
		else if (std::strcmp(argv[index], "--consumer") == 0) consumer = true;
		else if (std::strcmp(argv[index], "--consumer-verify-pixels") == 0) { consumer = true; verifyPixels = true; }
		else
		{
			std::fputs("Usage: opengl_vulkan_sharing_probe [--cross-process] [--verify-pixels] [--verify-disconnect] [--companion PATH] [--companion-screenshot PATH]\n", stderr);
			return 2;
		}
	}
	if ((!companion.empty() && disconnect) || (!companionScreenshot.empty() && companion.empty()))
	{ std::fputs("Companion display, screenshot and disconnect options are inconsistent\n", stderr); return 2; }
	if (verifyPixels && disconnect) { std::fputs("Pixel verification and early disconnect are separate proofs\n", stderr); return 2; }
	try
	{
		if (consumer)
		{
			RunConsumer(verifyPixels, disconnect);
			if (s_validationErrors.load()) throw std::runtime_error("Consumer Vulkan validation failed during teardown");
			return 0;
		}
		bool qualified = false;
		{
			Instance instance;
			instance.Create();
			auto query = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
			if (!query)
			{
				std::puts("SKIP: EGL device enumeration is unavailable");
				return 77;
			}
			EGLint count = 0;
			if (!query(0, nullptr, &count))
				throw std::runtime_error("EGL device enumeration failed");
			std::vector<EGLDeviceEXT> eglDevices(count);
			if (!query(count, eglDevices.data(), &count))
				throw std::runtime_error("EGL device enumeration failed");
			std::uint32_t vkCount = 0;
			Check(vkEnumeratePhysicalDevices(instance.handle, &vkCount, nullptr), "enumerate GL-compatible Vulkan devices");
			std::vector<VkPhysicalDevice> physicalDevices(vkCount);
			Check(vkEnumeratePhysicalDevices(instance.handle, &vkCount, physicalDevices.data()), "enumerate GL-compatible Vulkan devices");
			for (const auto eglDevice : eglDevices)
			{
				Context context;
				if (!context.Create(eglDevice) || !context.SupportsSharing())
					continue;
				bool matched = false;
				for (const auto physicalDevice : physicalDevices)
				{
					if (!context.Matches(physicalDevice))
						continue;
					matched = true;
					std::uint32_t family = 0, queue = 0;
					if (!SupportsSharing(physicalDevice, family, queue, SharedUsage))
						continue;
					if (crossProcess) ProveProcess(context, physicalDevice, family, queue, verifyPixels, disconnect, companion, companionScreenshot);
					else Prove(context, physicalDevice, family, verifyPixels);
					qualified = true;
				}
				if (!matched)
					std::puts("UNAVAILABLE: no Vulkan adapter matches both GL device and driver UUID");
			}
		}
		if (s_validationErrors.load() || s_glErrors.load())
			throw std::runtime_error("Graphics validation reported errors including teardown");
		if (!qualified)
		{
			std::puts("SKIP: no matching GL/Vulkan pair supports the required memory and semaphore handoff");
			return 77;
		}
		if (disconnect) { std::puts("PASS: completed-boundary Vulkan consumer disconnect and GL source reuse"); return 0; }
		std::puts(verifyPixels ? "PASS: GL rendering, GPU handoff and exact retained colors after source reuse; explicit CPU analysis" :
			"PASS: GL rendering into Vulkan backing, GPU semaphore handoff and private retained copies; no pixel readback");
		return 0;
	}
	catch (const ConsumerUnavailable& error)
	{
		std::fprintf(stderr, "SKIP: %s\n", error.what());
		return 77;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
