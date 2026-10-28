#include "app/services/RecordingCapture.h"
#include "Pch.h"
#include "app/services/WorkerConnection.h"
#include "automation/RecordingArchive.h"
#include "control/ControlSchema.h"

#include <stdexcept>

namespace simxr::app
{
namespace wire = control;
static void CheckCaptureSpace(const std::filesystem::path& directory)
{
	const auto space = std::filesystem::space(directory);
	if (space.available == static_cast<std::uintmax_t>(-1) || space.available < 64 * 1024 * 1024)
		throw std::runtime_error("Recording needs at least 64 MiB of free disk space");
}
static wire::Message CaptureRequest(WorkerConnection& connection, const RecordingTarget& target, wire::OpCode code,
									std::vector<wire::Field> fields = {})
{
	fields.push_back(wire::Field::U64(wire::tag::Session, target.session, true));
	fields.push_back(wire::Field::U64(wire::tag::Generation, target.generation, true));
	auto response = connection.Exchange(code, std::move(fields));
	const auto error = response.Find(wire::tag::Error);
	if (!error || error->AsU32() != 0)
	{
		const auto detail = response.Find(wire::tag::Detail);
		throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
															: "Recording capture request failed");
	}
	return response;
}
RecordingCapture::RecordingCapture()
{
	m_worker = std::thread(&RecordingCapture::Run, this);
}
RecordingCapture::~RecordingCapture()
{
	{
		std::lock_guard lock(m_mutex);
		m_exiting = true;
		if (m_current)
			m_current->Stop();
	}
	m_changed.notify_one();
	m_worker.join();
}
void RecordingCapture::SetTarget(RecordingTarget target)
{
	{
		std::lock_guard lock(m_mutex);
		if (m_target == target)
			return;
		m_target = std::move(target);
		if (m_snapshot.busy)
		{
			m_retired = true;
			if (m_current)
				m_current->Stop();
		}
	}
	m_changed.notify_one();
}
bool RecordingCapture::Start(std::filesystem::path directory)
{
	if (directory.empty() ||
		directory.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		throw std::invalid_argument("Recording directory is empty or contains NUL");
	{
		std::lock_guard lock(m_mutex);
		if (m_exiting || m_snapshot.busy || m_target.endpoint.empty() || !m_target.session || !m_target.generation)
			return false;
		m_snapshot = {};
		m_snapshot.phase = RecordingCapturePhase::Starting;
		m_snapshot.busy = true;
		m_snapshot.directory = directory;
		m_pending = Job{m_target, std::move(directory)};
		m_stop = m_retired = false;
	}
	m_changed.notify_one();
	return true;
}
void RecordingCapture::Stop()
{
	{
		std::lock_guard lock(m_mutex);
		if (!m_snapshot.busy)
			return;
		m_stop = true;
		m_snapshot.phase = RecordingCapturePhase::Stopping;
	}
	m_changed.notify_one();
}
RecordingCaptureSnapshot RecordingCapture::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}
void RecordingCapture::Capture(WorkerConnection& connection, const Job& job)
{
	if (std::filesystem::exists(job.directory))
		throw std::runtime_error("Recording archive directory already exists");
	if (job.directory.has_parent_path())
		std::filesystem::create_directories(job.directory.parent_path());
	CheckCaptureSpace(job.directory.has_parent_path() ? job.directory.parent_path() : std::filesystem::path{"."});
	CaptureRequest(connection, job.target, wire::OpCode::SimulationStatus);
	std::optional<wire::Message> gameOrigin;
	if (connection.Capabilities() & static_cast<std::uint64_t>(wire::Capability::GameObservations))
		gameOrigin = CaptureRequest(connection, job.target, wire::OpCode::ReadGameObservations,
									{wire::Field::U64(wire::tag::Length, 0)});
	const auto start = CaptureRequest(connection, job.target, wire::OpCode::StartRecording,
									  {wire::Field::Bool(wire::tag::RecordingDrain, true)});
	automation::RecordingArchiveWriter archive(job.directory, start, gameOrigin);
	const auto identity = archive.State().identity;
	{
		std::lock_guard lock(m_mutex);
		m_snapshot.identity = identity;
		if (!m_stop)
			m_snapshot.phase = RecordingCapturePhase::Capturing;
	}
	std::optional<wire::Message> stopped;
	std::optional<std::uint64_t> gameEnd;
	for (;;)
	{
		bool stop;
		{
			std::lock_guard lock(m_mutex);
			if (m_exiting || m_retired)
				throw std::runtime_error("Recording target retired; saved prefix is incomplete");
			stop = m_stop;
		}
		if (stop && !stopped)
			stopped = CaptureRequest(connection, job.target, wire::OpCode::StopRecording,
									 {wire::Field::U64(wire::tag::RecordingId, identity)});
		auto response = CaptureRequest(connection, job.target, wire::OpCode::ReadRecording,
									   {wire::Field::U64(wire::tag::RecordingId, identity),
										wire::Field::U64(wire::tag::Offset, archive.State().next),
										wire::Field::U64(wire::tag::Length, 16)});
		const auto page = automation::DecodeRecordingPage(response);
		if (!page.recording.transactions.empty())
		{
			CheckCaptureSpace(job.directory);
			const auto through = archive.Append(response);
			{
				std::lock_guard lock(m_mutex);
				m_snapshot.transactions = through;
			}
			CaptureRequest(
				connection, job.target, wire::OpCode::AcknowledgeRecording,
				{wire::Field::U64(wire::tag::RecordingId, identity), wire::Field::U64(wire::tag::Offset, through)});
		}
		if (archive.State().game)
		{
			if ((stopped || !page.recording.active) && !gameEnd)
			{
				const auto boundary = CaptureRequest(connection, job.target, wire::OpCode::ReadGameObservations,
													 {wire::Field::U64(wire::tag::Length, 0)});
				gameEnd = automation::DecodeRecordedGamePage(boundary).journal.total;
			}
			const auto next = archive.State().game->next;
			const auto count = gameEnd ? std::min<std::uint64_t>(16, *gameEnd > next ? *gameEnd - next : 0) : 16;
			const auto observations =
				CaptureRequest(connection, job.target, wire::OpCode::ReadGameObservations,
							   {wire::Field::U64(wire::tag::Offset, next), wire::Field::U64(wire::tag::Length, count)});
			const auto gamePage = automation::DecodeRecordedGamePage(observations);
			if (!gamePage.journal.observations.empty() || gamePage.journal.gap)
			{
				CheckCaptureSpace(job.directory);
				archive.AppendGame(observations);
			}
		}
		if (!page.recording.active)
		{
			if (!stopped)
				stopped = CaptureRequest(connection, job.target, wire::OpCode::StopRecording,
										 {wire::Field::U64(wire::tag::RecordingId, identity)});
			if (archive.State().next == page.recording.total &&
				(!archive.State().game || (gameEnd && archive.State().game->next >= *gameEnd)))
			{
				CheckCaptureSpace(job.directory);
				if (archive.State().game)
				{
					const auto end = CaptureRequest(connection, job.target, wire::OpCode::ReadGameObservations,
													{wire::Field::U64(wire::tag::Offset, archive.State().game->next),
													 wire::Field::U64(wire::tag::Length, 0)});
					archive.AppendGame(end, "end");
				}
				archive.Finish(*stopped);
				if (!archive.State().complete)
					throw std::runtime_error("Recording or SDK evidence has gaps; saved prefix is incomplete");
				return;
			}
		}
		if (page.recording.transactions.empty())
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait_for(lock, std::chrono::milliseconds{5},
							   [&]
							   {
								   return m_exiting || m_retired || m_stop;
							   });
		}
	}
}
void RecordingCapture::Run()
{
	for (;;)
	{
		Job job;
		std::shared_ptr<WorkerConnection> connection;
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock,
						   [&]
						   {
							   return m_exiting || m_pending.has_value();
						   });
			if (m_exiting)
				return;
			job = std::move(*m_pending);
			m_pending.reset();
		}
		std::string error;
		try
		{
			connection =
				std::make_shared<WorkerConnection>(job.target.endpoint, "simxr_companion_capture",
												   static_cast<std::uint64_t>(wire::Capability::CommandRecording) |
													   static_cast<std::uint64_t>(wire::Capability::RecordingDrain));
			{
				std::lock_guard lock(m_mutex);
				m_current = connection;
				if (m_exiting || m_retired)
					connection->Stop();
			}
			Capture(*connection, job);
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
		}
		catch (...)
		{
			error = "Unknown recording capture failure";
		}
		{
			std::lock_guard lock(m_mutex);
			m_current.reset();
		}
		connection.reset();
		{
			std::lock_guard lock(m_mutex);
			m_snapshot.error = std::move(error);
			m_snapshot.phase =
				m_snapshot.error.empty() ? RecordingCapturePhase::Complete : RecordingCapturePhase::Incomplete;
			m_snapshot.busy = false;
		}
	}
}
} // namespace simxr::app
