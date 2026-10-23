#include "graphics/ColorPixels.h"
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
		std::puts("PASS: canonical GLSL, exact float16/float32/RGB10 raw captures, cropped raw/depth arrays, normalized/linear depth, retained RGBA/sRGB/float16/float32/RGB10 mip chains after producer teardown and GPU handoff");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
