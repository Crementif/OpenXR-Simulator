#pragma once

#include "control/legacy/WindowsLegacyChannel.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mcp
{
struct FrameStatus
{
	std::uint64_t frame = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::string format;
	std::string state;
	float yaw = 0;
	float pitch = 0;
	float roll = 0;
	float x = 0;
	float y = 1.7f;
	float z = 0;
};
class WindowsLegacyCapture
{
  public:
	explicit WindowsLegacyCapture(std::shared_ptr<WindowsLegacyChannel> files);
	void SetFrame(std::uint64_t frame);
	void RequestScreenshot(ScreenshotRequest request = {});
	bool ScreenshotRequested() const;
	bool FailScreenshot(std::string_view reason, std::uint64_t capturedFrame = 0);
	std::string ScreenshotLayer() const;
	bool SubmitPixels(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::size_t stride,
					  bool bgra, std::uint64_t capturedFrame = 0);
	void StoreQuad(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
				   std::uint64_t capturedFrame = 0);
	bool SubmitQuad(bool completeRequest = true, std::uint64_t capturedFrame = 0);
	bool SubmitStereo(const std::uint8_t* left, const std::uint8_t* right, std::uint32_t width, std::uint32_t height);
	void RecordProjection(ProjLogEntry projection);
	ProjLogEntry LastProjection() const;
	void DumpProjection();
	bool BeginBurst(int frames);
	bool BurstActive() const;
	void BurstOnFrame(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::size_t stride,
					  BurstFrameMeta metadata);
	void PublishStatus(FrameStatus status);
	bool FlushBurst();
	void Close();

  private:
	static BitmapOutput CopyPixels(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
								   std::size_t stride, bool bgra, std::string filename);
	std::shared_ptr<WindowsLegacyChannel> m_files;
	mutable std::mutex m_mutex;
	ScreenshotRequest m_request;
	bool m_requested = false;
	std::uint64_t m_requestVersion = 0;
	std::uint64_t m_requestFrame = 0;
	std::uint64_t m_frame = 0;
	BitmapOutput m_quad;
	std::uint64_t m_quadFrame = 0;
	std::array<ProjLogEntry, 64> m_projection{};
	std::size_t m_projectionHead = 0;
	std::size_t m_projectionCount = 0;
	bool m_burstActive = false;
	int m_burstTotal = 0;
	std::size_t m_burstBytes = 0;
	std::vector<BitmapOutput> m_burstImages;
	std::vector<BurstFrameMeta> m_burstMetadata;
	std::chrono::steady_clock::time_point m_lastStatus;
};
} // namespace mcp
