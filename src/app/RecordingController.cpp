#include "app/RecordingController.h"
#include "app/services/Utf8Path.h"
#include "app/WorkerConnection.h"
#include "control/ControlSchema.h"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace simxr::app
{
namespace
{
namespace wire = control;
std::vector<wire::Field> SessionFields(const RecordingTarget& target)
{
	return {wire::Field::U64(wire::tag::Session, target.session, true),
			wire::Field::U64(wire::tag::Generation, target.generation, true),
			wire::Field::U32(wire::tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Manual))};
}
void CheckRecordingResponse(const wire::Message& response)
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
} // namespace
class RecordingController::State
{
  public:
	~State();
	void SetTarget(RecordingTarget target);
	bool Request(RecordingAction action, std::uint64_t offset, std::string path);
	RecordingSnapshot Snapshot() const;

  private:
	struct Job
	{
		RecordingAction action;
		RecordingTarget target;
		std::uint64_t revision, offset;
		std::string path;
	};
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	bool m_stopping = false;
	std::uint64_t m_revision = 0;
	RecordingTarget m_target;
	std::optional<Job> m_pending;
	RecordingSnapshot m_snapshot;
	std::shared_ptr<WorkerConnection> m_current;

	wire::Message Read(WorkerConnection& connection, const Job& job, std::uint64_t offset)
	{
		auto fields = SessionFields(job.target);
		fields.push_back(wire::Field::U64(wire::tag::Offset, offset));
		fields.push_back(wire::Field::U64(wire::tag::Length, 16));
		auto response = connection.Exchange(wire::OpCode::ReadRecording, std::move(fields));
		CheckRecordingResponse(response);
		return response;
	}
	RecordingSnapshot Execute(WorkerConnection& connection, const Job& job)
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
			const auto temporary = Utf8Path(
				job.path + ".writing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			try
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				output.write(script.data(), static_cast<std::streamsize>(script.size()));
				output.close();
				if (!output)
					throw std::runtime_error("Cannot write recording destination");
#ifdef _WIN32
				if (!MoveFileExW(temporary.c_str(), Utf8Path(job.path).c_str(),
								 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					throw std::runtime_error("Cannot replace recording destination");
#else
				std::filesystem::rename(temporary, Utf8Path(job.path));
#endif
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
	void Run()
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
};
RecordingController::State::~State()
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
void RecordingController::State::SetTarget(RecordingTarget target)
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
bool RecordingController::State::Request(RecordingAction action, std::uint64_t offset, std::string path)
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
RecordingSnapshot RecordingController::State::Snapshot() const
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
	std::string signature, seedLine;
	std::getline(input, signature);
	std::getline(input, seedLine);
	if (!seedLine.empty() && seedLine.back() == '\r')
		seedLine.pop_back();
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
RecordingController::RecordingController() : m_state(std::make_unique<State>()) {}
RecordingController::~RecordingController() = default;
void RecordingController::SetTarget(RecordingTarget target)
{
	m_state->SetTarget(std::move(target));
}
bool RecordingController::Request(RecordingAction action, std::uint64_t offset, std::string path)
{
	return m_state->Request(action, offset, std::move(path));
}
RecordingSnapshot RecordingController::Snapshot() const
{
	return m_state->Snapshot();
}
} // namespace simxr::app
