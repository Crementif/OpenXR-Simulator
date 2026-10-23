#include "graphics/ColorPixels.h"
#include "graphics/SwapchainAccess.h"
#include "graphics/backends/opengl/OpenGLBackend.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include <GL/glext.h>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

#ifdef SIMXR_TEST_WGL_FAILURE
using MakeCurrentFunction = BOOL(WINAPI*)(HDC, HGLRC);
static HGLRC s_applicationContext = nullptr;
static bool s_failRestoration = false;
static bool s_rejectContexts = false;
static bool s_rejectPrivateContext = false;
static unsigned s_rejectedContexts = 0;

static BOOL WINAPI MakeCurrent(HDC device, HGLRC context)
{
	static const auto s_makeCurrent =
		std::bit_cast<MakeCurrentFunction>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglMakeCurrent"));
	if (s_failRestoration && context == s_applicationContext)
	{
		s_failRestoration = false;
		s_rejectContexts = true;
		s_makeCurrent(nullptr, nullptr);
	}
	if (context && (s_rejectContexts || (s_rejectPrivateContext && context != s_applicationContext)))
	{
		++s_rejectedContexts;
		SetLastError(ERROR_BUSY);
		return FALSE;
	}
	return s_makeCurrent(device, context);
}

extern "C"
{
	MakeCurrentFunction __wrap___imp_wglMakeCurrent = MakeCurrent;
}
#endif
using namespace simxr;
void Require(bool result, const char* message)
{
	if (!result) throw std::runtime_error(message);
}
struct Application
{
	HWND window = nullptr;
	HDC device = nullptr;
	HGLRC context = nullptr;
	GLuint texture = 0;
	GLuint depthTexture = 0;
	Application()
	{
		window = CreateWindowExW(0, L"STATIC", L"OpenGL compositor fixture", WS_POPUP, 0, 0, 16, 16,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		Require(window != nullptr, "Create application window");
		device = GetDC(window);
		PIXELFORMATDESCRIPTOR format{};
		format.nSize = sizeof(format);
		format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		format.iPixelType = PFD_TYPE_RGBA;
		format.cColorBits = 32;
		Require(SetPixelFormat(device, ChoosePixelFormat(device, &format), &format), "Set application pixel format");
		context = wglCreateContext(device);
		Require(context && wglMakeCurrent(device, context), "Activate application context");
		glGenTextures(1, &depthTexture);
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		std::array<std::uint8_t, 8 * 8 * 4> pixels{};
		for (std::size_t y = 0; y < 8; ++y)
			for (std::size_t x = 0; x < 8; ++x)
			{
				pixels[(y * 8 + x) * 4 + (y < 4 ? 2 : 0)] = 255;
				pixels[(y * 8 + x) * 4 + 3] = 255;
			}
		glTexImage2D(GL_TEXTURE_2D, 0, 0x8058, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		Require(glGetError() == GL_NO_ERROR, "Upload application image");
	}
	~Application()
	{
		if (context)
		{
			wglMakeCurrent(device, context);
			glDeleteTextures(1, &depthTexture);
			glDeleteTextures(1, &texture);
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(context);
		}
		if (device) ReleaseDC(window, device);
		if (window) DestroyWindow(window);
	}
};
static void VerifyMultisample(Application& app, const VulkanInspectionDevice& device)
{
	const auto createTexture = std::bit_cast<PFNGLCREATETEXTURESPROC>(wglGetProcAddress("glCreateTextures"));
	const auto createFramebuffer = std::bit_cast<PFNGLCREATEFRAMEBUFFERSPROC>(wglGetProcAddress("glCreateFramebuffers"));
	const auto attach = std::bit_cast<PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC>(wglGetProcAddress("glNamedFramebufferTextureLayer"));
	const auto bindFramebuffer = std::bit_cast<PFNGLBINDFRAMEBUFFERPROC>(wglGetProcAddress("glBindFramebuffer"));
	const auto sampleMask = std::bit_cast<PFNGLSAMPLEMASKIPROC>(wglGetProcAddress("glSampleMaski"));
	for (const GLenum format : {GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA16F, GL_RGBA32F, GL_RGB10_A2,
		GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8})
	{
		const bool depth = format == GL_DEPTH_COMPONENT16 || format == GL_DEPTH_COMPONENT32F || format == GL_DEPTH24_STENCIL8;
		GLuint texture = 0, framebuffer = 0;
		createTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 1, &texture);
		std::bit_cast<PFNGLTEXTURESTORAGE3DMULTISAMPLEPROC>(wglGetProcAddress("glTextureStorage3DMultisample"))(
			texture, 4, format, 8, 8, 2, GL_TRUE);
		createFramebuffer(1, &framebuffer);
		bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		glDrawBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
		glReadBuffer(depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
		glViewport(0, 0, 8, 8);
		glDisable(GL_FRAMEBUFFER_SRGB);
		glDisable(GL_BLEND);
		glEnable(GL_MULTISAMPLE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
		glDepthMask(GL_TRUE);
		glClearDepth(0.125);
		for (GLuint layer = 0; layer < 2; ++layer)
		{
			attach(framebuffer, depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, texture, 0, layer);
			glClearColor(0, layer == 0 ? 1.0f : 0.0f, 0, 1);
			glClear(depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
		}
		glEnable(GL_SAMPLE_MASK);
		sampleMask(0, 1);
		glEnable(GL_SCISSOR_TEST);
		for (int half = 0; half < 2; ++half)
		{
			glScissor(0, half * 4, 8, 4);
			glColor4f(half ? 1.0f : 0.0f, 0, half ? 0.0f : 1.0f, 1);
			const float z = half ? 0.5f : 0.0f;
			glBegin(GL_TRIANGLE_STRIP);
			glVertex3f(-1, -1, z);
			glVertex3f(1, -1, z);
			glVertex3f(-1, 1, z);
			glVertex3f(1, 1, z);
			glEnd();
		}
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_SAMPLE_MASK);
		glDisable(GL_DEPTH_TEST);
		Require(glGetError() == GL_NO_ERROR, "Render sample-distinct OpenGL array");
		for (int mode = 0; mode < (depth ? 2 : 3); ++mode)
		{
			VulkanBackend delivery(device.Device(), 8, device.EnabledExtensions(), true);
			VulkanInspection inspection(OpenGLBackend::Identity(app.device, app.context), 8);
			auto backend = std::make_unique<OpenGLBackend>(app.device, app.context, device.Device(), device.EnabledExtensions());
			const OpenGLCompositionSource source{texture, format, 8, 8, 2, 1, 4};
			LayerDepthDescription selected{};
			selected.image.arraySlice = 1;
			selected.image.rectangle = {{2, 1}, {4, 6}};
			selected.minDepth = 0;
			selected.maxDepth = 1;
			selected.nearZ = 0.1f;
			selected.farZ = 10;
			CompositionPlan plan;
			std::shared_ptr<OpenGLFrame> frame;
			if (depth)
			{
				Require(PrepareDepthComposition(selected, mode == 1, {8, 8}, true, plan) == XR_SUCCESS, "Plan MSAA depth");
				Require(backend->Prepare(plan, std::span{&source, 1}, frame, 1) == XR_SUCCESS, "Prepare MSAA depth");
			}
			else if (mode == 0)
			{
				plan.width = 4;
				plan.height = 6;
				CompositionDraw draw{};
				draw.source = selected.image;
				draw.constants.eyeTangents = draw.constants.sourceTangents = {-1, 1, 1, -1};
				draw.constants.sourceFromEyeRotation = {0, 0, 0, 1};
				draw.constants.imageRectangle = {0.25f, 0.125f, 0.5f, 0.75f};
				plan.draws.push_back(draw);
				Require(backend->Prepare(plan, std::span{&source, 1}, frame, 1) == XR_SUCCESS, "Prepare MSAA color");
			}
			else
				Require(backend->PrepareCapture(selected.image, source, frame, mode == 2) == XR_SUCCESS, "Prepare MSAA raw/retained color");
			std::shared_ptr<VulkanFrame> copied;
			Require(delivery.PrepareNativeCopy(frame->Image(), frame->Ready(), 0, frame, {}, copied) == XR_SUCCESS, "Prepare MSAA handoff");
			auto transfer = inspection.Prepare(copied);
			Require(backend->Submit(frame) == XR_SUCCESS, "Submit MSAA OpenGL");
			Require(wglGetCurrentContext() == app.context, "MSAA submission changed application context");
			Require(delivery.Submit(copied) == XR_SUCCESS, "Submit MSAA handoff");
			transfer->Commit();
			auto retained = transfer->Output();
			CheckVulkan(delivery.Drain(5000000000ULL), "Drain MSAA delivery");
			CheckVulkan(inspection.Drain(), "Drain MSAA inspection");
			transfer.reset();
			copied.reset();
			frame.reset();
			backend.reset();
			if (mode == 2)
			{
				glDeleteTextures(1, &texture);
				texture = 0;
			}
			auto exported = retained->Export();
			std::shared_ptr<const VulkanImagePixels> pixels;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!(pixels = exported->Pixels()) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Require(pixels && pixels->layers == 1 && pixels->width == (mode == 2 ? 8u : 4u) &&
				pixels->height == (mode == 2 ? 8u : 6u), "MSAA retention lost full source or crop extent");
			const auto stride = ColorPixelBytes(pixels->format);
			for (const auto y : {1u, pixels->height - 2})
			{
				const auto color = DecodeColorPixel(pixels->format, std::as_bytes(std::span(pixels->rgba)).subspan(
					(y * pixels->width + pixels->width / 2) * stride, stride));
				const bool top = y < pixels->height / 2;
				float value = depth ? (top ? 0.75f : 0.5f) : 0.25f;
				if (depth && mode == 1)
					value = 0.1f * value / (10 * (1 - value) + 0.1f * value);
				if (depth || mode == 0 || format == GL_SRGB8_ALPHA8)
					value = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
				for (std::size_t channel = 0; channel < 3; ++channel)
				{
					const float expected = depth || channel == (top ? 0u : 2u) ? value : 0;
					Require(std::abs(color[channel] - expected) <= 0.012f, "MSAA resolve/depth sample zero/orientation mismatch");
				}
				Require(color[3] == 1, "MSAA alpha changed");
			}
		}
		glDeleteTextures(1, &texture);
		std::bit_cast<PFNGLDELETEFRAMEBUFFERSPROC>(wglGetProcAddress("glDeleteFramebuffers"))(1, &framebuffer);
	}
	std::puts("PASS: OpenGL 4x MSAA color resolve, sample-zero depth, cropped arrays and retained full source after producer teardown");
}

static void VerifySubmissionFailures(const VulkanInspectionDevice& device)
{
	Application app;
	CompositionPlan plan{8, 8, {}, {}};
	CompositionDraw draw{};
	draw.constants.eyeTangents = draw.constants.sourceTangents = {-1, 1, 1, -1};
	draw.constants.sourceFromEyeRotation = {0, 0, 0, 1};
	draw.constants.imageRectangle = {0, 0, 1, 1};
	plan.draws.push_back(draw);
	const OpenGLCompositionSource missing{app.texture, GL_RGBA8, 8, 8};
	{
		OpenGLBackend backend(app.device, app.context, device.Device(), device.EnabledExtensions());
		std::shared_ptr<OpenGLFrame> frame;
		Require(backend.Prepare(plan, std::span{&missing, 1}, frame) == XR_SUCCESS, "Prepare rejected GL source");
		Require(!frame->SourcesUsed(), "Prepared GL frame marked sources used");
		glDeleteTextures(1, &app.texture);
		app.texture = 0;
		bool rejected = false;
		try
		{
			rejected = XR_FAILED(backend.Submit(frame));
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Require(rejected && !frame->SourcesUsed(), "Pre-read GL failure marked source use");
		Require(wglGetCurrentContext() == app.context, "Rejected GL source changed application context");
	}
#ifdef SIMXR_TEST_WGL_FAILURE
	struct ResetContextFailure
	{
		~ResetContextFailure()
		{
			s_failRestoration = s_rejectContexts = s_rejectPrivateContext = false;
			s_applicationContext = nullptr;
		}
	} reset;
	glGenTextures(1, &app.texture);
	const std::array textures{app.texture, app.depthTexture};
	std::array<OpenGLCompositionSource, 2> sources;
	for (std::size_t eye = 0; eye < textures.size(); ++eye)
	{
		glBindTexture(GL_TEXTURE_2D, textures[eye]);
		std::array<std::uint8_t, 8 * 8 * 4> pixels{};
		for (std::size_t index = 0; index < pixels.size(); index += 4)
		{
			pixels[index + eye] = 255;
			pixels[index + 3] = 255;
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		sources[eye] = {textures[eye], GL_RGBA8, 8, 8};
	}
	Require(glGetError() == GL_NO_ERROR, "Upload failure-path GL sources");
	draw.eye = 1;
	plan.draws.push_back(draw);
	auto backend =
		std::make_unique<OpenGLBackend>(app.device, app.context, device.Device(), device.EnabledExtensions());
	std::shared_ptr<OpenGLFrame> frame;
	Require(backend->Prepare(plan, sources, frame) == XR_SUCCESS, "Prepare context-failure GL frame");
	SwapchainAccess access{1};
	std::uint32_t image = 0;
	Require(access.Acquire(image) == XR_SUCCESS && access.Wait(0) == XR_SUCCESS && access.Release() == XR_SUCCESS,
			"Release failure-path GL source");
	s_applicationContext = app.context;
	s_rejectedContexts = 0;
	s_failRestoration = true;
	bool rejected = false;
	try
	{
		rejected = XR_FAILED(backend->Submit(frame));
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected && s_rejectedContexts >= 2, "Did not inject GL restoration and cleanup context failures");
	Require(frame->SourcesUsed(), "Failed GL submission lost queued source use");
	access.MarkReleasedUsed(frame);
	Require(access.Acquire(image) == XR_SUCCESS && access.Wait(0) == XR_TIMEOUT_EXPIRED,
			"Unconfirmed GL source reads allowed reuse");
	s_rejectContexts = false;
	s_rejectPrivateContext = true;
	Require(wglMakeCurrent(app.device, app.context), "Restore application after injected GL failure");
	glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
	app.texture = app.depthTexture = 0;
	Require(glGetError() == GL_NO_ERROR, "Delete source names while private GL reads are retained");
	const std::weak_ptr<OpenGLFrame> retained = frame;
	frame.reset();
	Require(!retained.expired() && access.Wait(0) == XR_TIMEOUT_EXPIRED,
			"Failed GL submission dropped source completion ownership");
	s_rejectPrivateContext = false;
	frame = retained.lock();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (frame->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	Require(frame->PollCompletion() == CompletionStatus::Complete && access.Wait(0) == XR_SUCCESS,
			"Restored GL completion did not retire source use");
	VulkanBackend delivery(device.Device(), 8, device.EnabledExtensions(), true);
	VulkanInspection inspection(OpenGLBackend::Identity(app.device, app.context), 8);
	std::shared_ptr<VulkanFrame> copied;
	Require(delivery.PrepareNativeCopy(frame->Image(), frame->Ready(), 0, frame, {}, copied) == XR_SUCCESS,
			"Prepare failed-submit GL output inspection");
	auto transfer = inspection.Prepare(copied);
	Require(delivery.Submit(copied) == XR_SUCCESS, "Copy completed failed-submit GL output");
	transfer->Commit();
	auto output = transfer->Output();
	CheckVulkan(delivery.Drain(5000000000ULL), "Drain failed-submit GL output");
	CheckVulkan(inspection.Drain(), "Drain failed-submit GL inspection");
	auto exported = output->Export();
	std::shared_ptr<const VulkanImagePixels> pixels;
	const auto exportDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!(pixels = exported->Pixels()) && std::chrono::steady_clock::now() < exportDeadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	Require(pixels && pixels->width == 8 && pixels->height == 8 && pixels->layers == 2,
			"Failed-submit GL output was not retained");
	for (std::size_t eye = 0; eye < 2; ++eye)
		for (std::size_t pixel = 0; pixel < 64; ++pixel)
		{
			const auto offset = (eye * 64 + pixel) * 4;
			Require(pixels->rgba[offset] == (eye == 0 ? 255 : 0) && pixels->rgba[offset + 1] == (eye == 1 ? 255 : 0) &&
						pixels->rgba[offset + 2] == 0 && pixels->rgba[offset + 3] == 255,
					"Deleted GL source storage changed retained output");
		}
	transfer.reset();
	copied.reset();
	frame.reset();
	backend.reset();
	Require(glGetError() == GL_NO_ERROR, "Retiring failed GL sources left an application GL error");
	std::puts(
		"PASS: injected WGL restoration/cleanup failures retain both deleted sources and gate reuse until completion");
#else
	std::puts("UNAVAILABLE: injected WGL restoration/cleanup failure requires the MinGW x64 linker wrapper");
#endif
}

int main()
{
	try
	{
		Application app;
		const auto identity = OpenGLBackend::Identity(app.device, app.context);
		VulkanInspectionDevice device(identity, {}, true);
		VulkanBackend delivery(device.Device(), 8, device.EnabledExtensions(), true);
		VulkanInspection inspection(identity, 8);
		auto backend = std::make_unique<OpenGLBackend>(app.device, app.context, device.Device(), device.EnabledExtensions());
		CompositionPlan plan{8, 8, {}, {}};
		CompositionDraw draw{};
		draw.constants.eyeTangents = draw.constants.sourceTangents = {-1, 1, 1, -1};
		draw.constants.sourceFromEyeRotation = {0, 0, 0, 1};
		draw.constants.imageRectangle = {0, 0, 1, 1};
		plan.draws.push_back(draw);
		draw.eye = 1;
		plan.draws.push_back(draw);
		std::array sources{OpenGLCompositionSource{app.texture, 0x8058, 8, 8},
			OpenGLCompositionSource{app.texture, 0x8058, 8, 8}};
		for (int scenario = 0; scenario < 16; ++scenario)
		{
			sources[0].texture = sources[1].texture = app.texture;
			const bool mipChain = scenario >= 8 && scenario < 13;
			const bool hdrRaw = scenario >= 13;
			const bool raw = scenario == 1 || mipChain || hdrRaw;
			const bool depthCase = scenario >= 2 && scenario < 8;
			const bool linear = depthCase && scenario % 2 != 0;
			std::shared_ptr<OpenGLFrame> frame;
			if (raw)
			{
				LayerImageDescription image{};
				image.rectangle = {{2, 1}, {4, 6}};
				auto source = sources.front();
				if (mipChain || hdrRaw)
				{
					if (!backend)
						backend = std::make_unique<OpenGLBackend>(app.device, app.context, device.Device(), device.EnabledExtensions());
					source.format = hdrRaw ? std::array<GLenum, 3>{GL_RGBA16F, GL_RGBA32F, GL_RGB10_A2}[scenario - 13] :
						std::array<GLenum, 5>{GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA16F, GL_RGB10_A2, GL_RGBA32F}[scenario - 8];
					source.mipLevels = hdrRaw ? 1 : 4;
					for (std::uint32_t mip = 0; mip < source.mipLevels; ++mip)
					{
						const auto size = 8u >> mip;
						std::vector<float> values(size * size * 4);
						for (std::size_t pixel = 0; pixel < size * size; ++pixel)
						{
							values[pixel * 4 + (mip == 3 ? 1 : 0)] = 0.5f;
							values[pixel * 4 + 3] = 1;
							if (hdrRaw)
							{
								values[pixel * 4] = -0.5f;
								values[pixel * 4 + 1] = 0.5f;
								values[pixel * 4 + 2] = 2.0f;
								values[pixel * 4 + 3] = 0.25f;
							}
						}
						glTexImage2D(GL_TEXTURE_2D, mip, source.format, size, size, 0, GL_RGBA, GL_FLOAT, values.data());
					}
					Require(glGetError() == GL_NO_ERROR, "Upload retained application mip chain");
				}
				Require(backend->PrepareCapture(image, source, frame, mipChain) == XR_SUCCESS, "Prepare raw OpenGL capture");
			}
			else if (depthCase)
			{
				const GLenum format = std::array<GLenum, 3>{GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT32F,
					GL_DEPTH24_STENCIL8}[(scenario - 2) / 2];
				std::array<float, 128> values{};
				std::array<std::uint32_t, 128> packed{};
				for (std::size_t index = 0; index < values.size(); ++index)
				{
					values[index] = index < 64 ? 0.25f : (index < 96 ? 0.5f : 0.75f);
					packed[index] = static_cast<std::uint32_t>(values[index] * 16777215.0f) << 8;
				}
				glBindTexture(GL_TEXTURE_2D_ARRAY, app.depthTexture);
				const bool packedDepth = format == GL_DEPTH24_STENCIL8;
				std::bit_cast<PFNGLTEXIMAGE3DPROC>(wglGetProcAddress("glTexImage3D"))(
					GL_TEXTURE_2D_ARRAY, 0, format, 8, 8, 2, 0,
					packedDepth ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
					packedDepth ? GL_UNSIGNED_INT_24_8 : GL_FLOAT,
					packedDepth ? static_cast<const void*>(packed.data()) : values.data());
				Require(glGetError() == GL_NO_ERROR, "Upload application depth array");
				LayerDepthDescription depth{};
				depth.image.arraySlice = 1;
				depth.image.rectangle = {{2, 1}, {4, 6}};
				depth.minDepth = 0;
				depth.maxDepth = 1;
				depth.nearZ = 0.1f;
				depth.farZ = 10;
				CompositionPlan depthPlan;
				Require(PrepareDepthComposition(depth, linear, {8, 8}, false, depthPlan) == XR_SUCCESS,
					"Prepare shared depth inspection plan");
				const OpenGLCompositionSource source{app.depthTexture, format, 8, 8, 2};
				Require(backend->Prepare(depthPlan, std::span{&source, 1}, frame, 1) == XR_SUCCESS,
					"Prepare OpenGL depth inspection");
			}
			else
				Require(backend->Prepare(plan, sources, frame) == XR_SUCCESS, "Prepare OpenGL composition");
			std::shared_ptr<VulkanFrame> copied;
			Require(delivery.PrepareNativeCopy(frame->Image(), frame->Ready(), 0, frame, {}, copied) == XR_SUCCESS,
				"Prepare OpenGL GPU delivery");
			auto transfer = inspection.Prepare(copied);
			Require(backend->Submit(frame) == XR_SUCCESS, "Submit OpenGL composition");
			GLint bound = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			Require(wglGetCurrentContext() == app.context && wglGetCurrentDC() == app.device &&
				static_cast<GLuint>(bound) == app.texture, "Application OpenGL context and bindings changed");
			Require(delivery.Submit(copied) == XR_SUCCESS, "Submit OpenGL GPU delivery");
			transfer->Commit();
			auto retained = transfer->Output();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (retained->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Require(retained->PollCompletion() == CompletionStatus::Complete, "OpenGL delivery did not finish");
			std::shared_ptr<const VulkanInspectionFrame> recomposed;
			if (mipChain)
			{
				Require(retained->Image().mipLevels == 4 && retained->Image().layers == 1 &&
					retained->Image().extent.width == 8, "Retained OpenGL image lost full mip chain");
				backend.reset();
				// Destroy application storage before independent GPU recomposition.
				glDeleteTextures(1, &app.texture);
				glGenTextures(1, &app.texture);
				glBindTexture(GL_TEXTURE_2D, app.texture);
				auto minified = plan;
				minified.width = minified.height = 1;
				const std::array<std::shared_ptr<const VulkanInspectionFrame>, 2> retainedSources{retained, retained};
				recomposed = VulkanInspectionFrame::Compose(minified, retainedSources);
				Require(recomposed != nullptr, "Recompose retained OpenGL mip chain after producer teardown");
				while (recomposed->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				Require(recomposed->PollCompletion() == CompletionStatus::Complete, "Retained OpenGL mip composition did not finish");
			}
			auto exported = (recomposed ? recomposed : retained)->Export();
			std::shared_ptr<const VulkanImagePixels> pixels;
			while (!(pixels = exported->Pixels()) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const std::uint32_t width = mipChain ? 1 : raw || depthCase ? 4 : 8,
				height = mipChain ? 1 : raw || depthCase ? 6 : 8, layers = mipChain ? 2 : raw || depthCase ? 1 : 2;
			Require(pixels && pixels->width == width && pixels->height == height && pixels->layers == layers, "OpenGL output shape mismatch");
			if (hdrRaw)
			{
				const auto format = std::array<std::uint32_t, 3>{97, 109, 64}[scenario - 13];
				const auto stride = ColorPixelBytes(format);
				Require(pixels->format == format && pixels->rgba.size() == width * height * stride, "OpenGL HDR raw format/size mismatch");
				const auto bytes = std::as_bytes(std::span(pixels->rgba));
				const std::array expected = scenario == 15 ? std::array{0.0f, 512.0f / 1023, 1.0f, 1.0f / 3} :
					std::array{-0.5f, 0.5f, 2.0f, 0.25f};
				for (std::size_t offset = 0; offset < bytes.size(); offset += stride)
				{
					const auto color = DecodeColorPixel(format, bytes.subspan(offset, stride));
					for (std::size_t channel = 0; channel < 4; ++channel)
						Require(std::abs(color[channel] - expected[channel]) <= (scenario == 15 && channel < 3 ? 1.0f / 1023 : 0.0001f),
							"OpenGL HDR range/alpha changed");
				}
			}
			for (std::size_t eye = 0; !hdrRaw && eye < layers; ++eye)
			{
				for (std::size_t y : {mipChain ? 0u : 1u, mipChain ? 0u : height - 2})
				{
					const auto offset = (eye * width * height + y * width + width / 2) * 4;
					if (mipChain)
					{
						const int expected = scenario == 9 ? 128 : 188;
						Require(pixels->rgba[offset] <= 2 && std::abs(pixels->rgba[offset + 1] - expected) <= 2 &&
							pixels->rgba[offset + 2] <= 2 && pixels->rgba[offset + 3] == 255,
							"Retained OpenGL mip selection or HDR/sRGB conversion mismatch");
						continue;
					}
					if (depthCase)
					{
						const double depth = y < height / 2 ? 0.75 : 0.5;
						const double value = linear ? 0.1 * depth / (10 * (1 - depth) + 0.1 * depth) : depth;
						const auto expected = static_cast<int>(std::lround(255 * (value <= 0.0031308 ? 12.92 * value :
							1.055 * std::pow(value, 1 / 2.4) - 0.055)));
						for (std::size_t channel = 0; channel < 3; ++channel)
							Require(std::abs(pixels->rgba[offset + channel] - expected) <= 2,
								"Depth inspection differs from normalized/linear conversion or selected array slice");
						Require(pixels->rgba[offset + 3] == 255, "Depth inspection alpha mismatch");
						continue;
					}
					Require(pixels->rgba[offset] == (y < height / 2 ? 255 : 0) && pixels->rgba[offset + 1] == 0 &&
						pixels->rgba[offset + 2] == (y < height / 2 ? 0 : 255), "OpenGL image orientation or color mismatch");
				}
			}
			CheckVulkan(delivery.Drain(5000000000ULL), "Drain OpenGL delivery");
			CheckVulkan(inspection.Drain(), "Drain OpenGL inspection");
		}
		VerifyMultisample(app, device);
		VerifySubmissionFailures(device);
		std::puts("PASS: canonical GLSL, exact float16/float32/RGB10 raw captures, cropped raw/depth arrays, normalized/linear depth, retained RGBA/sRGB/float16/float32/RGB10 mip chains after producer teardown and GPU handoff");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
