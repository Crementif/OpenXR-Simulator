#include "app/RigPanel.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"
#include "app/LaunchAttachment.h"
#include "app/RuntimePanels.h"
#include "app/Utf8Path.h"
#include "app/WorkerConnection.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "automation/RigRunner.h"
#include "control/ControlSchema.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <stdexcept>
#include <string>

namespace simxr::app
{
namespace
{
namespace wire = control;
#ifdef SIMXR_COMPANION_AUTOMATION
std::string LoadScript(const std::filesystem::path& path)
{
	if (path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		throw std::invalid_argument("Rig script path contains a NUL character");
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || !size || size > wire::MaxPayload)
		throw std::runtime_error("Rig script must be a readable file of 1–1048576 bytes");
	std::string source(static_cast<std::size_t>(size), '\0');
	std::ifstream input(path, std::ios::binary);
	if (!input.read(source.data(), static_cast<std::streamsize>(source.size())))
		throw std::runtime_error("Cannot read rig script");
	return source;
}
const char* PhaseName(automation::RigPhase phase)
{
	switch (phase)
	{
	case automation::RigPhase::Idle:
		return "Idle";
	case automation::RigPhase::Running:
		return "Running";
	case automation::RigPhase::Paused:
		return "Paused";
	case automation::RigPhase::Complete:
		return "Complete";
	case automation::RigPhase::Cancelled:
		return "Cancelled";
	case automation::RigPhase::Failed:
		return "Failed";
	}
	return "Unknown";
}
bool Active(automation::RigPhase phase)
{
	return phase == automation::RigPhase::Running || phase == automation::RigPhase::Paused;
}

#endif
} // namespace
struct RigPanel::Impl
{
	std::string script;
	std::uint64_t seed = 0;
	int deadlineSeconds = 30, memoryMiB = 64;
	std::string error;
	std::uint64_t runId = 0;
	bool recordingReplay = false;
#ifdef SIMXR_COMPANION_AUTOMATION
	std::unique_ptr<automation::RigRunner> runner;
	std::shared_ptr<WorkerConnection> transport;
	std::string boundEndpoint;
	std::uint64_t boundSession = 0, boundGeneration = 0;
	struct Launch
	{
		std::string source, name;
	};
	std::unique_ptr<Launch> launch;
#endif
	~Impl()
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		if (transport)
			transport->Stop();
		if (runner)
		{
			runner->Cancel();
			runner->Join();
		}
#endif
	}
#ifdef SIMXR_COMPANION_AUTOMATION
	void Start(const std::string& endpoint, std::uint64_t session, std::uint64_t generation, std::string source,
			   std::string name)
	{
		if (runner && Active(runner->Snapshot().phase))
			throw std::runtime_error("Stop the current rig before starting another");
		if (endpoint.empty() || !session || !generation)
			throw std::runtime_error("Select a running session on a connected runtime first");
		if (runner)
			runner->Join();
		transport = std::make_shared<WorkerConnection>(
			endpoint, "simxr_companion_rig", static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
		runner = std::make_unique<automation::RigRunner>(
			[connection = transport](auto opcode, auto fields)
			{
				return connection->Exchange(opcode, std::move(fields));
			});
		auto scriptPath = name;
		automation::RigOptions options;
		options.session = {session};
		options.generation = generation;
		options.seed = seed;
		options.source = std::move(source);
		options.name = std::move(name);
		options.deadline = std::chrono::seconds(std::clamp(deadlineSeconds, 1, 3600));
		options.memoryLimit = static_cast<std::size_t>(std::clamp(memoryMiB, 1, 512)) * 1024 * 1024;
		runner->Start(std::move(options));
		script = std::move(scriptPath);
		++runId;
		boundEndpoint = endpoint;
		boundSession = session;
		boundGeneration = generation;
		error.clear();
	}
	void Save(const std::string& path, bool junit)
	{
		if (path.empty() || path.find('\0') != std::string::npos)
			throw std::invalid_argument("Rig report destination is empty or contains a NUL character");
		if (!runner)
			throw std::runtime_error("No rig report is available");
		const auto bytes = junit ? runner->JunitReport() : runner->JsonReport();
		std::ofstream output(Utf8Path(path), std::ios::binary | std::ios::trunc);
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.close();
		if (!output)
			throw std::runtime_error("Cannot write rig report destination");
	}
	void Attach(const LaunchedSession& session)
	{
		if (!launch)
			return;
		auto pending = std::move(launch);
		Start(session.endpoint, session.session, session.generation, std::move(pending->source),
			  std::move(pending->name));
	}
#endif
	void Prepare(const LaunchProfile& profile)
	{
		if (profile.rig.empty())
		{
#ifdef SIMXR_COMPANION_AUTOMATION
			launch.reset();
#endif
			return;
		}
#if defined(SIMXR_COMPANION_AUTOMATION) && (defined(__linux__) || defined(_WIN32))
		if (runner && Active(runner->Snapshot().phase))
			throw std::runtime_error("Stop the current rig before launching another rig profile");
		auto pending = std::make_unique<Launch>();
		auto file = Utf8Path(profile.rig);
		if (!file.is_absolute())
		{
			if (profile.workingDirectory.empty() || !Utf8Path(profile.workingDirectory).is_absolute())
				throw std::runtime_error("A relative rig path requires an absolute profile working directory");
			file = Utf8Path(profile.workingDirectory) / file;
		}
		pending->source = LoadScript(file);
		const auto name = file.u8string();
		pending->name.assign(name.begin(), name.end());
		launch = std::move(pending);
		this->error.clear();
#else
		throw std::runtime_error("This companion build does not provide native rig execution");
#endif
	}
};
RigPanel::RigPanel() : m_impl(std::make_unique<Impl>()) {}
RigPanel::~RigPanel() = default;
void RigPanel::RunScript(RuntimePanels& runtime, const std::string& path, const ScriptOptions& options)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	try
	{
		const auto current = runtime.Snapshot();
		m_impl->seed = options.seed;
		m_impl->deadlineSeconds = options.deadlineSeconds;
		m_impl->memoryMiB = options.memoryMiB;
		m_impl->recordingReplay = false;
		m_impl->Start(current.endpoint, current.session, current.generation, LoadScript(Utf8Path(path)), path);
	}
	catch (const std::exception& failure)
	{
		m_impl->error = failure.what();
	}
#else
	(void)runtime;
	(void)path;
	(void)options;
#endif
}
void RigPanel::Pause()
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (IsActive())
		m_impl->runner->Pause();
#endif
}
void RigPanel::Step()
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (IsActive())
		m_impl->runner->Step();
#endif
}
void RigPanel::Resume()
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (IsActive())
		m_impl->runner->Resume();
#endif
}
void RigPanel::Cancel()
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (IsActive())
		m_impl->runner->Cancel();
#endif
}
std::string RigPanel::Script() const
{
	return m_impl->script;
}
bool RigPanel::ReplayingTake() const
{
	return m_impl->recordingReplay;
}
bool RigPanel::WaitingForLaunch() const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return m_impl->launch != nullptr;
#else
	return false;
#endif
}
const std::string& RigPanel::Error() const
{
	return m_impl->error;
}
std::string RigPanel::SaveReport(const std::string& path, bool junit)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	try
	{
		m_impl->Save(path, junit);
		return {};
	}
	catch (const std::exception& failure)
	{
		return failure.what();
	}
#else
	(void)path;
	(void)junit;
	return "Scripts need a Luau-enabled build";
#endif
}
std::uint64_t RigPanel::RunId() const
{
	return m_impl->runId;
}
automation::RigSnapshot RigPanel::Snapshot() const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return m_impl->runner ? m_impl->runner->Snapshot() : automation::RigSnapshot{};
#else
	return {};
#endif
}
bool RigPanel::MatchesTarget(const RuntimeView& state) const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return state.endpoint == m_impl->boundEndpoint && state.session == m_impl->boundSession &&
		   state.generation == m_impl->boundGeneration;
#else
	(void)state;
	return false;
#endif
}
bool RigPanel::IsActive() const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return m_impl->runner && simxr::app::Active(m_impl->runner->Snapshot().phase);
#else
	return false;
#endif
}
void RigPanel::ReplayRecording(RuntimePanels& runtime, const std::string& path, std::uint64_t seed)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	m_impl->recordingReplay = true;
	try
	{
		const auto selected = runtime.Snapshot();
		auto source = LoadScript(Utf8Path(path));
		m_impl->seed = seed;
		m_impl->Start(runtime.Endpoint(), selected.session, selected.generation, std::move(source), path);
	}
	catch (const std::exception& error)
	{
		m_impl->error = error.what();
	}
#else
	(void)runtime;
	(void)path;
	(void)seed;
#endif
}
void RigPanel::PrepareLaunch(const LaunchProfile& profile)
{
	m_impl->Prepare(profile);
}
void RigPanel::AttachLaunch(const LaunchedSession& session)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	m_impl->Attach(session);
#else
	(void)session;
#endif
}
} // namespace simxr::app
