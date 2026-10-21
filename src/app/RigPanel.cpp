#include "app/RigPanel.h"
#include "app/RigLaunchAttachment.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimeEndpoints.h"
#include "app/RuntimePanels.h"
#include "app/Utf8Path.h"
#include "app/WorkerConnection.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "automation/RigRunner.h"
#include "control/ControlSchema.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
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
	std::array<char, 2048> script{}, report{};
	std::uint64_t seed = 0;
	int deadlineSeconds = 30, memoryMiB = 64;
	std::string error;
#ifdef SIMXR_COMPANION_AUTOMATION
	std::unique_ptr<automation::RigRunner> runner;
	std::shared_ptr<WorkerConnection> transport;
	RuntimeConnection discovery;
	std::string boundEndpoint;
	std::uint64_t boundSession = 0, boundGeneration = 0;
	struct Launch
	{
		std::string source, name;
		RigLaunchAttachment attachment;
		Launch(RuntimeConnection& discovery, const LaunchProfile& profile) : attachment(discovery, profile) {}
	};
	std::unique_ptr<Launch> launch;
#endif
	Impl()
	{
		std::snprintf(report.data(), report.size(), "%s", "rig-report.json");
	}
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
		automation::RigOptions options;
		options.session = {session};
		options.generation = generation;
		options.seed = seed;
		options.source = std::move(source);
		options.name = std::move(name);
		options.deadline = std::chrono::seconds(std::clamp(deadlineSeconds, 1, 3600));
		options.memoryLimit = static_cast<std::size_t>(std::clamp(memoryMiB, 1, 512)) * 1024 * 1024;
		runner->Start(std::move(options));
		boundEndpoint = endpoint;
		boundSession = session;
		boundGeneration = generation;
		error.clear();
	}
	void Save(bool junit)
	{
		if (!runner)
			throw std::runtime_error("No rig report is available");
		const auto bytes = junit ? runner->JunitReport() : runner->JsonReport();
		std::ofstream output(Utf8Path(report.data()), std::ios::binary | std::ios::trunc);
		if (!output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !output.flush())
			throw std::runtime_error("Cannot write rig report destination");
	}
	void PollLaunch(RuntimePanels& runtime)
	{
		if (!launch)
			return;
		if (auto session = launch->attachment.Poll())
		{
			auto source = std::move(launch->source);
			auto name = std::move(launch->name);
			launch.reset();
			runtime.ConnectEndpoint(session->endpoint);
			Start(session->endpoint, session->session, session->generation, std::move(source), std::move(name));
		}
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
		auto pending = std::make_unique<Launch>(discovery, profile);
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
	void Observe(const LaunchSnapshot& process)
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		if (!launch)
			return;
		try
		{
			launch->attachment.Observe(process);
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
			launch.reset();
		}
#else
		(void)process;
#endif
	}
	void Draw(RuntimePanels& runtime, bool visible)
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		try
		{
			PollLaunch(runtime);
		}
		catch (const std::exception& failure)
		{
			error = failure.what();
			launch.reset();
		}
#endif
		if (!visible)
			return;
		if (!ImGui::Begin("Rigs"))
		{
			ImGui::End();
			return;
		}
#ifdef SIMXR_COMPANION_AUTOMATION
		const auto current = runtime.Snapshot();
		ImGui::InputText("Luau script", script.data(), script.size());
		ImGui::InputScalar("Seed", ImGuiDataType_U64, &seed);
		ImGui::InputInt("Deadline seconds", &deadlineSeconds);
		ImGui::InputInt("Memory MiB", &memoryMiB);
		const auto state = runner ? runner->Snapshot() : automation::RigSnapshot{};
		ImGui::BeginDisabled(!current.connected || !current.sessionAvailable || Active(state.phase) ||
							 launch != nullptr);
		if (ImGui::Button("Start selected rig"))
			try
			{
				Start(current.endpoint, current.session, current.generation, LoadScript(Utf8Path(script.data())),
					  script.data());
			}
			catch (const std::exception& failure)
			{
				error = failure.what();
			}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(!runner || !Active(state.phase));
		if (ImGui::Button("Pause rig"))
			runner->Pause();
		ImGui::SameLine();
		if (ImGui::Button("Step rig binding"))
			runner->Step();
		ImGui::SameLine();
		if (ImGui::Button("Resume rig"))
			runner->Resume();
		if (ImGui::Button("Stop and release rig inputs"))
			runner->Cancel();
		ImGui::EndDisabled();
		if (launch)
			ImGui::TextWrapped("Waiting for the explicitly launched process's new endpoint and unique running session; "
							   "existing connections are excluded.");
		ImGui::Text("Rig: %s; transactions: %llu; captures: %llu", PhaseName(state.phase),
					static_cast<unsigned long long>(state.transactions),
					static_cast<unsigned long long>(state.captures));
		if (!boundEndpoint.empty())
			ImGui::TextWrapped("Bound to %s / session %llu / run %llu", boundEndpoint.c_str(),
							   static_cast<unsigned long long>(boundSession),
							   static_cast<unsigned long long>(boundGeneration));
		if (Active(state.phase) && (current.endpoint != boundEndpoint || current.session != boundSession ||
									current.generation != boundGeneration))
			ImGui::TextWrapped("The inspector currently selects another connection or session. This rig remains bound "
							   "to its original run.");
		if (!state.error.empty())
			ImGui::TextWrapped("%s", state.error.c_str());
		for (std::size_t index = 0; index < state.checkpoints.size(); ++index)
		{
			const auto& checkpoint = state.checkpoints[index];
			ImGui::TextWrapped("%s: %s — %s", checkpoint.passed ? "PASS" : "FAIL", checkpoint.name.c_str(),
							   checkpoint.detail.c_str());
		}
		if (ImGui::CollapsingHeader("Script output"))
			for (const auto& line : state.output)
				ImGui::TextUnformatted(line.c_str());
		ImGui::InputText("Report path", report.data(), report.size());
		ImGui::BeginDisabled(!runner);
		if (ImGui::Button("Save JSON report"))
			try
			{
				Save(false);
			}
			catch (const std::exception& failure)
			{
				error = failure.what();
			}
		ImGui::SameLine();
		if (ImGui::Button("Save JUnit report"))
			try
			{
				Save(true);
			}
			catch (const std::exception& failure)
			{
				error = failure.what();
			}
		ImGui::EndDisabled();
		ImGui::TextWrapped("Rig pause/step controls script binding boundaries. XR frame admission is controlled "
						   "separately through simxr.pause/step/resume in the script.");
#else
		(void)runtime;
		ImGui::TextWrapped(
			"This build has no Luau automation support. Build with SIMXR_BUILD_AUTOMATION to enable scripts.");
#endif
		if (!error.empty())
			ImGui::TextWrapped("%s", error.c_str());
		ImGui::End();
	}
};
RigPanel::RigPanel() : m_impl(std::make_unique<Impl>()) {}
RigPanel::~RigPanel() = default;
void RigPanel::Draw(RuntimePanels& runtime, bool visible)
{
	m_impl->Draw(runtime, visible);
}
void RigPanel::ReplayRecording(RuntimePanels& runtime, const std::string& path, std::uint64_t seed)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	try
	{
		const auto selected = runtime.Snapshot();
		auto source = LoadScript(Utf8Path(path));
		if (path.size() >= m_impl->script.size())
			throw std::invalid_argument("Recorded rig path exceeds the path editor's capacity");
		m_impl->seed = seed;
		m_impl->Start(runtime.Endpoint(), selected.session, selected.generation, std::move(source), path);
		std::snprintf(m_impl->script.data(), m_impl->script.size(), "%s", path.c_str());
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
void RigPanel::ObserveLaunch(const LaunchSnapshot& process)
{
	m_impl->Observe(process);
}
} // namespace simxr::app
