#include "VulkanRetentionProbe.h"
#include "Pch.h"
#include "math/Math.h"
#include "VulkanReprojectionProbe.h"
#include "graphics/RotationalReprojection.h"
#include "graphics/backends/vulkan/VulkanReprojection.h"
#include "graphics/backends/vulkan/VulkanFrameSubmission.h"
#include "runtime/Session.h"

#include <cstdio>
#include <stdexcept>
#include <thread>

namespace simxr::probe
{
static void Require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}
static void Await(const GpuCompletion& output)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;)
	{
		const auto status = output.PollCompletion();
		if (status == CompletionStatus::Complete)
			return;
		Require(status != CompletionStatus::Failed && std::chrono::steady_clock::now() < deadline,
				"Automatic retention failed or timed out");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
struct SubmittedFrame
{
	PresentationTick display;
	std::shared_ptr<const FrameLayers> layers;
	CompositionTarget target;
	PoseSample head;
};

class RetentionBindings final : public VulkanFrameBindings
{
  public:
	explicit RetentionBindings(std::shared_ptr<VulkanImage> image) : m_image(std::move(image)) {}
	std::optional<SwapchainDescription> Describe(XrSwapchain swapchain) const override
	{
		if (swapchain != m_handle)
			return {};
		return SwapchainDescription{m_image->extent.width, m_image->extent.height, m_image->layers, true};
	}
	std::shared_ptr<VulkanImage> ReleasedImage(XrSwapchain swapchain) const override
	{
		return swapchain == m_handle ? m_image : nullptr;
	}
	void MarkReleasedUsed(XrSwapchain swapchain,
						  const std::shared_ptr<const GpuCompletion>& completion) const noexcept override
	{
		if (swapchain == m_handle)
			m_used = completion;
	}
	const XrSwapchain m_handle = reinterpret_cast<XrSwapchain>(201);
	const std::shared_ptr<VulkanImage> m_image;
	mutable std::shared_ptr<const GpuCompletion> m_used;
};

RetainedSubmission RetainSubmittedLayers(VulkanBackend& backend, VulkanInspection& inspection,
										 const std::shared_ptr<VulkanImage>& image)
{
	RetentionBindings bindings(image);
	const std::array extensions{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME};
	auto instance =
		std::make_shared<Instance>(reinterpret_cast<XrInstance>(810), extensions, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(811), instance);
	CommandConfiguration initial;
	initial.views.outerWidth = 48;
	initial.views.outerHeight = 32;
	initial.views.insetWidth = 24;
	initial.views.insetHeight = 16;
	initial.tracking.head = {{EulerAngles(0.17f, 0.05f, 0.02f), {0.1f, 1.6f, -0.2f}}, TrackedPoseFlags, {}};
	Require(session.ConfigureCommands(1, initial) == XR_SUCCESS &&
				session.ConfigureCapturePool(backend.Reservations(), true, {}, false, true) == XR_SUCCESS,
			"Configure retention session");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Begin automatic retention session");
	Require(session.Display().Pause(), "Freeze retention selection");
	XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) == XR_SUCCESS &&
				session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), view) == XR_SUCCESS,
			"Create submitted layer spaces");
	RetainedSubmission retained{};
	const auto submit = [&](bool retain) -> SubmittedFrame
	{
		FramePrediction prediction;
		Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
				"Begin retained frame");
		const auto located = session.ViewsAt(prediction.displayTime);
		Require(located.views.size() == 4 && located.head.flags == TrackedPoseFlags, "Submitted head/eyes missing");
		std::array<XrCompositionLayerProjectionView, 4> views;
		for (std::uint32_t index = 0; index < views.size(); ++index)
			views[index] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
							nullptr,
							located.views[index].pose.pose,
							located.views[index].fov,
							{bindings.m_handle,
							 {{static_cast<std::int32_t>(2 + index), static_cast<std::int32_t>(3 + index)}, {4, 5}},
							 index}};
		const auto flags =
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		XrCompositionLayerProjection projection{
			XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, flags, local, 4, views.data()};
		XrCompositionLayerQuad world{XR_TYPE_COMPOSITION_LAYER_QUAD,
									 nullptr,
									 flags,
									 local,
									 XR_EYE_VISIBILITY_RIGHT,
									 views[3].subImage,
									 Compose(located.head.pose, {{0, 0, 0, 1}, {0.2f, 0, -2}}),
									 {1.2f, 0.8f}};
		auto hud = world;
		hud.space = view;
		hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		hud.pose = {{0, 0, 0, 1}, {-0.3f, 0.4f, -2}};
		hud.size = {0.6f, 0.4f};
		auto behind = world;
		behind.subImage = {bindings.m_handle, {{0, 0}, {4, 5}}, 0};
		behind.pose = Compose(located.head.pose, {EulerAngles(3.14159265f, 0, 0), {0, 0, 2}});
		const std::array layers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
								reinterpret_cast<const XrCompositionLayerBaseHeader*>(&world),
								reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud),
								reinterpret_cast<const XrCompositionLayerBaseHeader*>(&behind)};
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,
								 nullptr,
								 prediction.displayTime.value.count(),
								 XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
								 static_cast<std::uint32_t>(layers.size()),
								 layers.data()};
		const FrameKey key{session.Snapshot().generation, prediction.id};
		{
			VulkanFrameSubmission native(session, backend, &inspection, bindings, retain);
			FrameSubmission submission(session, native);
			const auto result = submission.Execute(end,
												   [&bindings](XrSwapchain swapchain)
												   {
													   return bindings.Describe(swapchain);
												   });
			if (XR_FAILED(result))
				std::fprintf(stderr, "Automatic submission retain=%d result=%d\n", retain, result);
			Require(result == XR_SUCCESS, "Production frame submission with optional retention failed");
		}
		Require(bindings.m_used != nullptr, "Retained application swapchain was not protected by batch completion");
		Await(*bindings.m_used);
		const auto entries = session.Display().Retain();
		auto found = std::find_if(entries.begin(), entries.end(),
								  [&](const auto& entry)
								  {
									  return entry.key == key;
								  });
		Require(found != entries.end() && found->output, "Submitted baseline disappeared");
		Await(*found->output);
		for (unsigned tick = 0; tick < 8; ++tick)
		{
			const auto selected = session.Display().Step();
			Require(selected.has_value(), "Step retained display");
			if (selected->source == key)
			{
				return {
					*selected,
					session.History().Find(key)->submission->layers,
					{located.views[0].description.width,
					 located.views[0].description.height,
					 {{{located.views[0].pose, located.views[0].fov}, {located.views[1].pose, located.views[1].fov}}}},
					located.head};
			}
		}
		throw std::runtime_error("Submitted frame never selected");
	};
	{
		const auto repeatOnly = submit(false).display;
		Require(!repeatOnly.reprojection.source && repeatOnly.reprojection.status == XR_SUCCESS &&
					backend.ReprojectionReservations().InUse() == 0,
				"Repeat policy retained layers unnecessarily");
	}
	std::vector<std::shared_ptr<FrameReservation>> captureBlockers;
	while (auto slot = backend.Reservations().Reserve())
		captureBlockers.push_back(std::move(slot));
	{
		const auto submitted = submit(true);
		const auto& selected = submitted.display;
		retained.layers = submitted.layers;
		retained.target = submitted.target;
		retained.head = submitted.head;
		if (!selected.reprojection.source)
			std::fprintf(stderr, "Retained input status=%d format=%d\n", selected.reprojection.status, image->format);
		Require(selected.reprojection.source && selected.reprojection.status == XR_SUCCESS,
				"Full capture budget prevented independent per-layer retention");
		retained.source = selected.reprojection.source;
		retained.baseline = selected.output;
		retained.format = image->format;
	}
	Await(*retained.source);
	Require(backend.ReprojectionReservations().InUse() == 5,
			"Cropped sources were not deduplicated or hidden layers were omitted");
	const auto resolve = [&](XrSwapchain source)
	{
		return bindings.ReleasedImage(source);
	};
	std::unique_ptr<VulkanReprojectionPreparation> cancelled;
	Require(VulkanReprojectionPreparation::Prepare(backend, inspection, *retained.layers, retained.target,
												   retained.head, resolve, 1, cancelled) == XR_ERROR_LIMIT_REACHED &&
				!cancelled,
			"Partial capacity rejection published retained work");
	Require(VulkanReprojectionPreparation::Prepare(backend, inspection, *retained.layers, retained.target,
												   retained.head, resolve, 32, cancelled) == XR_SUCCESS,
			"Prepare pending retained inputs");
	auto pending = cancelled->Source();
	auto unchanged = retained.baseline;
	Require(pending->PollCompletion() == CompletionStatus::Pending &&
				pending->Reproject(retained.head, unchanged) == XR_TIMEOUT_EXPIRED && unchanged == retained.baseline,
			"Pending retention sampled unsubmitted images or replaced the fallback");
	cancelled.reset();
	Require(inspection.Drain(5000000000ULL) == VK_SUCCESS, "Drain cancelled partial retention");
	Require(pending->PollCompletion() == CompletionStatus::Failed, "Cancelled input remained pending");
	pending.reset();
	backend.RetireCompleted();
	std::vector<std::shared_ptr<FrameReservation>> blockers;
	while (auto slot = backend.ReprojectionReservations().Reserve())
		blockers.push_back(std::move(slot));
	captureBlockers.clear();
	session.Scheduler().Pause();
	const auto capture = session.ReserveCapture();
	Require(capture.result == SnapshotAdmission::Accepted, "Reprojection pressure blocked exact capture admission");
	{
		Require(session.Scheduler().GrantPermits() == XR_SUCCESS, "Grant one exact capture frame");
		const auto fallback = submit(true).display;
		Require(fallback.output && !fallback.reprojection.source &&
					fallback.reprojection.status == XR_ERROR_LIMIT_REACHED,
				"Optional retention exhaustion lost baseline or its failure reason");
		session.Captures().Poll();
		const auto screenshot = session.Captures().Read(capture.id);
		Require(screenshot && screenshot->phase == SnapshotPhase::Ready && screenshot->output == fallback.output,
				"Reprojection capacity exhaustion prevented exact-frame screenshot completion");
	}
	session.Close();
	return retained;
}

void VerifySubmittedLayers(RetainedSubmission retained, bool verifyPixels)
{
	const std::array<std::array<std::uint8_t, 4>, 4> colors{
		{{64, 128, 192, 77}, {17, 45, 93, 125}, {111, 37, 201, 222}, {33, 199, 78, 159}}};
	std::vector<std::shared_ptr<const VulkanImagePixels>> reference;
	if (verifyPixels)
		for (std::uint32_t slice = 0; slice < colors.size(); ++slice)
		{
			auto pixels = std::make_shared<VulkanImagePixels>();
			pixels->format =
				(retained.format == VK_FORMAT_R8G8B8A8_UNORM || retained.format == VK_FORMAT_B8G8R8A8_UNORM)
					? VK_FORMAT_R8G8B8A8_UNORM
					: VK_FORMAT_R8G8B8A8_SRGB;
			pixels->width = pixels->height = 16;
			pixels->layers = 1;
			for (std::uint32_t y = 0; y < 16; ++y)
				for (std::uint32_t x = 0; x < 16; ++x)
				{
					auto color = x >= 2 + slice && x < 6 + slice && y >= 3 + slice && y < 8 + slice
									 ? colors[slice]
									 : std::array<std::uint8_t, 4>{0, 0, 255, 255};
					if ((retained.format == VK_FORMAT_B8G8R8A8_SRGB || retained.format == VK_FORMAT_B8G8R8A8_UNORM) &&
						x >= 2 + slice && x < 6 + slice && y >= 3 + slice && y < 8 + slice)
						std::swap(color[0], color[2]);
					pixels->rgba.insert(pixels->rgba.end(), color.begin(), color.end());
				}
			reference.push_back(std::move(pixels));
		}
	VirtualDisplay display;
	auto head = retained.head;
	const RuntimeTime epoch{Duration{std::max<std::int64_t>(1, head.sampleTime.value.count())}};
	display.ConfigurePolicy(PresentationPolicy::Rotational,
							[&](RuntimeTime)
							{
								return head;
							});
	display.Start({1}, {epoch, std::chrono::milliseconds(1)}, ExecutionMode::Deterministic);
	Require(display.Pause(), "Pause native display for exact output inspection");
	const FrameKey source{{1}, {1}};
	Require(display.Submit({source, epoch, retained.baseline, {}, {retained.source}}) ==
				PresentationAdmission::Accepted,
			"Submit surviving retained layers to rotational display");
	std::uint64_t priorImageTick = 0;
	for (const float yaw : {0.0f, 0.25f, 3.14159265f})
	{
		head = retained.head;
		head.sampleTime = {epoch.value + Duration{static_cast<std::int64_t>(display.NextTick().value) * 1000000}};
		head.pose.orientation = Compose(head.pose, {EulerAngles(yaw, 0, 0), {}}).orientation;
		auto selected = display.Step();
		Require(selected && selected->source == source, "Display changed the submitted source identity");
		const auto submissionTime = selected->reprojectionSubmissionTime;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (yaw != 0 && (!selected->reprojected || selected->displayHead->sampleTime != head.sampleTime))
		{
			Require(selected->output && selected->output->PollCompletion() == CompletionStatus::Complete &&
						std::chrono::steady_clock::now() < deadline,
					"Rotational display published pending work or failed to complete");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			selected = display.Step();
			Require(selected.has_value(), "Native display stopped advancing");
		}
		const auto output = display.Snapshot().latest->output;
		Require(output == selected->output && selected->source == source && selected->imageTick &&
					(yaw == 0 ? output == retained.baseline && !selected->reprojected
							  : selected->imageTick->value > priorImageTick),
				"Native displayed output, image tick and source identity disagree");
		priorImageTick = selected->imageTick->value;
		std::printf("case=rotational-display format=%d yaw=%.3f sourceFrame=1 imageTick=%llu "
					"enqueueCpuUs=%.3f observedCompletionUs=%.3f\n",
					retained.format, yaw, static_cast<unsigned long long>(priorImageTick),
					submissionTime.count() / 1000.0, selected->reprojectionCompletionLatency.count() / 1000.0);
		if (verifyPixels)
		{
			CompositionPlan expected;
			Require(PrepareRotationalComposition(
						*retained.layers, retained.target, retained.head, head,
						[](XrSwapchain)
						{
							return XrExtent2Di{16, 16};
						},
						expected) == XR_SUCCESS,
					"Prepare original-texture reference");
			for (auto& draw : expected.draws)
				draw.source.source =
					reinterpret_cast<XrSwapchain>(static_cast<std::uintptr_t>(draw.source.arraySlice) + 1);
			VerifyCompositionPixels(expected, *std::dynamic_pointer_cast<const VulkanInspectionFrame>(output),
									reference);
			if (yaw == 0)
				VerifyCompositionPixels(
					expected, *std::dynamic_pointer_cast<const VulkanInspectionFrame>(retained.baseline), reference);
		}
	}
	std::printf("case=automatic-layer-retention format=%d deduplicated=5 hiddenLayer=yes cropClamp=yes "
				"captureBudgetIsolated=yes pendingAndRollback=yes optionalFailure=repeat "
				"borrowedDeviceDestroyed=yes pixels=%s PASS\n",
				retained.format, verifyPixels ? "all" : "not-requested");
}
} // namespace simxr::probe
