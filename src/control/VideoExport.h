#pragma once
#include "control/ImageInspection.h"
#include "presentation/FramePreviewHistory.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace simxr::control
{
class VideoExport final : public ImageExport
{
  public:
	explicit VideoExport(std::vector<PreviewClipFrame> frames);
	~VideoExport();
	CompletionStatus PollCompletion() const noexcept override
	{
		return m_phase.load();
	}
	ImageDescription Description() const override
	{
		return {};
	}
	std::string_view MediaType() const noexcept override
	{
		return "video/mp4";
	}
	std::string Error() const override;
	std::uint64_t ByteSize() const noexcept override;
	std::span<const std::byte> Read() override;

  private:
	void Run(std::vector<PreviewClipFrame> frames) noexcept;
	std::atomic<CompletionStatus> m_phase{CompletionStatus::Pending};
	std::atomic<bool> m_stop{false};
	std::vector<std::byte> m_bytes;
	std::string m_error;
	std::thread m_worker;
};
} // namespace simxr::control
