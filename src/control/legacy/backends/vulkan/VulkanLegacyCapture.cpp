#include "control/legacy/backends/vulkan/VulkanLegacyCapture.h"
#include "Pch.h"
#include "flicker_detector.h"
#include "runtime/Session.h"

namespace simxr
{
static constexpr std::size_t s_maxCaptureJobs = 4;
static constexpr std::size_t s_maxCaptureSources = 64;
static constexpr std::size_t s_maxSourceBytes = 512 * 1024 * 1024;

static bool CaptureFormat(VkFormat format)
{
	return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
		   format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

void VulkanLegacyCapture::Observe(const XrFrameEndInfo& info, const CompositionPlan& composition,
								  const VulkanCaptureBindings& bindings) noexcept
{
	try
	{
		Submit(info, composition, bindings);
		std::uint32_t projections = 0;
		for (std::uint32_t layer = 0; layer < info.layerCount; ++layer)
			if (info.layers[layer]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
				++projections;
		m_diagnostics->ObserveSubmission(m_session->Snapshot().lastSubmitted.value, projections, info.layerCount);
	}
	catch (const std::exception& error)
	{
		Log(error.what());
	}
	catch (...)
	{
		Log("submitted frame observer failed");
	}
}

struct VulkanLegacyCapture::Job
{
	std::shared_ptr<VulkanReadbackBatch> readback;
	FrameId frame;
	RequestState request;
	CaptureImagePlan plan;
	std::vector<VulkanCaptureSource> sources;
	std::size_t sourceBytes = 0;
	bool hasProjection = false;
	std::uint32_t quadLayers = 0;
	std::optional<LegacyScreenshot> image;
};

VulkanLegacyCapture::VulkanLegacyCapture(std::shared_ptr<VulkanDevice> device, std::shared_ptr<Session> session,
										 std::shared_ptr<LegacyFileChannel> files,
										 std::shared_ptr<flicker::Diagnostics> diagnostics)
	: m_device(std::move(device)), m_session(std::move(session)), m_files(std::move(files)),
	  m_diagnostics(std::move(diagnostics))
{
	if (!m_device || !m_session || !m_files || !m_diagnostics)
		throw std::invalid_argument("Legacy capture requires matching device, session and output owners");
}

VulkanLegacyCapture::~VulkanLegacyCapture() = default;

void VulkanLegacyCapture::Request(LegacyCaptureRequest request, FrameId frame)
{
	if ((request.eye != "left" && request.eye != "right" && request.eye != "both") ||
		(request.layer != "projection" && request.layer != "quad" && request.layer != "all"))
		throw std::invalid_argument("Unknown legacy capture selection");
	std::lock_guard lock(m_mutex);
	if (!m_closed)
		m_request = RequestState{std::move(request), frame};
}

std::optional<std::size_t> VulkanLegacyCapture::AddSource(Job& job, const LayerImageDescription& image,
														  const VulkanCaptureBindings& bindings)
{
	auto source = bindings.ResolveSource(image);
	if (!source || !source->region.image || !CaptureFormat(source->region.image->format))
		return {};
	for (std::size_t index = 0; index < job.sources.size(); ++index)
	{
		const auto& existing = job.sources[index].region;
		const auto& incoming = source->region;
		if (existing.image == incoming.image && existing.offset.x == incoming.offset.x &&
			existing.offset.y == incoming.offset.y && existing.extent.width == incoming.extent.width &&
			existing.extent.height == incoming.extent.height && existing.layer == incoming.layer)
			return index;
	}
	if (job.sources.size() >= s_maxCaptureSources)
		return {};
	const auto bytes = CapturePixelBytes(source->region.extent.width, source->region.extent.height);
	if (bytes > s_maxSourceBytes - job.sourceBytes)
		return {};
	job.sources.push_back(std::move(*source));
	job.sourceBytes += bytes;
	return job.sources.size() - 1;
}

std::unique_ptr<VulkanLegacyCapture::Job> VulkanLegacyCapture::Prepare(const CompositionPlan& composition,
																	   XrExtent2Di outputExtent,
																	   const VulkanCaptureBindings& bindings,
																	   const RequestState& request)
{
	if (outputExtent.width <= 0 || outputExtent.height <= 0)
		return {};
	auto job = std::make_unique<Job>();
	job->frame = m_session->Snapshot().lastSubmitted;
	job->request = request;
	auto& plan = job->plan;
	plan.firstEye = job->request.selection.eye == "right" ? 1 : 0;
	plan.eyeCount = job->request.selection.eye == "both" ? 2 : 1;
	plan.width = static_cast<std::uint32_t>(outputExtent.width);
	plan.height = static_cast<std::uint32_t>(outputExtent.height);
	const auto eyeBytes = CapturePixelBytes(plan.width, plan.height);
	if (eyeBytes > s_maxSourceBytes / plan.eyeCount)
		return {};
	for (const auto& draw : composition.draws)
	{
		if (draw.eye < plan.firstEye || draw.eye >= plan.firstEye + plan.eyeCount)
			continue;
		const bool projection = draw.constants.control[0] == 0;
		if ((projection && job->request.selection.layer == "quad") ||
			(!projection && job->request.selection.layer == "projection"))
			continue;
		if (plan.draws.size() >= 128)
			return {};
		const auto source = AddSource(*job, draw.source, bindings);
		if (!source)
			return {};
		CaptureImageDraw capture{draw.constants, draw.eye, *source};
		capture.constants.imageRectangle = {0, 0, 1, 1};
		plan.draws.push_back(capture);
	}
	if (job->sources.empty())
		return {};
	std::vector<VulkanReadbackRegion> regions;
	regions.reserve(job->sources.size());
	for (const auto& source : job->sources)
		regions.push_back(source.region);
	job->readback = VulkanReadbackBatch::Prepare(m_device, regions);
	return job;
}

void VulkanLegacyCapture::Submit(const XrFrameEndInfo& frame, const CompositionPlan& composition,
								 const VulkanCaptureBindings& bindings)
{
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return;
	PollJobs();
	bool hasProjection = false;
	XrExtent2Di outputExtent{static_cast<std::int32_t>(composition.width),
							 static_cast<std::int32_t>(composition.height)};
	std::uint32_t quadLayers = 0;
	for (std::uint32_t index = 0; index < frame.layerCount; ++index)
	{
		if (frame.layers[index]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
		{
			const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(frame.layers[index]);
			const auto firstEye = m_request && m_request->selection.eye == "right" ? 1u : 0u;
			if (projection.viewCount > firstEye)
				outputExtent = projection.views[firstEye].subImage.imageRect.extent;
			hasProjection = true;
		}
		if (frame.layers[index]->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
			++quadLayers;
	}
	auto submit = [&](const RequestState& request, XrExtent2Di extent)
	{
		if ((!hasProjection && !quadLayers) || m_jobs.size() >= s_maxCaptureJobs ||
			(request.selection.layer == "projection" && !hasProjection) ||
			(request.selection.layer == "quad" && !quadLayers))
			return false;
		auto job = Prepare(composition, extent, bindings, request);
		if (!job)
			return false;
		job->hasProjection = hasProjection;
		job->quadLayers = request.selection.layer == "projection" ? 0 : quadLayers;
		m_jobs.push_back(std::move(job));
		auto& submitted = *m_jobs.back();
		const auto result = submitted.readback->Submit();
		for (const auto& source : submitted.sources)
			bindings.MarkSourceUsed(source, submitted.readback);
		if (result != VK_SUCCESS)
		{
			Log("Legacy capture submission failed");
			return false;
		}
		return true;
	};
	if (m_request && submit(*m_request, outputExtent))
		m_request.reset();
}

void VulkanLegacyCapture::CompleteImage(Job& job)
{
	auto pixels = job.readback->ReadPixels();
	std::vector<CapturePixels> sources;
	sources.reserve(job.sources.size());
	for (std::size_t index = 0; index < job.sources.size(); ++index)
	{
		const auto& source = job.sources[index].region;
		const auto rgba =
			source.image->format == VK_FORMAT_R8G8B8A8_UNORM || source.image->format == VK_FORMAT_R8G8B8A8_SRGB;
		sources.push_back(
			{source.extent.width, source.extent.height, rgba ? CaptureColorOrder::Rgba : CaptureColorOrder::Bgra,
			 (source.image->format == VK_FORMAT_R8G8B8A8_SRGB || source.image->format == VK_FORMAT_B8G8R8A8_SRGB)
				 ? CaptureColorEncoding::Srgb
				 : CaptureColorEncoding::Linear,
			 std::move(pixels[index])});
	}
	LegacyScreenshot image;
	image.selection = job.request.selection;
	image.width = job.plan.width * job.plan.eyeCount;
	image.height = job.plan.height;
	image.bgra = ComposeCaptureImage(job.plan, sources);
	m_diagnostics->ObservePreview(image.bgra.data(), image.width, image.height, 0, job.frame.value, job.frame.value);
	if (job.quadLayers)
	{
		flicker::UiFrameInfo ui;
		ui.quadLayers = job.quadLayers;
		ui.projectionRefreshed = job.hasProjection;
		ui.freshReadback = true;
		ui.composed = true;
		m_diagnostics->ObserveUi(image.bgra.data(), image.width, image.height, 0, job.frame.value, job.frame.value, ui);
	}
	image.status = "{\"layer\":\"" + job.request.selection.layer + "\",\"eye\":\"" + job.request.selection.eye +
				   "\",\"requestFrame\":" + std::to_string(job.request.frame.value) +
				   ",\"capturedFrame\":" + std::to_string(job.frame.value) +
				   ",\"completedFrame\":" + std::to_string(m_session->Snapshot().lastSubmitted.value) +
				   ",\"width\":" + std::to_string(image.width) + ",\"height\":" + std::to_string(image.height) + "}";
	job.image = std::move(image);
	job.sources.clear();
	job.readback.reset();
}

void VulkanLegacyCapture::PollJobs()
{
	while (!m_jobs.empty())
	{
		auto& job = *m_jobs.front();
		try
		{
			if (!job.image)
			{
				const auto status = job.readback->PollCompletion();
				if (status == CompletionStatus::Pending ||
					(status == CompletionStatus::Failed && !job.readback->NativeIdle()))
					return;
				if (status == CompletionStatus::Failed)
				{
					Log("Legacy screenshot readback failed");
					m_jobs.pop_front();
					continue;
				}
				CompleteImage(job);
			}
			if (!m_files->SaveScreenshot(*job.image))
			{
				if (!m_files->Status().closed)
					return;
				Log("Legacy screenshot writer is closed");
			}
		}
		catch (const std::exception& error)
		{
			Log(error.what());
		}
		m_jobs.pop_front();
	}
}

void VulkanLegacyCapture::Poll()
{
	std::lock_guard lock(m_mutex);
	PollJobs();
}

void VulkanLegacyCapture::Log(std::string_view message) noexcept
{
	try
	{
		m_files->Log(std::string(message));
	}
	catch (...)
	{
	}
}

VkResult VulkanLegacyCapture::Close(std::uint64_t timeout)
{
	std::lock_guard closeLock(m_closeMutex);
	using Clock = std::chrono::steady_clock;
	const auto now = Clock::now();
	const auto maximum = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::time_point::max() - now).count();
	const auto duration = std::chrono::nanoseconds(std::min(timeout, static_cast<std::uint64_t>(maximum)));
	const auto deadline = now + duration;
	std::vector<std::shared_ptr<VulkanReadbackBatch>> readbacks;
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		for (const auto& job : m_jobs)
			if (job->readback)
				readbacks.push_back(job->readback);
	}
	for (const auto& readback : readbacks)
	{
		const auto remaining =
			std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero());
		const auto result = readback->Wait(
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
		if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST && !readback->NativeIdle())
			return result;
	}
	readbacks.clear();
	for (;;)
	{
		{
			std::lock_guard lock(m_mutex);
			PollJobs();
			if (m_jobs.empty())
				break;
		}
		const auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining <= std::chrono::steady_clock::duration::zero())
			return VK_TIMEOUT;
		if (!m_files->DrainScreenshots(std::chrono::duration_cast<std::chrono::milliseconds>(remaining)) &&
			!m_files->Status().closed)
			return VK_TIMEOUT;
	}
	const auto remaining = std::max(deadline - Clock::now(), Clock::duration::zero());
	return m_files->DrainScreenshots(std::chrono::duration_cast<std::chrono::milliseconds>(remaining)) ? VK_SUCCESS
																									 : VK_TIMEOUT;
}
} // namespace simxr
