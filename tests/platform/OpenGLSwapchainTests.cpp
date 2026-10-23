#include "platform/windows/OpenGLSwapchain.h"
#include "Pch.h"

#include <GL/glext.h>
#include <GL/wglext.h>

#include "../TestSupport.h"

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <source_location>
#include <thread>

namespace
{
void Check(bool condition, const std::source_location location = std::source_location::current())
{
	Require(condition,
			(std::string("OpenGL swapchain fixture failed at line ") + std::to_string(location.line())).c_str());
}

using CreateContext = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using GenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
using DeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
constexpr GLenum TextureArray = 0x8C1A;
constexpr GLenum TextureArrayBinding = 0x8C1D;
constexpr GLenum UnpackBuffer = 0x88EC;
constexpr GLenum UnpackBufferBinding = 0x88EF;
constexpr GLenum UnpackImageHeight = 0x806E;
constexpr GLenum UnpackSkipImages = 0x806D;

template <class Function> Function Procedure(const char* name)
{
	const auto procedure = wglGetProcAddress(name);
	const auto address = std::bit_cast<std::uintptr_t>(procedure);
	if (address <= 3 || address == UINTPTR_MAX)
		return nullptr;
	return std::bit_cast<Function>(procedure);
}

class ContextFixture final
{
  public:
	ContextFixture()
	{
		m_window = CreateWindowExW(0, L"STATIC", L"SimXR GL swapchain fixture", WS_POPUP, 0, 0, 16, 16, nullptr,
								   nullptr, GetModuleHandleW(nullptr), nullptr);
		if (!m_window)
			return;
		m_device = GetDC(m_window);
		PIXELFORMATDESCRIPTOR format{};
		format.nSize = sizeof(format);
		format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		format.iPixelType = PFD_TYPE_RGBA;
		format.cColorBits = 32;
		const auto selected = ChoosePixelFormat(m_device, &format);
		if (!selected || !SetPixelFormat(m_device, selected, &format))
			return;
		m_context = wglCreateContext(m_device);
		if (!m_context || !wglMakeCurrent(m_device, m_context))
			return;
		const auto create = Procedure<CreateContext>("wglCreateContextAttribsARB");
		if (!create)
			return;
		const int attributes[]{WGL_CONTEXT_MAJOR_VERSION_ARB,
							   3,
							   WGL_CONTEXT_MINOR_VERSION_ARB,
							   3,
							   WGL_CONTEXT_PROFILE_MASK_ARB,
							   WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
							   WGL_CONTEXT_FLAGS_ARB,
							   WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
							   0};
		const auto modern = create(m_device, nullptr, attributes);
		if (!modern)
			return;
		wglMakeCurrent(nullptr, nullptr);
		wglDeleteContext(m_context);
		m_context = modern;
		m_ready = wglMakeCurrent(m_device, m_context) != FALSE;
	}
	~ContextFixture()
	{
		wglMakeCurrent(nullptr, nullptr);
		if (m_context)
			wglDeleteContext(m_context);
		if (m_device)
			ReleaseDC(m_window, m_device);
		if (m_window)
			DestroyWindow(m_window);
	}
	bool Ready() const noexcept
	{
		return m_ready;
	}
	HDC Device() const noexcept
	{
		return m_device;
	}
	HGLRC Context() const noexcept
	{
		return m_context;
	}

  private:
	HWND m_window = nullptr;
	HDC m_device = nullptr;
	HGLRC m_context = nullptr;
	bool m_ready = false;
};

GLint Integer(GLenum parameter)
{
	GLint value = 0;
	glGetIntegerv(parameter, &value);
	return value;
}

XrSwapchainCreateInfo Description()
{
	XrSwapchainCreateInfo description{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	description.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	description.format = 0x8058;
	description.width = 16;
	description.height = 8;
	description.arraySize = 1;
	description.faceCount = 1;
	description.mipCount = 3;
	description.sampleCount = 1;
	return description;
}

std::atomic<unsigned> s_debugErrors{0};
void APIENTRY DebugMessage(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*)
{
	if (type == GL_DEBUG_TYPE_ERROR)
	{
		++s_debugErrors;
		std::cerr << "OpenGL error: " << message << '\n';
	}
}

void Exercise(ContextFixture& fixture)
{
	const auto major = Integer(GL_MAJOR_VERSION), minor = Integer(GL_MINOR_VERSION);
	const auto profile = Integer(GL_CONTEXT_PROFILE_MASK), flags = Integer(GL_CONTEXT_FLAGS);
	std::cerr << "Application context: OpenGL " << major << '.' << minor << ", profile=" << profile
			  << ", flags=" << flags << '\n';
	Check(major == 3 && minor == 3 && profile == GL_CONTEXT_CORE_PROFILE_BIT &&
		  (flags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT));
	Procedure<PFNGLDEBUGMESSAGECALLBACKPROC>("glDebugMessageCallback")(DebugMessage, nullptr);
	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	using simxr::windows::OpenGLSwapchain;
	const auto genBuffers = Procedure<GenBuffers>("glGenBuffers");
	const auto bindBuffer = Procedure<BindBuffer>("glBindBuffer");
	const auto deleteBuffers = Procedure<DeleteBuffers>("glDeleteBuffers");
	Check(genBuffers && bindBuffer && deleteBuffers);
	GLuint unpackBuffer = 0;
	genBuffers(1, &unpackBuffer);
	bindBuffer(UnpackBuffer, unpackBuffer);
	// Allocate storage across native context migration; allocation must
	// preserve this binding and every unpack parameter.
	Procedure<PFNGLBUFFERDATAPROC>("glBufferData")(UnpackBuffer, 32768, nullptr, GL_STATIC_DRAW);
	std::array<GLuint, 2> hostTextures{};
	glGenTextures(static_cast<GLsizei>(hostTextures.size()), hostTextures.data());
	glBindTexture(GL_TEXTURE_2D, hostTextures[0]);
	glBindTexture(TextureArray, hostTextures[1]);
	const std::array<std::pair<GLenum, GLint>, 8> unpack{{{GL_UNPACK_ALIGNMENT, 8},
														  {GL_UNPACK_ROW_LENGTH, 31},
														  {GL_UNPACK_SKIP_ROWS, 7},
														  {GL_UNPACK_SKIP_PIXELS, 5},
														  {GL_UNPACK_SWAP_BYTES, 1},
														  {GL_UNPACK_LSB_FIRST, 1},
														  {UnpackImageHeight, 29},
														  {UnpackSkipImages, 3}}};
	for (const auto& [parameter, value] : unpack)
		glPixelStorei(parameter, value);
	Check(glGetError() == GL_NO_ERROR);

	auto description = Description();
	std::unique_ptr<OpenGLSwapchain> owner;
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_SUCCESS);
	Check(owner && owner->Images().size() == 3);
	Check(Integer(GL_TEXTURE_BINDING_2D) == static_cast<GLint>(hostTextures[0]));
	Check(Integer(TextureArrayBinding) == static_cast<GLint>(hostTextures[1]));
	Check(Integer(UnpackBufferBinding) == static_cast<GLint>(unpackBuffer));
	for (const auto& [parameter, value] : unpack)
		Check(Integer(parameter) == value);
	Check(wglGetCurrentContext() == fixture.Context());
	const auto first = owner->Images().front();
	glBindTexture(GL_TEXTURE_2D, first);
	GLint width = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &width);
	Check(width == 4);
	glBindTexture(GL_TEXTURE_2D, hostTextures[0]);

	const auto* preserved = owner.get();
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) ==
		  XR_ERROR_CALL_ORDER_INVALID);
	Check(owner.get() == preserved && glIsTexture(first));
	description.format = -1;
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) ==
		  XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
	Check(owner.get() == preserved && glIsTexture(first));
	description = Description();
	description.mipCount = 32;
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) ==
		  XR_ERROR_VALIDATION_FAILURE);
	Check(owner.get() == preserved);
	description = Description();
	description.width = static_cast<std::uint32_t>(Integer(GL_MAX_TEXTURE_SIZE)) + 1;
	std::unique_ptr<OpenGLSwapchain> rejected;
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, rejected) ==
		  XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
	Check(!rejected && owner.get() == preserved);

	// A context bound on another thread is unavailable, not permission to delete
	// names in whichever context happens to be current on the calling thread.
	Check(wglMakeCurrent(nullptr, nullptr));
	const auto hostContext = wglCreateContext(fixture.Device());
	Check(hostContext && wglMakeCurrent(fixture.Device(), hostContext));
	std::mutex mutex;
	std::condition_variable changed;
	bool acquired = false;
	bool release = false;
	bool workerCurrent = false;
	std::thread borrower(
		[&]
		{
			workerCurrent = wglMakeCurrent(fixture.Device(), fixture.Context()) != FALSE;
			{
				std::unique_lock lock(mutex);
				acquired = true;
				changed.notify_one();
				changed.wait(lock,
							 [&]
							 {
								 return release;
							 });
			}
			wglMakeCurrent(nullptr, nullptr);
		});
	{
		std::unique_lock lock(mutex);
		changed.wait(lock,
					 [&]
					 {
						 return acquired;
					 });
	}
	const auto rejectedClose = !owner->Close();
	const auto retainedImages = owner->Images().size();
	const auto restoredHost = wglGetCurrentContext() == hostContext;
	{
		std::lock_guard lock(mutex);
		release = true;
	}
	changed.notify_one();
	borrower.join();
	Check(workerCurrent && rejectedClose && retainedImages == 3 && restoredHost);
	Check(owner->Close());
	Check(owner->Images().empty());
	Check(wglGetCurrentContext() == hostContext);
	Check(wglMakeCurrent(nullptr, nullptr));
	Check(wglDeleteContext(hostContext));
	Check(wglMakeCurrent(fixture.Device(), fixture.Context()));
	Check(!glIsTexture(first));
	Check(owner->Close());

	// Array depth and all mip levels remain the actual submitted description.
	description = Description();
	description.arraySize = 2;
	description.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
	owner.reset();
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_SUCCESS);
	Check(owner->Images().size() == 1);
	glBindTexture(TextureArray, owner->Images()[0]);
	GLint depth = 0;
	glGetTexLevelParameteriv(TextureArray, 2, 0x8071, &depth);
	glGetTexLevelParameteriv(TextureArray, 2, GL_TEXTURE_WIDTH, &width);
	Check(depth == 2 && width == 4);
	glBindTexture(TextureArray, hostTextures[1]);
	Check(owner->Close());
	Check(Integer(TextureArrayBinding) == static_cast<GLint>(hostTextures[1]));
	Check(glGetError() == GL_NO_ERROR);
	owner.reset();
	for (const auto format : OpenGLSwapchain::Formats())
	{
		description.format = format;
		Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_SUCCESS);
		Check(Integer(TextureArrayBinding) == static_cast<GLint>(hostTextures[1]));
		Check(Integer(UnpackBufferBinding) == static_cast<GLint>(unpackBuffer));
		Check(owner->Close());
		owner.reset();
	}
	std::uint32_t maximumSamples = 1;
	Check(OpenGLSwapchain::MaxSampleCount(maximumSamples) == XR_SUCCESS && maximumSamples > 1);
	Check(wglGetCurrentContext() == fixture.Context());
	for (const auto format : OpenGLSwapchain::Formats())
		for (const auto layers : {1u, 2u})
		{
			description.format = format;
			description.arraySize = layers;
			description.sampleCount = maximumSamples;
			description.mipCount = 1;
			Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_SUCCESS);
			Check(Integer(GL_TEXTURE_BINDING_2D) == static_cast<GLint>(hostTextures[0]));
			Check(Integer(TextureArrayBinding) == static_cast<GLint>(hostTextures[1]));
			Check(Integer(UnpackBufferBinding) == static_cast<GLint>(unpackBuffer));
			GLint samples = 0;
			Procedure<PFNGLGETTEXTURELEVELPARAMETERIVPROC>("glGetTextureLevelParameteriv")(
				owner->Images().front(), 0, GL_TEXTURE_SAMPLES, &samples);
			Check(samples == static_cast<GLint>(maximumSamples));
			Check(owner->Close());
			owner.reset();
		}
	description.mipCount = 2;
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_ERROR_VALIDATION_FAILURE);
	description.mipCount = 1;
	description.sampleCount = 3;
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_ERROR_VALIDATION_FAILURE);
	description.sampleCount = 1;
	Check(s_debugErrors == 0);
	Procedure<PFNGLDEBUGMESSAGECALLBACKPROC>("glDebugMessageCallback")(nullptr, nullptr);
	// A pre-existing GL error is observed once and fails creation; it is never
	// silently drained into a successful, misleading allocation result.
	glEnable(0xFFFFFFFF);
	Check(OpenGLSwapchain::Create(fixture.Device(), fixture.Context(), description, owner) == XR_ERROR_RUNTIME_FAILURE);
	Check(!owner && glGetError() == GL_NO_ERROR);
	bindBuffer(UnpackBuffer, 0);
	deleteBuffers(1, &unpackBuffer);
	glDeleteTextures(static_cast<GLsizei>(hostTextures.size()), hostTextures.data());
}
} // namespace

int main()
{
	ContextFixture fixture;
	if (!fixture.Ready())
	{
		std::cout << "SKIP: OpenGL 3.3 WGL context unavailable\n";
		return 77;
	}
	Exercise(fixture);
	std::cout << "PASS: OpenGL 3.3 formats, mip arrays, exact maximum samples, binding preservation and context-close retry\n";
	return 0;
}
