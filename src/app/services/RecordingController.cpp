#include "Pch.h"
#include "app/services/RecordingController.h"
#include "app/services/Utf8Path.h"
#include "app/WorkerConnection.h"
#include "app/platform/FileStorage.h"
#include "control/ControlSchema.h"

#include <charconv>
#include <fstream>
#include <stdexcept>

namespace simxr::app
{
namespace wire = control;
static std::vector<wire::Field> SessionFields(const RecordingTarget& target)
{
	return {wire::Field::U64(wire::tag::Session, target.session, true),
			wire::Field::U64(wire::tag::Generation, target.generation, true),
			wire::Field::U32(wire::tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Manual))};
}
static void CheckRecordingResponse(const wire::Message& response)
{
	const auto error = response.Find(wire::tag::Error);
	if (!error || !error->AsU32())
		throw std::runtime_error("Recording response omitted its result");
	if (*error->AsU32())
	{
		const auto detail = response.Find(wire::tag::Detail);
		throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
															: "Recording request failed");
	}
}
wire::Message RecordingController::Read(WorkerConnection& connection, const Job& job, std::uint64_t offset)
{
	auto fields = SessionFields(job.target);
	fields.push_back(wire::Field::U64(wire::tag::Offset, offset));
	fields.push_back(wire::Field::U64(wire::tag::Length, 16));
	auto response = connection.Exchange(wire::OpCode::ReadRecording, std::move(fields));
	CheckRecordingResponse(response);
	return response;
}
RecordingSnapshot RecordingController::Execute(WorkerConnection& connection, const Job& job)
{
	RecordingSnapshot result;
	if (job.action == RecordingAction::Save)
	{
		automation::RecordingCollection collection;
		while (!collection.Complete())
			collection.Append(automation::DecodeRecordingPage(Read(connection, job, collection.Offset())));
		const auto script = automation::EditableRig(collection.Recording());
		result.savedSeed = collection.Recording().seed;
		result.savedPath = job.path;
		result.page = std::make_shared<const automation::InputRecording>(collection.Recording());
		const auto destination = Utf8Path(job.path);
		const auto temporary = CreatePrivateTemporaryFile(destination);
		try
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output)
				throw std::runtime_error("Cannot open recording temporary file");
			output.write(script.data(), static_cast<std::streamsize>(script.size()));
			if (!output)
				throw std::runtime_error("Cannot write recording temporary file");
			output.close();
			if (!output)
				throw std::runtime_error("Cannot close recording temporary file");
			CommitTemporaryFile(temporary, destination);
		}
		catch (...)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			throw;
		}
		return result;
	}
	wire::Message response;
	if (job.action == RecordingAction::Read)
		response = Read(connection, job, job.offset);
	else
	{
		const auto opcode =
			job.action == RecordingAction::Start ? wire::OpCode::StartRecording : wire::OpCode::StopRecording;
		response = connection.Exchange(opcode, SessionFields(job.target));
		CheckRecordingResponse(response);
	}
	auto page = automation::DecodeRecordingPage(response);
	result.offset = page.offset;
	result.page = std::make_shared<const automation::InputRecording>(std::move(page.recording));
	return result;
}
void RecordingController::Run()
{
	std::uint64_t bound = 0;
	std::shared_ptr<WorkerConnection> connection;
	for (;;)
	{
		std::optional<Job> job;
		bool rebind = false;
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock,
						   [&]
						   {
							   return m_stopping || m_pending || bound != m_revision;
						   });
			if (m_stopping)
			{
				m_current.reset();
				return;
			}
			if (bound != m_revision)
			{
				bound = m_revision;
				m_current.reset();
				rebind = true;
			}
			job = std::move(m_pending);
			m_pending.reset();
		}
		if (rebind)
			connection.reset();
		if (!job)
			continue;
		RecordingSnapshot result;
		try
		{
			if (!connection)
				connection = std::make_shared<WorkerConnection>(
					job->target.endpoint, "simxr_companion_recording",
					static_cast<std::uint64_t>(wire::Capability::CommandRecording));
			{
				std::lock_guard lock(m_mutex);
				if (m_stopping || job->revision != m_revision)
					connection->Stop();
				m_current = connection;
			}
			result = Execute(*connection, *job);
		}
		catch (const std::exception& error)
		{
			result.error = error.what();
			if (connection && connection->Failed())
			{
				{
					std::lock_guard lock(m_mutex);
					m_current.reset();
				}
				connection.reset();
			}
		}
		catch (...)
		{
			result.error = "Unknown recording worker failure";
		}

		{
			std::lock_guard lock(m_mutex);
			if (job->revision == m_revision)
			{
				if (!result.page)
				{
					result.page = m_snapshot.page;
					result.offset = m_snapshot.offset;
				}
				if (result.savedPath.empty() && !(job->action == RecordingAction::Start && result.error.empty()))
				{
					result.savedPath = std::move(m_snapshot.savedPath);
					result.savedSeed = m_snapshot.savedSeed;
				}
				m_snapshot = std::move(result);
			}
			m_snapshot.busy = false;
		}
	}
}
RecordingController::~RecordingController()
{
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
		if (m_current)
			m_current->Stop();
	}
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}
void RecordingController::SetTarget(RecordingTarget target)
{
	{
		std::lock_guard lock(m_mutex);
		if (m_target == target)
			return;
		m_target = std::move(target);
		++m_revision;
		const auto busy = m_snapshot.busy;
		m_snapshot = {};
		m_snapshot.busy = busy;
		if (m_current)
			m_current->Stop();
	}
	m_changed.notify_all();
}
bool RecordingController::Request(RecordingAction action, std::uint64_t offset, std::string path)
{
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping || m_snapshot.busy || m_target.endpoint.empty() || !m_target.session || !m_target.generation)
			return false;
		if (offset > 1024 ||
			(action == RecordingAction::Save &&
			 (path.empty() || path.size() > 2048 || path.find('\0') != std::string::npos)))
			throw std::invalid_argument("Invalid recording page or destination");
		Job job{action, m_target, m_revision, offset, std::move(path)};
		if (!m_worker.joinable())
			m_worker = std::thread(
				[this]
				{
					Run();
				});
		m_pending = std::move(job);
		m_snapshot.busy = true;
		m_snapshot.error.clear();
	}
	m_changed.notify_all();
	return true;
}
RecordingSnapshot RecordingController::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}
automation::SavedRecording OpenSavedRecording(const std::string& path)
{
	if (path.empty() || path.find('\0') != std::string::npos)
		throw std::invalid_argument("Invalid recording path");
	std::error_code error;
	const auto size = std::filesystem::file_size(Utf8Path(path), error);
	if (error || size == 0 || size > control::MaxPayload)
		throw std::runtime_error("Recording must be a readable file of 1–1048576 bytes");
	std::ifstream input(Utf8Path(path), std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open saved recording");
	std::string bytes(static_cast<std::size_t>(size), '\0');
	if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
		input.peek() != std::char_traits<char>::eof() || input.bad())
		throw std::runtime_error("Cannot read complete saved recording");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close saved recording");
	const std::string_view script(bytes);
	const auto firstLine = script.find('\n');
	if (firstLine == std::string_view::npos)
		throw std::runtime_error("Saved recording omitted its seed");
	const auto signature = script.substr(0, firstLine);
	auto seedLine = script.substr(firstLine + 1);
	seedLine = seedLine.substr(0, seedLine.find('\n'));
	if (!seedLine.empty() && seedLine.back() == '\r')
		seedLine.remove_suffix(1);
	constexpr std::string_view prefix = "-- Launch simxr_runner with --seed ";
	if ((!signature.starts_with("-- OpenXR Simulator recorded inputs, schema 1.") &&
		 !signature.starts_with("-- OpenXR Simulator recorded inputs, schema 2.")) ||
		!seedLine.starts_with(prefix))
		throw std::runtime_error("This is not a saved simulator recording. Open other Luau scripts in Rigs.");
	const auto text = std::string_view(seedLine).substr(prefix.size());
	std::uint64_t seed = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seed);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
		throw std::runtime_error("The recording's seed is invalid");
	return {path, seed};
}
RecordingController::RecordingController() = default;
} // namespace simxr::app
