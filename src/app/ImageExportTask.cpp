#include "app/ImageExportTask.h"
#include "app/Utf8Path.h"

#include <cstdio>
#include <utility>

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
bool ImageExportWriter::Save(std::string path, std::vector<std::byte> bytes)
{
	if (path.empty() || path.find('\0') != std::string::npos || bytes.size() > 512ULL * 1024 * 1024)
		throw std::invalid_argument("Invalid explicit export destination or byte capacity");
	// Native path conversion can allocate or fail; finish it before admission.
	auto destination = Utf8Path(path);
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_busy)
		return false;
	m_job.emplace(Job{std::move(path), std::move(destination), std::move(bytes)});
	m_busy = true;
	m_result.reset();
	m_changed.notify_all();
	return true;
}
bool ImageExportWriter::Busy() const
{
	std::lock_guard lock(m_mutex);
	return m_busy;
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
		// Every result owns the already-allocated destination string. Native file
		// operations and publication therefore need no worker-thread allocation.
		auto status = ImageExportWriteResult::Status::OpenFailed;
#ifdef _WIN32
		auto file = ::_wfopen(job->destination.c_str(), L"wb");
#else
		auto file = std::fopen(job->destination.c_str(), "wb");
#endif
		if (file)
		{
			const auto written = std::fwrite(job->bytes.data(), 1, job->bytes.size(), file);
			const auto closed = std::fclose(file);
			status = written == job->bytes.size() && !closed ? ImageExportWriteResult::Status::Saved
															 : ImageExportWriteResult::Status::WriteFailed;
		}
		std::lock_guard lock(m_mutex);
		m_result.emplace(ImageExportWriteResult{status, std::move(job->path)});
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
	m_id = m_offset = m_size = 0;
	m_requested = false;
	m_bytes.clear();
}
bool ImageExportTask::Request(std::uint64_t reader, std::string path)
{
	if (!reader || Busy())
		return false;
	m_path = std::move(path);
	if (m_path.empty())
		throw std::invalid_argument("Image export destination is empty");
	m_requested = m_requests.Send(wire::OpCode::StartExport, PanelWork::Export,
								  {wire::Field::U64(tag::ImageReaderId, reader, true)}) != 0;
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
			if (!m_id || !m_size || m_size > 512ULL * 1024 * 1024)
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
				m_requests.Send(wire::OpCode::ReadExport, PanelWork::ExportRead,
								{wire::Field::U64(tag::ExportId, m_id, true),
								 wire::Field::U64(tag::Offset, m_offset, true),
								 wire::Field::U64(tag::Length, wire::MaxPayload - 4096, true)},
								m_id);
			else if (phase == "failed")
				Abort("GPU export failed");
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
				if (!m_writer.Save(m_path, std::move(m_bytes)))
					throw std::runtime_error("Explicit image file worker is already occupied");
				ReleaseRemote();
				m_detail = "Writing exact RGBA bytes";
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
			m_detail = "Saved exact RGBA bytes to " + result->path;
			break;
		case ImageExportWriteResult::Status::OpenFailed:
			m_detail = "Cannot open image export destination";
			break;
		case ImageExportWriteResult::Status::WriteFailed:
			m_detail = "Image file write failed";
			break;
		}
	}
	if (m_requests.Snapshot().state == RuntimeConnectionState::Connected && m_id &&
		!m_requests.Has(PanelWork::ExportInfo) && !m_requests.Has(PanelWork::ExportRead))
		m_requests.Send(wire::OpCode::ExportInfo, PanelWork::ExportInfo, {wire::Field::U64(tag::ExportId, m_id, true)},
						m_id);
}
} // namespace simxr::app
