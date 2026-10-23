#include "app/services/RigController.h"
#include "Pch.h"
#include "app/platform/FileStorage.h"
#include "app/services/Utf8Path.h"
#include "automation/RigReport.h"
#include "control/ControlSchema.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace simxr::app
{
namespace wire = control;
static std::string LoadScript(const std::filesystem::path& path)
{
	if (path.empty() ||
		path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		throw std::invalid_argument("Rig script path is empty or contains a NUL character");
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || !size || size > wire::MaxPayload)
		throw std::runtime_error("Rig script must be a readable file of 1–1048576 bytes");
	std::string source(static_cast<std::size_t>(size), '\0');
	std::ifstream input(path, std::ios::binary);
	if (!input.read(source.data(), static_cast<std::streamsize>(source.size())) ||
		input.peek() != std::char_traits<char>::eof() || input.bad())
		throw std::runtime_error("Cannot read the complete rig script");
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close rig script");
	return source;
}
static void WriteReport(const std::filesystem::path& destination, const std::string& bytes)
{
	if (bytes.size() > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
		throw std::runtime_error("Rig report exceeds the file stream capacity");
	const auto temporary = CreatePrivateTemporaryFile(destination);
	try
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("Cannot open rig report temporary file");
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		if (!output)
			throw std::runtime_error("Cannot write rig report");
		output.close();
		if (!output)
			throw std::runtime_error("Cannot close rig report");
		CommitTemporaryFile(temporary, destination);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}
RigController::RigController()
	: m_runner(
		  [this](auto opcode, auto fields)
		  {
			  return m_transport->Exchange(opcode, std::move(fields));
		  })
{
}
RigController::~RigController()
{
	if (m_transport)
		m_transport->Stop();
	m_runner.Cancel();
	m_runner.Join();
	m_transport.reset();
}
void RigController::Start(RigTarget target, std::string source, std::string name, const ScriptOptions& options,
						  bool replay)
{
	if (IsActive())
		throw std::runtime_error("Stop the current rig before starting another");
	if (m_runId == std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("Rig run identity exhausted");
	if (target.endpoint.empty() || !target.session || !target.generation)
		throw std::runtime_error("Select a running session on a connected runtime first");
	automation::RigOptions run;
	run.session = {target.session};
	run.generation = target.generation;
	run.seed = options.seed;
	run.source = std::move(source);
	run.name = name;
	run.deadline = std::chrono::seconds(std::clamp(options.deadlineSeconds, 1, 3600));
	run.memoryLimit = static_cast<std::size_t>(std::clamp(options.memoryMiB, 1, 512)) * 1024 * 1024;
	m_runner.Join();
	m_transport.emplace(target.endpoint, "simxr_companion_rig",
						static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
	m_runner.Start(std::move(run));
	m_script = std::move(name);
	m_target = std::move(target);
	m_recordingReplay = replay;
	++m_runId;
	m_error.clear();
}
void RigController::RunScript(RigTarget target, const std::string& path, const ScriptOptions& options)
{
	try
	{
		Start(std::move(target), LoadScript(Utf8Path(path)), path, options, false);
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
void RigController::ReplayRecording(RigTarget target, const std::string& path, std::uint64_t seed)
{
	try
	{
		Start(std::move(target), LoadScript(Utf8Path(path)), path, ScriptOptions{seed}, true);
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
void RigController::Pause()
{
	if (IsActive())
		m_runner.Pause();
}
void RigController::Step()
{
	if (IsActive())
		m_runner.Step();
}
void RigController::Resume()
{
	if (IsActive())
		m_runner.Resume();
}
void RigController::Cancel()
{
	if (IsActive())
		m_runner.Cancel();
}
const std::string& RigController::Script() const noexcept
{
	return m_script;
}
bool RigController::ReplayingTake() const noexcept
{
	return m_recordingReplay;
}
bool RigController::WaitingForLaunch() const noexcept
{
	return m_launch.has_value();
}
const std::string& RigController::Error() const noexcept
{
	return m_error;
}
std::uint64_t RigController::RunId() const noexcept
{
	return m_runId;
}
automation::RigSnapshot RigController::Snapshot() const
{
	return m_runner.Snapshot();
}
bool RigController::MatchesTarget(const RigTarget& target) const noexcept
{
	return m_runId && m_target == target;
}
bool RigController::IsActive() const
{
	const auto phase = m_runner.Snapshot().phase;
	return phase == automation::RigPhase::Running || phase == automation::RigPhase::Paused;
}
std::string RigController::SaveReport(const std::string& path, bool junit)
{
	try
	{
		if (ReportBusy())
			throw std::runtime_error("A report is already being saved");
		if (path.empty() || path.find('\0') != std::string::npos)
			throw std::invalid_argument("Rig report destination is empty or contains a NUL character");
		auto snapshot = Snapshot();
		if (snapshot.phase == automation::RigPhase::Idle || snapshot.phase == automation::RigPhase::Running ||
			snapshot.phase == automation::RigPhase::Paused)
			throw std::runtime_error("Finish the current rig before saving its report");
		m_report = std::async(
			std::launch::async,
			[path = std::string(path), destination = Utf8Path(path), snapshot = std::move(snapshot), junit]() mutable
			{
				RigReportResult result{std::move(path), {}};
				try
				{
					WriteReport(destination, junit ? automation::SerializeRigJunit(snapshot)
												   : automation::SerializeRigJson(snapshot));
				}
				catch (...)
				{
					result.error = std::current_exception();
				}
				return result;
			});
		return {};
	}
	catch (const std::exception& error)
	{
		return error.what();
	}
}
bool RigController::ReportBusy() const noexcept
{
	return m_report.valid();
}
std::optional<RigReportResult> RigController::TakeReportResult()
{
	if (!m_report.valid() || m_report.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return {};
	return m_report.get();
}
void RigController::PrepareLaunch(const LaunchProfile& profile)
{
	m_launch.reset();
	if (profile.rig.empty())
		return;
	if (IsActive())
		throw std::runtime_error("Stop the current rig before launching another rig profile");
	auto file = Utf8Path(profile.rig);
	if (!file.is_absolute())
	{
		if (profile.workingDirectory.empty() || !Utf8Path(profile.workingDirectory).is_absolute())
			throw std::runtime_error("A relative rig path requires an absolute profile working directory");
		file = Utf8Path(profile.workingDirectory) / file;
	}
	m_launch = Launch{LoadScript(file), PathText(file)};
	m_error.clear();
}
void RigController::AttachLaunch(const LaunchedSession& session)
{
	if (!m_launch)
		return;
	auto pending = std::move(*m_launch);
	m_launch.reset();
	Start({session.endpoint, session.session, session.generation}, std::move(pending.source), std::move(pending.name),
		  {}, false);
}
} // namespace simxr::app
