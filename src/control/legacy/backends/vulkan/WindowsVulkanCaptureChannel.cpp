#include "control/legacy/backends/vulkan/WindowsVulkanCaptureChannel.h"

#include <stdexcept>
#include <utility>

namespace simxr
{
WindowsVulkanCaptureChannel::WindowsVulkanCaptureChannel(std::shared_ptr<mcp::WindowsLegacyChannel> files)
	: m_files(std::move(files))
{
	if (!m_files)
		throw std::invalid_argument("Vulkan compatibility capture requires its file worker");
}

LegacyFileRequests WindowsVulkanCaptureChannel::TakeRequests()
{
	auto requests = m_files->TakeRequests();
	LegacyFileRequests result;
	if (requests.screenshot)
		result.capture = LegacyCaptureRequest{std::move(requests.capture.eye), std::move(requests.capture.layer)};
	result.projectionLog = requests.projectionLog;
	if (requests.burst.valid)
	{
		m_files->Log("Vulkan burst capture is not available through the compatibility observer yet");
		m_files->Acknowledge("burst", false);
	}
	return result;
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
