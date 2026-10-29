#include "app/services/RangeExportTask.h"
#include "app/UiPch.h"
#include "app/services/ImageExport.h"
#include "app/services/RuntimeSession.h"
#include "app/services/Utf8Path.h"

namespace simxr::app
{
static void WriteRangeMetadata(const std::vector<HistoryFrame>& frames, const std::filesystem::path& destination)
{
	SaveExportFile(destination, [&](std::ostream& output)
	{
		output.imbue(std::locale::classic());
		output << "epoch,session,run,frame,run_seconds,display_interval_ms,snapshot,preview,dropped\n";
		for (const auto& frame : frames)
			output << frame.epoch << ',' << frame.identity.session << ',' << frame.identity.run << ','
				   << frame.identity.frame << ',' << frame.runSeconds << ',' << frame.frameMilliseconds << ','
				   << bool(frame.flags & HistorySnapshot) << ',' << bool(frame.flags & HistoryPreview) << ','
				   << bool(frame.flags & HistoryDropped) << '\n';
	}, true);
}

RangeExportTask::RangeExportTask(RuntimeSession& session) : m_session(session) {}
RangeExportTask::Protection::Protection(PreviewImages& preview, std::span<const std::uint64_t> images)
	: m_preview(preview)
{
	m_preview.ProtectExports(images);
}
RangeExportTask::Protection::~Protection()
{
	m_preview.ProtectExports({});
}
bool RangeExportTask::Busy() const
{
	return m_csv.valid() || m_protection.has_value();
}
void RangeExportTask::Finish(std::string result)
{
	m_images = std::move(result);
	m_queue.clear();
	m_requested = false;
	m_protection.reset();
}
bool RangeExportTask::Start(FrameIdentity first, FrameIdentity last, std::uint64_t epoch, const std::string& path)
{
	if (Busy() || m_session.Exports().Busy())
		return false;
	m_metadata.clear();
	m_images.clear();
	try
	{
		if (path.empty() || path.find('\0') != std::string::npos)
			throw std::invalid_argument("Choose a frame metadata destination");
		const auto destination = Utf8Path(path);
		const auto& history = m_session.History();
		const auto begin = history.Find(first, epoch), end = history.Find(last, epoch);
		if (!begin || !end || *end < *begin || first.session != last.session || first.run != last.run)
			throw std::invalid_argument("Choose saved frames from one run.");
		std::vector<HistoryFrame> frames;
		frames.reserve(*end - *begin + 1);
		for (auto index = *begin; index <= *end; ++index)
			frames.push_back(history.At(index));
		const auto prefix = PathText(std::filesystem::path(destination).replace_extension());
		std::vector<std::uint64_t> images;
		m_queue.clear();
		for (const auto& image : m_session.Preview().Images())
			if (!image.live && image.ready && !image.released && image.reader && image.scope.epoch == epoch &&
				image.scope.epoch == m_session.Epoch() && image.scope.session == first.session &&
				image.scope.generation == first.run && image.frame >= first.frame && image.frame <= last.frame)
			{
				images.push_back(image.id);
				m_queue.push_back({image.reader, prefix + "-frame-" + std::to_string(image.frame) + "-image-" +
													 std::to_string(image.id) + ".png"});
			}
		m_path = path;
		m_epoch = m_session.Epoch();
		m_next = 0;
		m_requested = false;
		if (m_queue.empty())
			m_images = "No snapshots kept in this range";
		else
			m_protection.emplace(m_session.Preview(), images);
		m_csv = std::async(std::launch::async,
						   [frames = std::move(frames), destination]
						   {
							   WriteRangeMetadata(frames, destination);
						   });
		return true;
	}
	catch (const std::exception& error)
	{
		m_metadata = error.what();
		Finish("Snapshot export did not start");
		return false;
	}
}
void RangeExportTask::Poll()
{
	if (m_protection && (m_epoch != m_session.Epoch() || !m_session.Connected()))
		Finish("Snapshot export stopped: connection changed");
	if (m_csv.valid())
	{
		if (m_csv.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;
		try
		{
			m_csv.get();
			m_metadata = "Saved frame metadata: " + m_path;
		}
		catch (const std::exception& error)
		{
			m_metadata = "Could not save frame metadata: " + std::string(error.what());
			Finish("Snapshot export stopped: metadata could not be saved");
		}
	}
	if (!m_protection)
		return;
	auto& exports = m_session.Exports();
	if (exports.Busy())
		return;
	if (m_requested)
	{
		if (!exports.Succeeded())
		{
			Finish(exports.Detail());
			return;
		}
		++m_next;
		m_requested = false;
	}
	if (m_next == m_queue.size())
	{
		Finish("Saved " + std::to_string(m_next) + " snapshots");
		return;
	}
	const auto& [reader, file] = m_queue[m_next];
	try
	{
		m_requested = exports.Request(reader, file);
		if (!m_requested)
			Finish("Snapshot export could not start");
	}
	catch (const std::exception& error)
	{
		Finish("Snapshot export failed: " + std::string(error.what()));
	}
}
} // namespace simxr::app
