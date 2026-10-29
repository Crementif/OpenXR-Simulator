#include "app/services/ImageExportTask.h"
#include "Pch.h"
#include "app/services/ImageExport.h"
#include "app/services/Utf8Path.h"
#include "control/ImageInspection.h"
#include "graphics/ColorPixels.h"

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using namespace panel;
ImageExportWriter::ImageExportWriter()
	: m_worker(
		  [this]
		  {
			  Run();
		  })
{
}
ImageExportWriter::~ImageExportWriter()
{
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	m_worker.join();
}
bool ImageExportWriter::Save(std::string path, std::vector<std::byte> bytes, ImageExportMetadata metadata)
{
	if (path.empty() || path.find('\0') != std::string::npos || bytes.size() > 512ULL * 1024 * 1024)
		throw std::invalid_argument("Invalid explicit export destination or byte capacity");
	// Native path conversion can allocate or fail; finish it before admission.
	auto destination = Utf8Path(path);
	const auto extension = destination.extension();
	const bool image = extension == ".png" || extension == ".jpg" || extension == ".jpeg";
	if (image)
	{
		const auto pixelBytes = ColorPixelBytes(metadata.format);
		// Division avoids overflow from untrusted dimensions before any file is opened.
		if (!metadata.width || !metadata.height || !metadata.slices || !metadata.session || !metadata.generation ||
			!metadata.frame || !pixelBytes || bytes.size() % pixelBytes ||
			bytes.size() / pixelBytes / metadata.width / metadata.height != metadata.slices ||
			static_cast<std::uint64_t>(metadata.width) * metadata.height * metadata.slices != bytes.size() / pixelBytes)
			throw std::invalid_argument(
				"Image export requires a supported color image, exact dimensions and frame identity");
	}
	else if (extension == ".mp4")
	{
		if (bytes.size() < 12 || std::memcmp(bytes.data() + 4, "ftyp", 4) != 0)
			throw std::invalid_argument("The export is not an MP4 video");
	}
	else if (extension != ".rgba")
		throw std::invalid_argument("Use .png or .jpg for an image, .rgba for raw pixels or .mp4 for video");
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_busy || m_result)
		return false;
	m_job.emplace(Job{std::move(path), std::move(destination), std::move(bytes), metadata, image});
	m_busy = true;
	m_result.reset();
	m_changed.notify_all();
	return true;
}
bool ImageExportWriter::Busy() const
{
	std::lock_guard lock(m_mutex);
	return m_busy || m_result.has_value();
}
std::optional<ImageExportWriteResult> ImageExportWriter::TakeResult()
{
	std::lock_guard lock(m_mutex);
	return std::exchange(m_result, {});
}
void ImageExportWriter::Run() noexcept
{
	for (;;)
	{
		std::optional<Job> job;
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock,
						   [&]
						   {
							   return m_stopping || m_job.has_value();
						   });
			if (!m_job)
				return;
			job = std::move(m_job);
			m_job.reset();
		}
		auto status = ImageExportWriteResult::Status::Failed;
		std::string error;
		try
		{
			if (job->image)
			{
				ImageEncodingPermit permit;
				const auto& image = job->metadata;
				const auto height = static_cast<std::uint64_t>(image.height) * image.slices;
				if (height > FileImageLimits.dimension)
					throw std::invalid_argument("Image height exceeds the encoding limit");
				auto encoded = EncodeImage(job->bytes, image.format, image.width, static_cast<std::uint32_t>(height),
					job->destination.extension() == ".png", 85, FileImageLimits);
				SaveExportFile(job->destination, encoded.bytes, true);
			}
			else
				SaveExportFile(job->destination, job->bytes, true);
			status = ImageExportWriteResult::Status::Saved;
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
		}
		catch (...)
		{
			error = "Export failed";
		}
		std::lock_guard lock(m_mutex);
		m_result.emplace(ImageExportWriteResult{status, std::move(job->path), std::move(error)});
		m_busy = false;
	}
}
ImageExportTask::ImageExportTask(RuntimePanelRequests& requests) : m_requests(requests) {}
bool ImageExportTask::Busy() const
{
	return m_requested || m_id || m_writer.Busy();
}
const std::string& ImageExportTask::Detail() const noexcept
{
	return m_detail;
}
void ImageExportTask::ResetEpoch()
{
	if (m_requested || m_id)
	{
		m_detail = "Export interrupted by disconnect";
		m_succeeded = false;
	}
	m_id = m_offset = m_size = 0;
	m_requested = m_ready = false;
	m_bytes.clear();
	m_metadata = {};
}
bool ImageExportTask::Request(std::uint64_t reader, std::string path)
{
	if (!reader || Busy())
		return false;
	m_video = false;
	m_succeeded = false;
	m_path = std::move(path);
	if (m_path.empty())
		throw std::invalid_argument("Image export destination is empty");
	m_requested = m_requests.Send(wire::OpCode::StartExport, PanelWork::Export,
								  {wire::Field::U64(tag::ImageReaderId, reader, true)}) != 0;
	return m_requested;
}
bool ImageExportTask::RequestVideo(std::uint64_t session, std::uint64_t generation, std::uint64_t first,
								   std::uint64_t last, std::string path)
{
	if (Busy())
		return false;
	if (Utf8Path(path).extension() != ".mp4")
		throw std::invalid_argument("Use an .mp4 destination");
	m_video = true;
	m_succeeded = false;
	m_path = std::move(path);
	m_requested =
		m_requests.Send(
			wire::OpCode::StartVideoExport, PanelWork::Export,
			{wire::Field::U64(tag::Session, session, true), wire::Field::U64(tag::Generation, generation, true),
			 wire::Field::U64(tag::FirstFrame, first, true), wire::Field::U64(tag::LastFrame, last, true)}) != 0;
	return m_requested;
}
void ImageExportTask::ReleaseRemote()
{
	if (m_id)
		m_requests.Release(wire::OpCode::ReleaseExport, tag::ExportId, std::exchange(m_id, 0));
	m_requested = false;
}
void ImageExportTask::Abort(std::string detail)
{
	ReleaseRemote();
	m_bytes.clear();
	m_detail = std::move(detail);
}
void ImageExportTask::Failed(const PanelTask& task, std::string detail)
{
	if (task.kind == PanelWork::Export || task.kind == PanelWork::ExportInfo || task.kind == PanelWork::ExportRead)
		Abort(std::move(detail));
}
bool ImageExportTask::Handle(const PanelTask& task, const wire::Message& message)
{
	if (task.kind != PanelWork::Export && task.kind != PanelWork::ExportInfo && task.kind != PanelWork::ExportRead)
		return false;
	try
	{
		if (task.kind == PanelWork::Export)
		{
			m_id = U64(message, tag::ExportId);
			m_size = U64(message, tag::TotalBytes);
			m_offset = 0;
			m_ready = false;
			m_metadata = {};
			if (const auto description = message.Find(tag::ImageDescriptor))
			{
				const auto image = wire::DecodeImageDescription(*description);
				const auto target = Nested(message.Find(tag::Target));
				m_metadata = {U64(message, tag::Session),
							  U64(message, tag::Generation),
							  Frame(message),
							  U64(message, tag::CaptureId),
							  image.width,
							  image.height,
							  image.arrayLayers,
							  image.format,
							  U32(target, tag::CaptureKind),
							  U32(target, tag::Layer),
							  U32(target, tag::View)};
			}
			if (!m_id || (!m_video && !m_size) || m_size > 512ULL * 1024 * 1024 ||
				(m_video && Text(message, tag::MediaType) != "video/mp4"))
				throw std::runtime_error("Runtime export exceeds the exact image byte capacity");
			m_bytes.clear();
			m_bytes.reserve(static_cast<std::size_t>(m_size));
			m_detail = "Export requested";
		}
		else if (task.id != m_id)
			return true;
		else if (task.kind == PanelWork::ExportInfo)
		{
			const auto phase = Text(message, tag::Phase);
			if (phase == "complete")
			{
				const auto size = U64(message, tag::TotalBytes);
				if (!size || size > 512ULL * 1024 * 1024 || (m_size && size != m_size))
					throw std::runtime_error("Invalid export size");
				m_size = size;
				m_bytes.reserve(static_cast<std::size_t>(m_size));
				m_ready = true;
			}
			else if (phase == "failed")
				Abort(Text(message, tag::Detail).empty() ? "Export failed" : Text(message, tag::Detail));
			else if (phase != "pending")
				throw std::runtime_error("Runtime export phase is unsupported");
		}
		else
		{
			const auto bytes = message.Find(tag::Bytes);
			if (!bytes || U64(message, tag::Offset) != m_offset || bytes->value.size() != U64(message, tag::Length) ||
				m_offset > m_size || bytes->value.size() > m_size - m_offset)
				throw std::runtime_error("Export chunk differs from the requested image range");
			const auto next = m_offset + bytes->value.size();
			if (Boolean(message, tag::End) != (next == m_size) || (bytes->value.empty() && next != m_size))
				throw std::runtime_error("Export ended early or failed to make progress");
			m_bytes.insert(m_bytes.end(), bytes->value.begin(), bytes->value.end());
			m_offset = next;
			if (next == m_size)
			{
				if (!m_writer.Save(m_path, std::move(m_bytes), m_metadata))
					throw std::runtime_error("Explicit image file worker is already occupied");
				ReleaseRemote();
				m_detail = "Saving file";
			}
		}
	}
	catch (...)
	{
		ReleaseRemote();
		m_bytes.clear();
		throw;
	}
	return true;
}
void ImageExportTask::Poll()
{
	if (auto result = m_writer.TakeResult())
	{
		switch (result->status)
		{
		case ImageExportWriteResult::Status::Saved:
			m_succeeded = true;
			m_detail = "Saved " + result->path;
			break;
		case ImageExportWriteResult::Status::Failed:
			m_detail = result->error;
			break;
		}
	}
	if (m_requests.Snapshot().state == RuntimeConnectionState::Connected && m_id &&
		!m_requests.Has(PanelWork::ExportInfo) && !m_requests.Has(PanelWork::ExportRead))
	{
		if (m_ready)
			m_requests.Send(wire::OpCode::ReadExport, PanelWork::ExportRead,
				{wire::Field::U64(tag::ExportId, m_id, true), wire::Field::U64(tag::Offset, m_offset, true),
				 wire::Field::U64(tag::Length, wire::MaxPayload - 4096, true)}, m_id);
		else
			m_requests.Send(wire::OpCode::ExportInfo, PanelWork::ExportInfo,
				{wire::Field::U64(tag::ExportId, m_id, true)}, m_id);
	}
}
} // namespace simxr::app
