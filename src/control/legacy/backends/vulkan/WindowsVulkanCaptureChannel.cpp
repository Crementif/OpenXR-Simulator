#include "control/legacy/backends/vulkan/WindowsVulkanCaptureChannel.h"
#include "Pch.h"
#include "runtime/Session.h"

namespace simxr
{
WindowsVulkanCaptureChannel::WindowsVulkanCaptureChannel(std::shared_ptr<mcp::WindowsLegacyChannel> files,
														 std::shared_ptr<mcp::WindowsLegacyCapture> capture,
														 std::shared_ptr<Session> session,
														 std::function<mcp::HeadPoseCommand()> head,
														 std::function<void(const mcp::HeadPoseCommand&)> step)
	: m_files(std::move(files)), m_capture(std::move(capture)), m_session(std::move(session)), m_head(std::move(head)),
	  m_step(std::move(step))
{
	if (!m_files || !m_capture || !m_session || !m_head || !m_step)
		throw std::invalid_argument("Vulkan compatibility capture requires its file worker");
}

LegacyFileRequests WindowsVulkanCaptureChannel::TakeRequests()
{
	auto requests = m_files->TakeRequests();
	LegacyFileRequests result;
	if (requests.screenshot)
		result.capture = LegacyCaptureRequest{std::move(requests.capture.eye), std::move(requests.capture.layer)};
	result.projectionLog = requests.projectionLog;
	const auto head = m_head();
	if (requests.burst.valid)
	{
		if (m_capture->BeginBurst(requests.burst.frames))
		{
			++m_burstGeneration;
			m_pendingStep = requests.burst.pose;
		}
		else
			m_files->Acknowledge("burst", false);
	}
	if (m_capture->BurstActive())
	{
		const auto metadata = BurstMetadata(head);
		const auto generation = m_burstGeneration;
		result.frameCapture =
			LegacyFrameCapture{{"both", "all"},
							   [this, metadata, generation](LegacyScreenshot& image)
							   {
								   if (generation == m_burstGeneration)
									   m_capture->BurstOnFrame(image.bgra.data(), image.width, image.height,
															   std::size_t(image.width) * 4, metadata);
								   return true;
							   },
							   [this]
							   {
								   // Step only after the baseline readback is admitted, including under queue pressure.
								   if (m_pendingStep.valid)
								   {
									   m_step(m_pendingStep);
									   m_pendingStep.valid = false;
								   }
							   }};
	}
	return result;
}

mcp::BurstFrameMeta WindowsVulkanCaptureChannel::BurstMetadata(const mcp::HeadPoseCommand& head) const
{
	mcp::BurstFrameMeta metadata;
	const auto snapshot = m_session->Snapshot();
	metadata.frame = snapshot.lastSubmitted.value;
	metadata.headX = head.x;
	metadata.headY = head.y;
	metadata.headZ = head.z;
	metadata.headYaw = head.yaw;
	metadata.headPitch = head.pitch;
	metadata.headRoll = head.roll;
	const auto record = m_session->History().Find({snapshot.generation, snapshot.lastSubmitted});
	if (!record || !record->submission)
		throw std::runtime_error("Burst capture requires submitted frame metadata");
	const auto& submission = *record->submission;
	metadata.tMs = std::chrono::duration<double, std::milli>(submission.displayTime.value).count();
	if (submission.layers)
		for (const auto& layer : submission.layers->layers)
		{
			const auto* projection = std::get_if<ProjectionLayerDescription>(&layer);
			if (!projection || projection->views.size() < 2)
				continue;
			auto& entry = metadata.proj;
			entry.frame = metadata.frame;
			const auto& pose = projection->views.front().poseInSpace;
			entry.posX = pose.position.x;
			entry.posY = pose.position.y;
			entry.posZ = pose.position.z;
			entry.poseQx = pose.orientation.x;
			entry.poseQy = pose.orientation.y;
			entry.poseQz = pose.orientation.z;
			entry.poseQw = pose.orientation.w;
			for (std::size_t eye = 0; eye < 2; ++eye)
			{
				const auto& view = projection->views[eye];
				entry.aL[eye] = view.fov.angleLeft;
				entry.aR[eye] = view.fov.angleRight;
				entry.aU[eye] = view.fov.angleUp;
				entry.aD[eye] = view.fov.angleDown;
				entry.rectX[eye] = view.color.rectangle.offset.x;
				entry.rectY[eye] = view.color.rectangle.offset.y;
				entry.rectW[eye] = view.color.rectangle.extent.width;
				entry.rectH[eye] = view.color.rectangle.extent.height;
			}
		}
	return metadata;
}

bool WindowsVulkanCaptureChannel::FinishCaptures(std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!m_capture->FlushBurst())
	{
		const auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining <= std::chrono::steady_clock::duration::zero() ||
			!m_files->DrainImages(std::chrono::duration_cast<std::chrono::milliseconds>(remaining)))
			return false;
	}
	return m_files->DrainImages(
		std::max(std::chrono::milliseconds::zero(),
				 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())));
}

bool WindowsVulkanCaptureChannel::Publish(LegacyOutputFile file, std::string document)
{
	if (file != LegacyOutputFile::ProjectionLog)
		return false;
	return m_files->Publish(mcp::WindowsDocument::ProjectionLog, std::move(document));
}

bool WindowsVulkanCaptureChannel::SaveScreenshot(LegacyScreenshot& screenshot)
{
	mcp::ImageOutput output;
	output.statusFilename = "screenshot_status.json";
	output.status = screenshot.status;
	output.images.push_back({screenshot.selection.layer == "quad" ? "screenshot_quad.bmp" : "screenshot.bmp",
							 screenshot.width,
							 screenshot.height,
							 {}});
	output.images.front().bgra.swap(screenshot.bgra);
	try
	{
		if (m_files->PublishImages(output))
			return true;
	}
	catch (...)
	{
		output.images.front().bgra.swap(screenshot.bgra);
		throw;
	}
	output.images.front().bgra.swap(screenshot.bgra);
	return false;
}

bool WindowsVulkanCaptureChannel::DrainScreenshots(std::chrono::milliseconds timeout)
{
	return m_files->DrainImages(timeout);
}

bool WindowsVulkanCaptureChannel::Log(std::string message)
{
	return m_files->Log(std::move(message));
}

LegacyFileStatus WindowsVulkanCaptureChannel::Status() const
{
	const auto status = m_files->Status();
	return {status.closed, status.errors, status.droppedLogs, status.detail};
}
} // namespace simxr
