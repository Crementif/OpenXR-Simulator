#include "control/VideoExport.h"
#include "Pch.h"
#include "media/Mp4Video.h"
#include <chrono>
#include <stdexcept>

namespace simxr::control
{
VideoExport::VideoExport(std::vector<PreviewClipFrame> frames)
	: m_worker(
		  [this, frames = std::move(frames)]() mutable
		  {
			  Run(std::move(frames));
		  })
{
}
VideoExport::~VideoExport()
{
	m_stop = true;
	m_worker.join();
}
std::string VideoExport::Error() const
{
	return m_phase.load() == CompletionStatus::Failed ? m_error : std::string{};
}
std::uint64_t VideoExport::ByteSize() const noexcept
{
	return m_phase.load() == CompletionStatus::Complete ? m_bytes.size() : 0;
}
std::span<const std::byte> VideoExport::Read()
{
	if (m_phase.load() != CompletionStatus::Complete)
		throw std::runtime_error("The video is not ready.");
	return m_bytes;
}
void VideoExport::Run(std::vector<PreviewClipFrame> frames) noexcept
{
	try
	{
		std::shared_ptr<const ArchivedFrame> reference, black;
		std::uint64_t encodedBytes = 0;
		for (auto& frame : frames)
		{
			if (m_stop)
				throw std::runtime_error("Video export cancelled.");
			if (frame.clear)
				continue;
			if (!frame.archive)
			{
				if (!frame.output || !frame.output->CanArchive())
					throw std::runtime_error("Vulkan H.264 encoding is unavailable for this clip.");
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (frame.output->PollCompletion() == CompletionStatus::Pending)
				{
					if (m_stop || std::chrono::steady_clock::now() >= deadline)
						throw std::runtime_error("A video frame did not finish rendering.");
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				if (frame.output->PollCompletion() != CompletionStatus::Complete)
					throw std::runtime_error("A video frame failed to render.");
				frame.archive = frame.output->Archive();
			}
			if (!frame.archive || frame.archive->Video().bytes.empty())
				throw std::runtime_error("This frame has no H.264 image.");
			encodedBytes += frame.archive->Video().bytes.size();
			if (encodedBytes > (480ull << 20))
				throw std::length_error("The video exceeds 480 MiB. Save a shorter range.");
			if (!reference)
				reference = frame.archive;
			frame.output.reset();
		}
		if (!reference)
			throw std::runtime_error("This clip has no rendered images.");
		std::vector<VideoSample> samples;
		samples.reserve(frames.size());
		std::int64_t remainder = 500000000;
		for (std::size_t i = 0; i < frames.size(); ++i)
		{
			if (m_stop)
				throw std::runtime_error("Video export cancelled.");
			auto& frame = frames[i];
			if (frame.clear)
			{
				if (!black)
					black = reference->Black();
				frame.archive = black;
			}
			if (!frame.archive)
				throw std::runtime_error("Cannot encode an empty frame in this clip.");
			if (frame.time.value.count() < 0 || (i + 1 < frames.size() && frames[i + 1].time.value <= frame.time.value))
				throw std::runtime_error("This clip has invalid frame timing.");
			const auto duration = i + 1 < frames.size() ? frames[i + 1].time.value.count() - frame.time.value.count()
														: frame.period.count();
			if (duration <= 0 || duration > std::int64_t{UINT32_MAX} * 1000000000 / 90000)
				throw std::runtime_error("This clip has invalid frame timing.");
			const auto scaled = duration * 90000 + remainder;
			const auto ticks = scaled / 1000000000;
			remainder = scaled % 1000000000;
			if (!ticks || ticks > UINT32_MAX)
				throw std::runtime_error("The clip frame interval is unsupported.");
			samples.push_back({frame.archive->Video(), static_cast<std::uint32_t>(ticks)});
		}
		m_bytes = WriteMp4Video(samples);
		m_phase = CompletionStatus::Complete;
	}
	catch (const std::exception& error)
	{
		try
		{
			m_error = error.what();
		}
		catch (...)
		{
		}
		m_phase = CompletionStatus::Failed;
	}
	catch (...)
	{
		m_phase = CompletionStatus::Failed;
	}
}
} // namespace simxr::control
