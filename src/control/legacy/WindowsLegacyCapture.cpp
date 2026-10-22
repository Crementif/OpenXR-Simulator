#include "control/legacy/WindowsLegacyCapture.h"
#include "control/legacy/WindowsLegacyFormat.h"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace mcp
{
namespace
{
constexpr std::size_t MaxCaptureBytes = 512 * 1024 * 1024;
void WriteJsonString(std::ostream& output, std::string_view text)
{
	constexpr char HexDigits[] = "0123456789abcdef";
	output << '"';
	for (const unsigned char character : text)
	{
		if (character == '"' || character == '\\')
			output << '\\' << character;
		else if (character < 0x20)
			output << "\\u00" << HexDigits[character >> 4] << HexDigits[character & 0xF];
		else
			output << character;
	}
	output << '"';
}
std::string FrameFilename(std::size_t index)
{
	std::ostringstream name;
	name.imbue(std::locale::classic());
	name << "burst_" << std::setw(3) << std::setfill('0') << index << ".bmp";
	return name.str();
}
void ProjectionJson(std::ostream& output, const ProjLogEntry& entry)
{
	output << "{\"frame\":" << entry.frame << ",\"pose\":{\"x\":" << entry.posX << ",\"y\":" << entry.posY
		   << ",\"z\":" << entry.posZ << ",\"qx\":" << entry.poseQx << ",\"qy\":" << entry.poseQy
		   << ",\"qz\":" << entry.poseQz << ",\"qw\":" << entry.poseQw << '}';
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		const auto name = eye ? "right" : "left";
		output << ",\"" << name << "_fov\":{\"aL\":" << entry.aL[eye] << ",\"aR\":" << entry.aR[eye]
			   << ",\"aU\":" << entry.aU[eye] << ",\"aD\":" << entry.aD[eye] << "},\"" << name << "_rect\":["
			   << entry.rectX[eye] << ',' << entry.rectY[eye] << ',' << entry.rectW[eye] << ',' << entry.rectH[eye]
			   << ']';
	}
	output << '}';
}
} // namespace
WindowsLegacyCapture::WindowsLegacyCapture(std::shared_ptr<WindowsLegacyChannel> files) : m_files(std::move(files))
{
	if (!m_files)
		throw std::invalid_argument("Compatibility capture requires its output worker");
}
void WindowsLegacyCapture::SetFrame(std::uint64_t frame)
{
	std::lock_guard lock(m_mutex);
	m_frame = frame;
}
void WindowsLegacyCapture::RequestScreenshot(ScreenshotRequest request)
{
	std::lock_guard lock(m_mutex);
	std::swap(m_request, request);
	m_requested = true;
	m_requestFrame = m_frame;
	++m_requestVersion;
}
bool WindowsLegacyCapture::ScreenshotRequested() const
{
	std::lock_guard lock(m_mutex);
	return m_requested;
}
bool WindowsLegacyCapture::FailScreenshot(std::string_view reason, std::uint64_t capturedFrame)
{
	std::uint64_t version;
	ImageOutput output;
	{
		std::lock_guard lock(m_mutex);
		if (!m_requested || (capturedFrame && capturedFrame < m_requestFrame))
			return false;
		version = m_requestVersion;
		std::ostringstream status;
		status.imbue(std::locale::classic());
		status << "{\"timestamp\":\"" << detail::LocalTimestamp() << "\",\"success\":false,\"layer\":";
		WriteJsonString(status, m_request.layer);
		status << ",\"eye\":";
		WriteJsonString(status, m_request.eye);
		status << ",\"requestFrame\":" << m_requestFrame
			   << ",\"failedFrame\":" << (capturedFrame ? capturedFrame : m_frame) << ",\"error\":";
		WriteJsonString(status, reason.substr(0, 2048));
		status << '}';
		output.statusFilename = "screenshot_status.json";
		output.status = status.str();
	}
	if (!m_files->PublishImages(output))
		return false;
	std::lock_guard lock(m_mutex);
	if (version == m_requestVersion)
		m_requested = false;
	return true;
}
std::string WindowsLegacyCapture::ScreenshotLayer() const
{
	std::lock_guard lock(m_mutex);
	return m_request.layer;
}
BitmapOutput WindowsLegacyCapture::CopyPixels(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
											  std::size_t stride, bool bgra, std::string filename)
{
	const auto count = std::uint64_t(width) * height;
	const auto rowBytes = std::uint64_t(width) * 4;
	if (!pixels || !count || count > MaxCaptureBytes / 4 || stride < rowBytes ||
		(height > 1 && stride > (std::numeric_limits<std::size_t>::max() - rowBytes) / (height - 1)))
		throw std::invalid_argument("Compatibility capture requires bounded representable color rows");
	BitmapOutput image{std::move(filename), width, height,
					   std::vector<std::uint8_t>(static_cast<std::size_t>(count * 4))};
	for (std::uint32_t row = 0; row < height; ++row)
	{
		auto* target = image.bgra.data() + std::size_t(row) * width * 4;
		std::memcpy(target, pixels + std::size_t(row) * stride, std::size_t(width) * 4);
		if (!bgra)
			for (std::uint32_t column = 0; column < width; ++column)
				std::swap(target[column * 4], target[column * 4 + 2]);
	}
	return image;
}
bool WindowsLegacyCapture::SubmitPixels(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
										std::size_t stride, bool bgra, std::uint64_t capturedFrame)
{
	ScreenshotRequest request;
	BitmapOutput quad;
	std::uint64_t requestFrame;
	std::uint64_t version;
	{
		std::lock_guard lock(m_mutex);
		if (!m_requested || (capturedFrame && capturedFrame < m_requestFrame))
			return false;
		request = m_request;
		requestFrame = m_requestFrame;
		version = m_requestVersion;
		if (!capturedFrame)
			capturedFrame = m_frame;
		if (request.layer == "all" && m_quadFrame == capturedFrame && !m_quad.bgra.empty())
		{
			const auto pixels = std::uint64_t(width) * height;
			if (pixels > MaxCaptureBytes / 4 || m_quad.bgra.size() > MaxCaptureBytes - pixels * 4)
				return false;
			quad = m_quad;
		}
	}
	ImageOutput output;
	output.images.push_back(CopyPixels(pixels, width, height, stride, bgra, "screenshot.bmp"));
	if (!quad.bgra.empty())
		output.images.push_back(std::move(quad));
	output.statusFilename = "screenshot_status.json";
	std::ostringstream status;
	status.imbue(std::locale::classic());
	status << "{\"timestamp\":\"" << detail::LocalTimestamp() << "\",\"layer\":\"" << request.layer << "\",\"eye\":\""
		   << request.eye << "\",\"requestFrame\":" << requestFrame << ",\"capturedFrame\":" << capturedFrame
		   << ",\"width\":" << width << ",\"height\":" << height << '}';
	output.status = status.str();
	if (!m_files->PublishImages(output))
		return false;
	std::lock_guard lock(m_mutex);
	if (version == m_requestVersion)
		m_requested = false;
	return true;
}
void WindowsLegacyCapture::StoreQuad(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
									 std::uint64_t capturedFrame)
{
	auto image = CopyPixels(pixels, width, height, std::size_t(width) * 4, false, "screenshot_quad.bmp");
	std::lock_guard lock(m_mutex);
	std::swap(m_quad, image);
	m_quadFrame = capturedFrame ? capturedFrame : m_frame;
}
bool WindowsLegacyCapture::SubmitQuad(bool completeRequest, std::uint64_t capturedFrame)
{
	ImageOutput output;
	std::uint64_t version;
	{
		std::lock_guard lock(m_mutex);
		if (!capturedFrame)
			capturedFrame = m_quadFrame;
		if (m_quad.bgra.empty() || m_quadFrame != capturedFrame ||
			(completeRequest && (!m_requested || (capturedFrame && capturedFrame < m_requestFrame))))
			return false;
		output.images.push_back(m_quad);
		version = m_requestVersion;
		if (completeRequest)
		{
			output.statusFilename = "screenshot_status.json";
			output.status = "{\"timestamp\":\"" + detail::LocalTimestamp() + "\",\"layer\":\"quad\",\"eye\":\"" +
							m_request.eye + "\",\"requestFrame\":" + std::to_string(m_requestFrame) +
							",\"capturedFrame\":" + std::to_string(capturedFrame) +
							",\"width\":" + std::to_string(m_quad.width) +
							",\"height\":" + std::to_string(m_quad.height) + "}";
		}
	}
	if (!m_files->PublishImages(output))
		return false;
	std::lock_guard lock(m_mutex);
	if (completeRequest && version == m_requestVersion)
		m_requested = false;
	return true;
}
bool WindowsLegacyCapture::SubmitStereo(const std::uint8_t* left, const std::uint8_t* right, std::uint32_t width,
										std::uint32_t height)
{
	if (!left && !right)
		return false;
	const auto eyes = left && right ? 2u : 1u;
	const auto pixels = std::uint64_t(width) * height;
	if (!pixels || pixels > MaxCaptureBytes / (4 * eyes))
		throw std::invalid_argument("Stereo capture exceeds its CPU byte bound");
	const auto total = pixels * 4 * eyes;
	std::vector<std::uint8_t> joined(static_cast<std::size_t>(total));
	for (std::uint32_t row = 0; row < height; ++row)
	{
		const auto bytes = std::size_t(width) * 4;
		std::memcpy(joined.data() + std::size_t(row) * bytes * eyes, (left ? left : right) + std::size_t(row) * bytes,
					bytes);
		if (eyes == 2)
			std::memcpy(joined.data() + std::size_t(row) * bytes * 2 + bytes, right + std::size_t(row) * bytes, bytes);
	}
	return SubmitPixels(joined.data(), width * eyes, height, std::size_t(width) * eyes * 4, false);
}
void WindowsLegacyCapture::RecordProjection(ProjLogEntry projection)
{
	std::lock_guard lock(m_mutex);
	m_projection[m_projectionHead] = projection;
	m_projectionHead = (m_projectionHead + 1) % m_projection.size();
	m_projectionCount = std::min(m_projectionCount + 1, m_projection.size());
}
ProjLogEntry WindowsLegacyCapture::LastProjection() const
{
	std::lock_guard lock(m_mutex);
	return m_projectionCount ? m_projection[(m_projectionHead + m_projection.size() - 1) % m_projection.size()]
							 : ProjLogEntry{};
}
void WindowsLegacyCapture::DumpProjection()
{
	std::array<ProjLogEntry, 64> entries;
	std::size_t count;
	{
		std::lock_guard lock(m_mutex);
		entries = m_projection;
		count = m_projectionCount;
		std::rotate(entries.begin(),
					entries.begin() + (m_projectionHead + m_projection.size() - count) % m_projection.size(),
					entries.end());
	}
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << "{\"entries\":[";
	for (std::size_t index = 0; index < count; ++index)
	{
		if (index)
			output << ',';
		ProjectionJson(output, entries[index]);
	}
	output << "]}";
	m_files->Publish(WindowsDocument::ProjectionLog, output.str());
}
bool WindowsLegacyCapture::BeginBurst(int frames)
{
	frames = std::clamp(frames, 1, 64);
	std::vector<BitmapOutput> images;
	std::vector<BurstFrameMeta> metadata;
	images.reserve(frames);
	metadata.reserve(frames);
	ImageOutput reset;
	reset.resetBurst = true;
	reset.statusFilename = "command_ack.json";
	reset.status = "{\"timestamp\":\"" + detail::LocalTimestamp() + "\",\"command\":\"burst\",\"success\":true}";
	if (!m_files->PublishImages(reset))
		return false;
	std::lock_guard lock(m_mutex);
	m_burstImages.swap(images);
	m_burstMetadata.swap(metadata);
	m_burstBytes = 0;
	m_burstTotal = frames;
	m_burstActive = true;
	return true;
}
bool WindowsLegacyCapture::BurstActive() const
{
	std::lock_guard lock(m_mutex);
	return m_burstActive;
}
bool WindowsLegacyCapture::FlushBurst()
{
	ImageOutput output;
	std::unique_lock lock(m_mutex);
	const auto& metadata = m_burstMetadata;
	if (m_burstImages.empty())
	{
		m_burstActive = false;
		return true;
	}
	const auto width = m_burstImages.front().width;
	const auto height = m_burstImages.front().height;
	std::ostringstream manifest;
	manifest.imbue(std::locale::classic());
	manifest << std::setprecision(17);
	manifest << "{\"width\":" << width << ",\"height\":" << height << ",\"count\":" << metadata.size()
			 << ",\"frames\":[";
	for (std::size_t index = 0; index < metadata.size(); ++index)
	{
		const auto& entry = metadata[index];
		if (index)
			manifest << ',';
		manifest << "{\"i\":" << index << ",\"file\":\"" << FrameFilename(index) << "\",\"frame\":" << entry.frame
				 << ",\"t_ms\":" << entry.tMs << ",\"head\":{\"yaw\":" << entry.headYaw
				 << ",\"pitch\":" << entry.headPitch << ",\"roll\":" << entry.headRoll << ",\"x\":" << entry.headX
				 << ",\"y\":" << entry.headY << ",\"z\":" << entry.headZ
				 << "},\"submitted\":{\"frame\":" << entry.proj.frame << ",\"qx\":" << entry.proj.poseQx
				 << ",\"qy\":" << entry.proj.poseQy << ",\"qz\":" << entry.proj.poseQz
				 << ",\"qw\":" << entry.proj.poseQw << ",\"x\":" << entry.proj.posX << ",\"y\":" << entry.proj.posY
				 << ",\"z\":" << entry.proj.posZ << "}}";
	}
	manifest << "]}";
	output.manifest = manifest.str();
	output.statusFilename = "burst_done.json";
	output.status = "{\"count\":" + std::to_string(metadata.size()) + ",\"width\":" + std::to_string(width) +
					",\"height\":" + std::to_string(height) + "}";
	output.images.swap(m_burstImages);
	try
	{
		// Only CPU data crosses this lock order. The file worker never enters
		// capture state; rejection restores the exact retained byte owners.
		if (!m_files->PublishImages(output))
		{
			output.images.swap(m_burstImages);
			return false;
		}
	}
	catch (...)
	{
		output.images.swap(m_burstImages);
		throw;
	}
	m_burstMetadata.clear();
	m_burstBytes = 0;
	m_burstActive = false;
	return true;
}
void WindowsLegacyCapture::BurstOnFrame(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
										std::size_t stride, BurstFrameMeta metadata)
{
	bool flush;
	{
		std::lock_guard lock(m_mutex);
		if (!m_burstActive)
			return;
		flush = m_burstImages.size() >= static_cast<std::size_t>(m_burstTotal) ||
				(!m_burstImages.empty() &&
				 (width != m_burstImages.front().width || height != m_burstImages.front().height)) ||
				std::uint64_t(width) * height > (MaxCaptureBytes - m_burstBytes) / 4;
		if (!flush)
		{
			auto image = CopyPixels(pixels, width, height, stride, true, FrameFilename(m_burstImages.size()));
			m_burstBytes += image.bgra.size();
			m_burstImages.push_back(std::move(image));
			m_burstMetadata.push_back(metadata);
			flush = m_burstImages.size() == static_cast<std::size_t>(m_burstTotal);
		}
	}
	if (flush)
		FlushBurst();
}
void WindowsLegacyCapture::PublishStatus(FrameStatus status)
{
	{
		std::lock_guard lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		if (m_lastStatus != std::chrono::steady_clock::time_point{} &&
			now - m_lastStatus < std::chrono::milliseconds(500))
			return;
		m_lastStatus = now;
	}
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << "{\"timestamp\":\"" << detail::LocalTimestamp() << "\",\"frame_count\":" << status.frame
		   << ",\"preview_width\":" << status.width << ",\"preview_height\":" << status.height << ",\"format\":\""
		   << status.format << "\",\"session_state\":\"" << status.state
		   << "\",\"target_fps\":90,\"frame_time_ms\":11.1,"
		   << "\"head_tracking\":{\"position\":{\"x\":" << status.x << ",\"y\":" << status.y << ",\"z\":" << status.z
		   << "},\"yaw\":" << status.yaw << ",\"pitch\":" << status.pitch << ",\"roll\":" << status.roll << "}}";
	m_files->Publish(WindowsDocument::RuntimeStatus, output.str());
}
void WindowsLegacyCapture::Close()
{
	while (!FlushBurst())
	{
		std::size_t bytes;
		{
			std::lock_guard lock(m_mutex);
			bytes = m_burstBytes;
		}
		if (!m_files->WaitForImageCapacity(bytes))
			return;
	}
}
} // namespace mcp
