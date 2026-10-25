#include "graphics/ColorPixels.h"
#include "graphics/backends/d3d11/D3D11Backend.h"

#ifdef SIMXR_TEST_D3D11_VULKAN
#include "graphics/backends/d3d11/D3D11VulkanImport.h"
#include "graphics/backends/d3d12/D3D12VulkanImport.h"
#include "graphics/backends/windows/WindowsSessionGraphics.h"
#include "graphics/backends/windows/WindowsGraphicsCapabilities.h"
#include "runtime/Session.h"
#include "runtime/Instance.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "../../probes/VulkanProbeInstance.h"
#include <chrono>
#include <future>
#include <thread>
#endif
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <stdexcept>

using namespace simxr;
using Microsoft::WRL::ComPtr;

struct ColorSourceCase
{
	UINT samples;
	DXGI_FORMAT resource;
	DXGI_FORMAT view;
};
constexpr ColorSourceCase ColorSources[]{
	{1, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM},
	{4, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM},
	{1, DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT},
	{1, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT},
	{1, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT},
	{4, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT},
	{1, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM},
	{4, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}};

void Check(HRESULT result)
{
	if (FAILED(result))
		throw std::runtime_error("Direct3D failure: " + std::to_string(result));
}

static ComPtr<IDXGIAdapter1> TestAdapter()
{
	ComPtr<IDXGIAdapter1> adapter;
#ifdef SIMXR_TEST_D3D11_VULKAN
	LUID luid{};
	if (XR_FAILED(windows::Direct3DAdapterLuid(luid)))
		throw std::runtime_error("No shared Direct3D/Vulkan adapter");
	ComPtr<IDXGIFactory1> factory;
	Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
	for (UINT index = 0;; ++index)
	{
		Check(factory->EnumAdapters1(index, &adapter));
		DXGI_ADAPTER_DESC1 description{};
		Check(adapter->GetDesc1(&description));
		if (!std::memcmp(&description.AdapterLuid, &luid, sizeof(luid)))
			break;
		adapter.Reset();
	}
#else
	ComPtr<IDXGIFactory1> factory;
	Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
	Check(factory->EnumAdapters1(0, &adapter));
#endif
	return adapter;
}

#ifdef SIMXR_TEST_D3D11_VULKAN
template <typename Frame>
void VerifyVulkan(const std::shared_ptr<Frame>& source, bool mipChain = false, VkExtent2D expected = {8, 8},
				  std::uint32_t layers = 2, int grayscale = -1)
{
	probe::Instance instance;
	instance.Create();
	std::uint32_t count = 0;
	CheckVulkan(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "Enumerate import devices");
	std::vector<VkPhysicalDevice> physical(count);
	CheckVulkan(vkEnumeratePhysicalDevices(instance.handle, &count, physical.data()), "Enumerate import devices");
	if (physical.empty())
		throw std::runtime_error("No Vulkan device for D3D11 import");
	const auto adapterLuid = [&]()
	{
		if constexpr (std::is_same_v<Frame, D3D11Frame>)
		{
			ComPtr<ID3D11Device> device;
			source->Texture()->GetDevice(&device);
			ComPtr<IDXGIDevice> dxgi;
			ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC description{};
			Check(device.As(&dxgi));
			Check(dxgi->GetAdapter(&adapter));
			Check(adapter->GetDesc(&description));
			return description.AdapterLuid;
		}
		else
		{
			ComPtr<ID3D12Device> device;
			Check(source->Texture()->GetDevice(IID_PPV_ARGS(&device)));
			return device->GetAdapterLuid();
		}
	}();
	const auto matching = std::find_if(
		physical.begin(), physical.end(),
		[&](VkPhysicalDevice candidate)
		{
			VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
			VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
			vkGetPhysicalDeviceProperties2(candidate, &properties);
			return identity.deviceLUIDValid && !std::memcmp(identity.deviceLUID, &adapterLuid, VK_LUID_SIZE);
		});
	if (matching == physical.end())
		throw std::runtime_error("No Vulkan device matches the Direct3D source adapter");
	const auto importDevice = *matching;
	vkGetPhysicalDeviceQueueFamilyProperties(importDevice, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(importDevice, &count, families.data());
	std::uint32_t family = 0;
	while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		++family;
	if (family == count)
		throw std::runtime_error("No Vulkan graphics queue");
	const float priority = 1;
	VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue.queueFamilyIndex = family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;
	const std::array extensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension,
								VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
	vkGetPhysicalDeviceFeatures2(importDevice, &features);
	if (!timeline.timelineSemaphore)
		throw std::runtime_error("Native compositor import requires timeline semaphores");
	VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &timeline};
	create.queueCreateInfoCount = 1;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = extensions.size();
	create.ppEnabledExtensionNames = extensions.data();
	struct Native
	{
		VkDevice device = VK_NULL_HANDLE;
		~Native()
		{
			if (device)
			{
				vkDeviceWaitIdle(device);
				vkDestroyDevice(device, nullptr);
			}
		}
	} native;
	CheckVulkan(vkCreateDevice(importDevice, &create, nullptr, &native.device), "Create import device");
	VulkanBackend backend({instance.handle, importDevice, native.device, family, 0}, 8, extensions, VK_API_VERSION_1_1);
	auto imported = [&]()
	{
		if constexpr (std::is_same_v<Frame, D3D11Frame>)
			return ImportD3D11Frame(backend.Device(), source);
		else
			return ImportD3D12Frame(backend.Device(), source);
	}();
	std::shared_ptr<VulkanFrame> copied;
	if (XR_FAILED(backend.PrepareNativeCopy(imported->Image(), imported->Ready(), D3D11Frame::ReadyValue,
		imported, source->Views(), copied)))
		throw std::runtime_error("Prepare D3D11 compositor copy failed");
	VulkanInspection inspection(copied->ExternalImage()->Descriptor().identity);
	auto transfer = inspection.Prepare(copied);
	if (XR_FAILED(backend.Submit(copied)))
		throw std::runtime_error("Submit D3D11 compositor copy failed");
	transfer->Commit();
	auto retained = transfer->Output();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (retained->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	if (retained->PollCompletion() != CompletionStatus::Complete)
		throw std::runtime_error("D3D11 Vulkan inspection did not complete");
	std::shared_ptr<const VulkanInspectionFrame> downsampled;
	if (mipChain)
	{
		if (copied->Image().mipLevels != 4 || copied->Image().layers != 1)
			throw std::runtime_error("Retained D3D11 copy lost mip chain or array selection");
		const auto format = [&]()
		{
			if constexpr (std::is_same_v<Frame, D3D11Frame>)
				return source->Format();
			else
				return source->Texture()->GetDesc().Format;
		}();
		if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
			format == DXGI_FORMAT_R32G32B32A32_FLOAT)
		{
			auto raw = retained->Export();
			std::shared_ptr<const VulkanImagePixels> pixels;
			while (!(pixels = raw->Pixels()) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (!pixels || pixels->rgba.size() != 8 * 8 * ColorPixelBytes(pixels->format))
				throw std::runtime_error("HDR mip export lost its format-dependent byte size");
		}

		CompositionPlan plan{1, 1, {}, {}};
		CompositionDraw draw{};
		draw.constants.eyeTangents = draw.constants.sourceTangents = {-1, 1, 1, -1};
		draw.constants.sourceFromEyeRotation = {0, 0, 0, 1};
		draw.constants.imageRectangle = {0, 0, 1, 1};
		plan.draws.push_back(draw);
		draw.eye = 1;
		plan.draws.push_back(draw);
		const std::array<std::shared_ptr<const VulkanInspectionFrame>, 2> sources{retained, retained};
		downsampled = VulkanInspectionFrame::Compose(plan, sources);
		if (!downsampled)
			throw std::runtime_error("Retained D3D11 mip composition unavailable");
		while (downsampled->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (downsampled->PollCompletion() != CompletionStatus::Complete)
			throw std::runtime_error("Retained D3D11 mip composition did not complete");
	}
	auto readback = (downsampled ? downsampled : retained)->Export();
	std::shared_ptr<const VulkanImagePixels> pixels;
	while (!(pixels = readback->Pixels()) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	if (mipChain)
		expected = {1, 1};
	if (!pixels || pixels->layers != layers || pixels->width != expected.width || pixels->height != expected.height)
		throw std::runtime_error("D3D11 Vulkan export shape mismatch");
	for (std::size_t eye = 0; eye < layers; ++eye)
	{
		const auto offset = (eye * expected.width * expected.height +
			expected.height / 2 * expected.width + expected.width / 2) * 4;
		if (grayscale >= 0)
		{
			for (std::size_t channel = 0; channel < 3; ++channel)
				if (std::abs(static_cast<int>(pixels->rgba[offset + channel]) - grayscale) > 1)
					throw std::runtime_error("Native depth normalization pixels mismatch");
			continue;
		}
		const auto color = DecodeColorPixel(pixels->format,
			std::as_bytes(std::span(pixels->rgba)).subspan(offset / 4 * ColorPixelBytes(pixels->format)));
		if (color[0] != (!mipChain && layers == 2 && eye == 0 ? 1.0f : 0.0f) ||
			color[1] != (!mipChain && (layers == 1 || eye == 1) ? 1.0f : 0.0f) ||
			color[2] != (mipChain ? 1.0f : 0.0f))
			throw std::runtime_error("D3D11 Vulkan exported pixels mismatch");
	}
	CheckVulkan(backend.Drain(5000000000ULL), "Drain D3D11 import queue");
	CheckVulkan(inspection.Drain(), "Drain D3D11 inspection queue");
	readback.reset();
	downsampled.reset();
	retained.reset();
	transfer.reset();
	copied.reset();
	imported.reset();
	CheckVulkan(inspection.Close(), "Close D3D11 inspection");
	if (XR_FAILED(backend.Close()))
		throw std::runtime_error("Close D3D11 import backend failed");
	std::puts("PASS: native GPU fence -> Vulkan copy -> retained inspection pixels");
}

void VerifySessionClose(ID3D11Device* device, ID3D11Texture2D* texture)
{
	const auto require = [](bool success, const char* message)
	{
		if (!success)
			throw std::runtime_error(message);
	};
	Instance instance(reinterpret_cast<XrInstance>(1), {}, XR_MAKE_VERSION(1, 0, 27));
	auto session = std::make_shared<Session>(reinterpret_cast<XrSession>(2), instance);
	WindowsSessionGraphics graphics(device, session);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
	begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	require(session->Ready(timing.epoch) == XR_SUCCESS &&
				session->Begin(begin, timing, PresentationPolicy::Repeat) == XR_SUCCESS,
			"Start native close-overlap session");
	XrSpace space;
	require(session->Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {}}, space) ==
				XR_SUCCESS,
			"Create native close-overlap space");
	FramePrediction prediction;
	require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
			"Begin native close-overlap frame");
	ViewSettings settings;
	settings.outerWidth = settings.outerHeight = 8;
	session->PublishTracking(prediction.displayTime, {}, settings, prediction.id);
	const auto swapchain = reinterpret_cast<XrSwapchain>(3);
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	quad.space = space;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.subImage = {swapchain, {{0, 0}, {8, 8}}, 0};
	quad.pose = {{0, 0, 0, 1}, {0, 1.7f, -1}};
	quad.size = {1, 1};
	const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
	const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
							 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		&layer};
	std::promise<void> entered, resume;
	auto reached = entered.get_future();
	auto resumed = resume.get_future().share();
	bool gated = false;
	auto submission = std::async(std::launch::async,
								 [&]
								 {
									 return graphics.EndFrame(
										 end,
										 [&](XrSwapchain handle) -> std::optional<SwapchainDescription>
										 {
											 return handle == swapchain
														? std::optional{SwapchainDescription{8, 8, 2, true}}
														: std::nullopt;
										 },
										 [&](XrSwapchain)
										 {
											 if (!gated)
											 {
												 gated = true;
												 entered.set_value();
												 resumed.wait();
											 }
											 return D3D11CompositionSource{texture, DXGI_FORMAT_R8G8B8A8_UNORM};
										 });
								 });
	const bool preparing = reached.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
	if (!preparing)
	{
		resume.set_value();
		submission.get();
		require(false, "Native preparation did not reach close-overlap gate");
	}
	session->Close();
	std::promise<void> closing;
	auto started = closing.get_future();
	auto close = std::async(std::launch::async,
							[&]
							{
								closing.set_value();
								return graphics.Close();
							});
	started.wait();
	const bool held = close.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
	resume.set_value();
	const auto submitted = submission.get();
	const auto closed = close.get();
	require(held, "Native close overtook an active frame preparation");
	require(submitted == XR_ERROR_SESSION_NOT_RUNNING && closed == XR_SUCCESS && graphics.Close() == XR_SUCCESS,
			"Native close-overlap did not cancel preparation and preserve close retry");
	std::puts("PASS: native close waits for frame preparation before draining");
}

void VerifySessionCaptures(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT depthFormat)
{
	const auto require = [](bool success, const char* message)
	{
		if (!success)
			throw std::runtime_error(message);
	};
	const char* extensions[]{XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
	Instance instance(reinterpret_cast<XrInstance>(1), extensions, XR_MAKE_VERSION(1, 0, 27));
	auto session = std::make_shared<Session>(reinterpret_cast<XrSession>(2), instance);
	WindowsSessionGraphics graphics(device, session);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
	begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing, depthFormat == DXGI_FORMAT_UNKNOWN ?
		PresentationPolicy::Rotational : PresentationPolicy::Repeat) == XR_SUCCESS,
		"Start filtered capture session");
	XrSpace space;
	require(session->Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {}}, space) == XR_SUCCESS,
		"Create capture space");
	session->Scheduler().Pause();
	session->Display().Pause();
	const auto swapchain = reinterpret_cast<XrSwapchain>(3);
	const bool depthOnly = depthFormat != DXGI_FORMAT_UNKNOWN;
	if (!depthOnly)
		depthFormat = DXGI_FORMAT_D32_FLOAT;
	const auto depthSwapchain = reinterpret_cast<XrSwapchain>(4);
	D3D11_TEXTURE2D_DESC depthDescription{};
	texture->GetDesc(&depthDescription);
	depthDescription.Format = depthFormat;
	depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ComPtr<ID3D11Texture2D> depthTexture;
	Check(device->CreateTexture2D(&depthDescription, nullptr, &depthTexture));
	ComPtr<ID3D11DeviceContext> context;
	device->GetImmediateContext(&context);
	for (UINT eye = 0; eye < 2; ++eye)
	{
		D3D11_DEPTH_STENCIL_VIEW_DESC view{};
		view.Format = depthFormat;
		view.ViewDimension = depthDescription.SampleDesc.Count > 1
			? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		if (depthDescription.SampleDesc.Count > 1)
		{
			view.Texture2DMSArray.FirstArraySlice = eye;
			view.Texture2DMSArray.ArraySize = 1;
		}
		else
		{
			view.Texture2DArray.FirstArraySlice = eye;
			view.Texture2DArray.ArraySize = 1;
		}
		ComPtr<ID3D11DepthStencilView> target;
		Check(device->CreateDepthStencilView(depthTexture.Get(), &view, &target));
		context->ClearDepthStencilView(target.Get(), D3D11_CLEAR_DEPTH, eye ? 0.75f : 0.25f, 0);
	}
	std::vector<std::shared_ptr<const VulkanInspectionFrame>> retained;
	FrameKey lastFrame;
	const auto targets = depthOnly
		? std::vector<CaptureTarget>{{CaptureKind::LayerDepth, 0, 1}, {CaptureKind::LayerDepthLinear, 0, 1}}
		: std::vector<CaptureTarget>{{CaptureKind::ProjectionLayers}, {CaptureKind::QuadLayers}, {CaptureKind::LayerView, 1, 0}};
	for (const auto target : targets)
	{
		const auto capture = session->ReserveCapture(target);
		require(capture.result == SnapshotAdmission::Accepted && session->Scheduler().GrantPermits() == XR_SUCCESS,
			"Reserve exact filtered capture");
		FramePrediction prediction;
		require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS, "Begin filtered frame");
		ViewSettings settings;
		settings.outerWidth = settings.outerHeight = 8;
		session->PublishTracking(prediction.displayTime, {}, settings, prediction.id);
		lastFrame = {session->Snapshot().generation, prediction.id};
		const auto views = session->ViewsAt(prediction.displayTime);
		std::array<XrCompositionLayerProjectionView, 2> projectionViews;
		std::array<XrCompositionLayerDepthInfoKHR, 2> depthViews;
		for (std::uint32_t eye = 0; eye < 2; ++eye)
		{
			projectionViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			depthViews[eye] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
			depthViews[eye].subImage = {depthSwapchain, {{1, 2}, {3, 4}}, eye};
			depthViews[eye].minDepth = 0;
			depthViews[eye].maxDepth = 1;
			depthViews[eye].nearZ = 0.1f;
			depthViews[eye].farZ = 10;
			projectionViews[eye].next = &depthViews[eye];
			projectionViews[eye].pose = views.views[eye].pose.pose;
			projectionViews[eye].fov = views.views[eye].fov;
			projectionViews[eye].subImage = {swapchain, {{0, 0}, {8, 8}}, eye};
		}
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		projection.space = space;
		projection.viewCount = 2;
		projection.views = projectionViews.data();
		XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
		quad.space = space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage = {swapchain, {{2, 1}, {4, 3}}, 1};
		quad.pose = {{0, 0, 0, 1}, {0, 1.7f, -1}};
		quad.size = {1, 1};
		const std::array layers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
			reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, prediction.displayTime.value.count(),
			XR_ENVIRONMENT_BLEND_MODE_OPAQUE, static_cast<std::uint32_t>(layers.size()), layers.data()};
		std::array<std::pair<XrSwapchain, std::shared_ptr<const GpuCompletion>>, 64> sourceUses;
		std::size_t sourceUseCount = 0;
		const auto submitted = graphics.EndFrame(
			end,
			[&](XrSwapchain handle) -> std::optional<SwapchainDescription>
			{
				return handle == swapchain || handle == depthSwapchain
						   ? std::optional{SwapchainDescription{8, 8, 2, true}}
						   : std::nullopt;
			},
			[&](XrSwapchain handle)
			{
				return handle == depthSwapchain ? D3D11CompositionSource{depthTexture.Get(), depthFormat}
												: D3D11CompositionSource{texture, DXGI_FORMAT_R8G8B8A8_UNORM};
			},
			[&](XrSwapchain source, const std::shared_ptr<const GpuCompletion>& completion)
			{
				if (sourceUseCount < sourceUses.size())
					sourceUses[sourceUseCount] = {source, completion};
				++sourceUseCount;
			});
		if (XR_FAILED(submitted))
			throw std::runtime_error("Submit filtered D3D11 capture: " + std::to_string(submitted));
		require(sourceUseCount && sourceUseCount <= sourceUses.size(), "Native source-use notifications missing");
		bool usedDepth = false;
		for (std::size_t index = 0; index < sourceUseCount; ++index)
		{
			const auto& [source, completion] = sourceUses[index];
			const auto native = std::dynamic_pointer_cast<const D3D11Frame>(completion);
			require((source == swapchain || source == depthSwapchain) && native && native->SourcesUsed(),
					"Native completion did not identify an executed source read");
			usedDepth |= source == depthSwapchain;
		}
		require(usedDepth == depthOnly, "Native source-use notification included an unrelated depth source");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		std::optional<SnapshotRecord> record;
		do
		{
			session->Captures().Poll();
			record = session->Captures().Read(capture.id);
			if (record && record->phase == SnapshotPhase::Ready)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		require(record && record->phase == SnapshotPhase::Ready && record->target == target,
			"Filtered D3D11 capture did not complete with its requested identity");
		retained.push_back(std::dynamic_pointer_cast<const VulkanInspectionFrame>(record->output));
		require(retained.back() != nullptr && session->Captures().Release(capture.id), "Retain and release filtered capture");
	}
	if (!depthOnly)
	{
		const auto history = session->ImageHistory().Find(lastFrame, ImageHistoryView::Quads);
		auto quad = std::dynamic_pointer_cast<const VulkanInspectionFrame>(history.output);
		require(quad != nullptr && history.quadVisible, "D3D11 live quad history is unavailable");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (quad->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(quad->PollCompletion() == CompletionStatus::Complete, "D3D11 quad history did not complete");
		retained.push_back(std::move(quad));
		const auto submissions = session->Display().Retain();
		const auto found = std::find_if(submissions.begin(), submissions.end(),
			[&](const auto& submission) { return submission.key == lastFrame; });
		require(found != submissions.end() && found->reprojection.source && found->reprojection.status == XR_SUCCESS,
			"D3D11 session did not retain rotational layer inputs");
		const auto source = found->reprojection.source;
		while (source->PollCompletion() == CompletionStatus::Pending && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(source->PollCompletion() == CompletionStatus::Complete, "D3D11 rotational inputs did not complete");
		auto head = source->SubmittedHead();
		head.pose.orientation = {0, std::sin(0.25f), 0, std::cos(0.25f)};
		PresentationTick baseline;
		baseline.source = lastFrame;
		baseline.output = found->output;
		baseline.reprojection = found->reprojection;
		RotationalDisplay display;
		PresentationImage image;
		do
		{
			image = display.Process(baseline, head);
			if (image.reprojected)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		require(image.reprojected && image.result == XR_SUCCESS && image.output != baseline.output,
			"D3D11 display did not reproject while application rendering was stopped");
		auto original = std::dynamic_pointer_cast<const VulkanInspectionFrame>(baseline.output)->Export();
		auto rotated = std::dynamic_pointer_cast<const VulkanInspectionFrame>(image.output)->Export();
		std::shared_ptr<const VulkanImagePixels> originalPixels, rotatedPixels;
		while ((!(originalPixels = original->Pixels()) || !(rotatedPixels = rotated->Pixels())) &&
			std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(originalPixels && rotatedPixels && originalPixels->rgba != rotatedPixels->rgba &&
			rotatedPixels->width == 8 && rotatedPixels->height == 8 && rotatedPixels->layers == 2,
			"Rotational display did not change retained D3D11 pixels");
		std::puts("PASS: D3D11 retained projection/quad layers reproject without another application frame");
	}
	session->Close();
	require(graphics.Close() == XR_SUCCESS, "Close filtered capture session");
	for (std::size_t capture = 0; capture < retained.size(); ++capture)
	{
		auto readback = retained[capture]->Export();
		std::shared_ptr<const VulkanImagePixels> pixels;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!(pixels = readback->Pixels()) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (depthOnly)
		{
			require(pixels && pixels->width == 3 && pixels->height == 4 && pixels->layers == 1,
				"Depth capture lost crop dimensions or selected slice");
			const double value = capture == 0 ? 0.75 : 0.1 * 0.75 / (10 * 0.25 + 0.1 * 0.75);
			const auto expected = static_cast<int>(std::lround(255 * (value <= 0.0031308 ? 12.92 * value
				: 1.055 * std::pow(value, 1 / 2.4) - 0.055)));
			for (std::size_t offset = 0; offset < pixels->rgba.size(); offset += 4)
				require(std::abs(pixels->rgba[offset] - expected) <= 2 &&
					std::abs(pixels->rgba[offset + 1] - expected) <= 2 && std::abs(pixels->rgba[offset + 2] - expected) <= 2 &&
					pixels->rgba[offset + 3] == 255, "Depth capture differs from shared normalized/linear conversion");
			continue;
		}
		if (capture == 2)
		{
			require(pixels && pixels->width == 4 && pixels->height == 3 && pixels->layers == 1 &&
				pixels->format == VK_FORMAT_R8G8B8A8_UNORM, "Raw capture lost crop dimensions, slice or format");
			for (std::size_t offset = 0; offset < pixels->rgba.size(); offset += 4)
				require(pixels->rgba[offset] == 0 && pixels->rgba[offset + 1] == 255 &&
					pixels->rgba[offset + 2] == 0 && pixels->rgba[offset + 3] == 64, "Raw capture lost selected slice pixels or alpha");
			continue;
		}
		require(pixels && pixels->width == 8 && pixels->height == 8 && pixels->layers == 2, "Retained filtered dimensions");
		for (std::size_t eye = 0; eye < 2; ++eye)
			for (const std::size_t position : {0U, 36U})
			{
				const bool projection = capture == 0;
				const bool green = projection ? eye == 1 : position == 36;
				const auto offset = (eye * 64 + position) * 4;
				require(pixels->rgba[offset] == (projection && eye == 0 ? 255 : 0) &&
					pixels->rgba[offset + 1] == (green ? 255 : 0) && pixels->rgba[offset + 2] == 0,
					"Filtered D3D11 pixels contain another layer or lost retained content");
			}
	}
	std::puts("PASS: D3D11 exact projection/quad/raw/depth captures retain isolated pixels and crop metadata after session shutdown");
}

#endif

#ifdef SIMXR_TEST_D3D11_VULKAN
void VerifyD3D12()
{
	ComPtr<ID3D12Device> device;
	Check(D3D12CreateDevice(TestAdapter().Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
	D3D12_COMMAND_QUEUE_DESC queueDescription{};
	queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue;
	Check(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)));
	auto backend = std::make_unique<D3D12Backend>(device.Get(), queue.Get());
	std::shared_ptr<D3D12Frame> retained;
	for (const auto [samples, resourceFormat, viewFormat] : ColorSources)
	{
		D3D12_RESOURCE_DESC description{};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = description.Height = 8;
		description.DepthOrArraySize = 2;
		description.MipLevels = 1;
		description.Format = resourceFormat;
		description.SampleDesc.Count = samples;
		description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		ComPtr<ID3D12Resource> source;
		Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&source)));
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> commands;
		Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
		Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
		D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
		descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descriptors.NumDescriptors = 2;
		ComPtr<ID3D12DescriptorHeap> targets;
		Check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&targets)));
		CompositionPlan plan{8, 8, {}, {}};
		for (UINT eye = 0; eye < 2; ++eye)
		{
			D3D12_RENDER_TARGET_VIEW_DESC view{};
			view.Format = viewFormat;
			view.ViewDimension = samples == 1 ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
			if (samples == 1)
				view.Texture2DArray = {0, eye, 1, 0};
			else
				view.Texture2DMSArray = {eye, 1};
			auto target = targets->GetCPUDescriptorHandleForHeapStart();
			target.ptr += eye * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			device->CreateRenderTargetView(source.Get(), &view, target);
			const float color[]{eye == 0 ? 1.0f : 0.0f, eye == 1 ? 1.0f : 0.0f, 0, 1};
			commands->ClearRenderTargetView(target, color, 0, nullptr);
			CompositionDraw draw{};
			draw.eye = eye;
			draw.source.arraySlice = eye;
			draw.constants.eyeTangents = draw.constants.sourceTangents = {-1, 1, 1, -1};
			draw.constants.sourceFromEyeRotation = {0, 0, 0, 1};
			draw.constants.imageRectangle = {0, 0, 1, 1};
			plan.draws.push_back(draw);
		}
		Check(commands->Close());
		ID3D12CommandList* lists[]{commands.Get()};
		queue->ExecuteCommandLists(1, lists);
		const std::array sources{D3D12CompositionSource{source.Get(), viewFormat}, D3D12CompositionSource{source.Get(), viewFormat}};
		std::shared_ptr<D3D12Frame> frame;
		Check(backend->Prepare(plan, sources, frame));
		Check(backend->Submit(frame));
		VerifyVulkan(frame);
		// Reuse the same application resources in their release state on the next frame.
		std::shared_ptr<D3D12Frame> repeated;
		Check(backend->Prepare(plan, sources, repeated));
		Check(backend->Submit(repeated));
		VerifyVulkan(repeated);
		LayerImageDescription image{};
		image.arraySlice = 1;
		image.rectangle = {{2, 1}, {4, 3}};
		std::shared_ptr<D3D12Frame> capture;
		Check(backend->PrepareCapture(image, sources.front(), capture));
		Check(backend->Submit(capture));
		VerifyVulkan(capture, false, {4, 3}, 1);
		retained = std::move(capture);
	}
	for (const auto format : {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
		DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
	{
		for (UINT samples : {1u, 4u})
		{
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = description.Height = 8;
			description.DepthOrArraySize = 2;
			description.MipLevels = 1;
			description.Format = format;
			description.SampleDesc.Count = samples;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			ComPtr<ID3D12Resource> source;
			Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&source)));
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12GraphicsCommandList> commands;
			Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
			Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
			D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
			descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			descriptors.NumDescriptors = 1;
			ComPtr<ID3D12DescriptorHeap> targets;
			Check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&targets)));
			D3D12_DEPTH_STENCIL_VIEW_DESC view{};
			view.Format = format;
			view.ViewDimension = samples == 1 ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
			if (samples == 1)
				view.Texture2DArray = {0, 1, 1};
			else
				view.Texture2DMSArray = {1, 1};
			const auto target = targets->GetCPUDescriptorHandleForHeapStart();
			device->CreateDepthStencilView(source.Get(), &view, target);
			commands->ClearDepthStencilView(target, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, nullptr);
			Check(commands->Close());
			ID3D12CommandList* lists[]{commands.Get()};
			queue->ExecuteCommandLists(1, lists);
			LayerDepthDescription depth{};
			depth.image.arraySlice = 1;
			depth.image.rectangle = {{2, 1}, {4, 3}};
			depth.minDepth = 0;
			depth.maxDepth = 1;
			depth.nearZ = 0.1f;
			depth.farZ = 100;
			const D3D12CompositionSource input{source.Get(), format, D3D12_RESOURCE_STATE_DEPTH_WRITE};
			for (bool linear : {false, true})
			{
				CompositionPlan plan;
				if (PrepareDepthComposition(depth, linear, {8, 8}, samples > 1, plan) != XR_SUCCESS)
					throw std::runtime_error("Cannot plan depth inspection");
				std::shared_ptr<D3D12Frame> frame;
				Check(backend->Prepare(plan, std::span{&input, 1}, frame, 1));
				Check(backend->Submit(frame));
				VerifyVulkan(frame, false, {4, 3}, 1, linear ? 10 : 225);
			}
		}
	}
	std::vector<std::shared_ptr<D3D12Frame>> retainedMips;
	for (const auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
		DXGI_FORMAT_R32G32B32A32_FLOAT})
	{
		D3D12_RESOURCE_DESC description{};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = description.Height = 8;
		description.DepthOrArraySize = 2;
		description.MipLevels = 4;
		description.Format = format;
		description.SampleDesc.Count = 1;
		description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		ComPtr<ID3D12Resource> texture;
		Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture)));
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> commands;
		Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
		Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
		D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
		descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descriptors.NumDescriptors = 4;
		ComPtr<ID3D12DescriptorHeap> targets;
		Check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&targets)));
		for (UINT mip = 0; mip < 4; ++mip)
		{
			D3D12_RENDER_TARGET_VIEW_DESC view{};
			view.Format = format;
			view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			view.Texture2DArray = {mip, 1, 1, 0};
			auto target = targets->GetCPUDescriptorHandleForHeapStart();
			target.ptr += mip * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			device->CreateRenderTargetView(texture.Get(), &view, target);
			const float color[]{mip == 3 ? 0.0f : 1.0f, 0, mip == 3 ? 1.0f : 0.0f, 1};
			commands->ClearRenderTargetView(target, color, 0, nullptr);
		}
		Check(commands->Close());
		ID3D12CommandList* lists[]{commands.Get()};
		queue->ExecuteCommandLists(1, lists);
		LayerImageDescription image{};
		image.arraySlice = 1;
		image.rectangle = {{2, 1}, {4, 3}};
		std::shared_ptr<D3D12Frame> capture;
		Check(backend->PrepareCapture(image, {texture.Get(), format}, capture, true));
		Check(backend->Submit(capture));
		VerifyVulkan(capture, true);
		retainedMips.push_back(std::move(capture));
	}
	backend.reset();
	for (const auto& mipChain : retainedMips)
		VerifyVulkan(mipChain, true);
	std::puts("PASS: retained D3D12 float16/float32/RGB10 mip chains compose after producer teardown");
	VerifyVulkan(retained, false, {4, 3}, 1);
	std::puts("PASS: retained D3D12 pixels survive native producer teardown");
	std::puts("PASS: D3D12 normalized/linear depth, four depth formats, array selection and MSAA sample zero");
	std::puts("PASS: D3D12 canonical stereo shader, MSAA resolve, shared resource/fence and repeated source use");
}
#endif

int main(int argc, char**)
{
	try
	{
#ifdef SIMXR_TEST_D3D11_VULKAN
		if (argc > 1)
		{
			VerifyD3D12();
			return 0;
		}
#endif
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		Check(D3D11CreateDevice(TestAdapter().Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
			D3D11_SDK_VERSION, &device, nullptr, &context));
		D3D11Backend backend(device.Get());
		for (const auto [samples, resourceFormat, viewFormat] : ColorSources)
		{
			D3D11_TEXTURE2D_DESC description{};
			description.Width = description.Height = 8;
			description.MipLevels = 1;
			description.ArraySize = 2;
			description.Format = resourceFormat;
			description.SampleDesc.Count = samples;
			// Swapchains need not permit shader views. Exercise two array slices and
			// multisample resolve through the exact native input used by the runtime.
			description.BindFlags = D3D11_BIND_RENDER_TARGET;
			ComPtr<ID3D11Texture2D> source;
			Check(device->CreateTexture2D(&description, nullptr, &source));
			for (UINT eye = 0; eye < 2; ++eye)
			{
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				view.Format = viewFormat;
				view.ViewDimension = samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				if (samples > 1)
				{
					view.Texture2DMSArray.FirstArraySlice = eye;
					view.Texture2DMSArray.ArraySize = 1;
				}
				else
				{
					view.Texture2DArray.FirstArraySlice = eye;
					view.Texture2DArray.ArraySize = 1;
				}
				ComPtr<ID3D11RenderTargetView> target;
				Check(device->CreateRenderTargetView(source.Get(), &view, &target));
				const float color[]{eye == 0 ? 1.0f : 0.0f, eye == 1 ? 1.0f : 0.0f, 0, 0.25f};
				context->ClearRenderTargetView(target.Get(), color);
			}
			description.SampleDesc.Count = 1;
			description.ArraySize = 2;
			description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
			description.BindFlags = D3D11_BIND_RENDER_TARGET;
			CompositionPlan plan{8, 8, {}, {}};
			CompositionDraw draw{};
			draw.constants.eyeTangents = draw.constants.sourceTangents = {-1, 1, 1, -1};
			draw.constants.sourceFromEyeRotation = {0, 0, 0, 1};
			draw.constants.imageRectangle = {0, 0, 1, 1};
			plan.draws.push_back(draw);
			draw.eye = 1;
			draw.source.arraySlice = 1;
			plan.draws.push_back(draw);
			const std::array sources{D3D11CompositionSource{source.Get(), viewFormat},
				D3D11CompositionSource{source.Get(), viewFormat}};
			// Verify that executing the private list preserves the application's state.
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
			std::shared_ptr<D3D11Frame> frame;
			Check(backend.Prepare(plan, sources, frame));
			Check(backend.Submit(frame));
			D3D11_PRIMITIVE_TOPOLOGY topology;
			context->IAGetPrimitiveTopology(&topology);
			if (topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST)
				throw std::runtime_error("Application topology changed");
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Texture2D> staging;
			ComPtr<ID3D11Device> consumer;
			ComPtr<ID3D11DeviceContext> consumerContext;
			Check(D3D11CreateDevice(TestAdapter().Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
				D3D11_SDK_VERSION, &consumer, nullptr, &consumerContext));
			ComPtr<ID3D11Device5> consumerDevice;
			ComPtr<ID3D11DeviceContext4> consumerQueue;
			Check(consumer.As(&consumerDevice));
			Check(consumerContext.As(&consumerQueue));
			ComPtr<ID3D11Texture2D> imported;
			ComPtr<ID3D11Fence> ready;
			Check(consumerDevice->OpenSharedResource1(frame->TextureHandle(), IID_PPV_ARGS(&imported)));
			Check(consumerDevice->OpenSharedFence(frame->FenceHandle(), IID_PPV_ARGS(&ready)));
			Check(consumerQueue->Wait(ready.Get(), D3D11Frame::ReadyValue));
			Check(consumer->CreateTexture2D(&description, nullptr, &staging));
			consumerContext->CopyResource(staging.Get(), imported.Get());
			for (UINT eye = 0; eye < 2; ++eye)
			{
				D3D11_MAPPED_SUBRESOURCE mapped{};
				Check(consumerContext->Map(staging.Get(), eye, D3D11_MAP_READ, 0, &mapped));
				const auto* pixel = static_cast<const std::uint8_t*>(mapped.pData) + 4 * mapped.RowPitch + 4 * 4;
				const bool valid = pixel[0] == (eye == 0 ? 255 : 0) && pixel[1] == (eye == 1 ? 255 : 0) && pixel[2] == 0 && pixel[3] == 255;
				if (!valid)
					std::fprintf(stderr, "samples=%u eye=%u pixel=%u,%u,%u,%u\n", samples, eye, pixel[0], pixel[1], pixel[2], pixel[3]);
				consumerContext->Unmap(staging.Get(), eye);
				if (!valid)
					throw std::runtime_error("Shared compositor stereo pixels differ");
			}
#ifdef SIMXR_TEST_D3D11_VULKAN
			VerifyVulkan(frame);
			LayerImageDescription image{};
			image.arraySlice = 1;
			image.rectangle = {{2, 1}, {4, 3}};
			std::shared_ptr<D3D11Frame> raw;
			Check(backend.PrepareCapture(image, sources.front(), raw));
			Check(backend.Submit(raw));
			VerifyVulkan(raw, false, {4, 3}, 1);
			if (viewFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
				continue;
			VerifySessionClose(device.Get(), source.Get());
			VerifySessionCaptures(device.Get(), source.Get(), DXGI_FORMAT_UNKNOWN);
			for (const auto depthFormat : {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
				DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
				VerifySessionCaptures(device.Get(), source.Get(), depthFormat);
#endif
		}
#ifdef SIMXR_TEST_D3D11_VULKAN
		for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT})
		{
			D3D11_TEXTURE2D_DESC description{};
			description.Width = description.Height = 8;
			description.MipLevels = 4;
			description.ArraySize = 2;
			description.Format = format;
			description.SampleDesc.Count = 1;
			description.BindFlags = D3D11_BIND_RENDER_TARGET;
			ComPtr<ID3D11Texture2D> texture;
			Check(device->CreateTexture2D(&description, nullptr, &texture));
			for (UINT mip = 0; mip < 4; ++mip)
			{
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				view.Format = format;
				view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				view.Texture2DArray = {mip, 1, 1};
				ComPtr<ID3D11RenderTargetView> target;
				Check(device->CreateRenderTargetView(texture.Get(), &view, &target));
				const float color[]{mip == 3 ? 0.0f : 1.0f, 0, mip == 3 ? 1.0f : 0.0f, 1};
				context->ClearRenderTargetView(target.Get(), color);
			}
			LayerImageDescription image{};
			image.arraySlice = 1;
			image.rectangle = {{2, 1}, {4, 3}};
			std::shared_ptr<D3D11Frame> retained;
			Check(backend.PrepareCapture(image, {texture.Get(), format}, retained, true));
			Check(backend.Submit(retained));
			VerifyVulkan(retained, true);
		}
		std::puts("PASS: retained D3D11 RGBA8/float16/float32/RGB10 mip chains reach independent Vulkan composition");
#endif
		std::puts("PASS: canonical D3D11 shader, array slices, MSAA, shared texture/fence and application state preservation");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
