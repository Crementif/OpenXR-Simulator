#include "app/WorkspacePanels.h"
#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include "simxr/Version.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_misc.h>
#include <imgui.h>
#include <stdexcept>

namespace simxr::app
{
static LaunchProfile ResolveProfile(const LaunchProfile& profile)
{
	if (!profile.useBundledRuntime)
		return profile;
	const auto* base = SDL_GetBasePath();
	if (!base)
		throw std::runtime_error("Cannot locate the OpenXR Simulator installation");
	return ResolveLaunchProfile(profile, Utf8Path(base));
}

WorkspacePanels::WorkspacePanels(ProcessSupervisor& processes, bool enabled, std::string endpoint)
	: WorkspacePanels(processes, std::make_shared<SharedWorkspaceSettings>())
{
	m_enabled = enabled;
	if (m_enabled)
	{
		try
		{
			m_path = CompanionSettingsPath();
			m_settings = LoadSettings(m_path);
			m_shared->savedSettings = m_settings;
		}
		catch (const std::exception& failure)
		{
			m_notices.Push({"Settings unavailable", failure.what()});
		}
		m_shared->updateChecker = std::make_unique<UpdateChecker>(simxr::ReleaseRepository, simxr::Version);
		if (m_settings.autoUpdateCheck)
			m_shared->updateChecker->Start();
		m_shared->update.result = m_shared->updateChecker->State();
	}
	ImGui::GetIO().IniFilename = nullptr;
	if (m_enabled && !m_path.empty())
	{
		m_layoutPath = m_path.parent_path() / "companion-v2.ini";
		try
		{
			const auto layout = LoadWindowLayout(m_layoutPath);
			if (!layout.empty())
				ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
		}
		catch (const std::exception& failure)
		{
			m_notices.Push({"Layout unavailable", failure.what()});
		}
	}
	if (!endpoint.empty())
		m_settings.endpoint = endpoint;
	m_workspaceProfile = m_settings.activeProfile;
	for (std::size_t index = 0; index < m_settings.profiles.size(); ++index)
		if (m_settings.profiles[index].name == m_settings.activeProfile)
			m_profiles.Edit(m_settings, static_cast<int>(index));
}

WorkspacePanels::WorkspacePanels(ProcessSupervisor& processes, WorkspacePanels& existing)
	: WorkspacePanels(processes, existing.m_shared)
{
	m_workspaceProfile = m_settings.activeProfile;
	for (std::size_t index = 0; index < m_settings.profiles.size(); ++index)
		if (m_settings.profiles[index].name == m_settings.activeProfile)
			m_profiles.Edit(m_settings, static_cast<int>(index));
}

WorkspacePanels::WorkspacePanels(ProcessSupervisor& processes, std::shared_ptr<SharedWorkspaceSettings> shared)
	: m_shared(std::move(shared)), m_enabled(m_shared->enabled), m_dirty(m_shared->dirty), m_path(m_shared->path),
	  m_layoutPath(m_shared->layoutPath), m_settings(m_shared->settings), m_launcher(processes),
	  m_appliedScale(m_shared->appliedScale), m_originalStyle(m_shared->originalStyle),
	  m_styleCaptured(m_shared->styleCaptured)
{
}

WorkspacePanels::~WorkspacePanels() = default;

void WorkspacePanels::Save()
{
	if (!m_enabled)
		return;
	m_dirty = true;
	try
	{
		if (!m_shared->savedSettings)
			throw std::runtime_error("Settings could not be loaded. Reopen this window after resolving the error.");
		m_shared->savedSettings = SaveSettingsIfUnchanged(m_path, *m_shared->savedSettings, m_settings);
		m_dirty = false;
	}
	catch (const std::exception& failure)
	{
		m_notices.Push({"Settings not saved", failure.what()});
	}
}

void WorkspacePanels::SaveLayout()
{
	if (m_layoutPath.empty())
	{
		if (m_path.empty())
			m_path = CompanionSettingsPath();
		m_layoutPath = m_path.parent_path() / "companion-v2.ini";
	}
	std::size_t size = 0;
	const char* layout = ImGui::SaveIniSettingsToMemory(&size);
	SaveWindowLayout(m_layoutPath, {layout, size});
}

void WorkspacePanels::BeginFrame(RuntimePanels&)
{
	if (m_shared->updateChecker && m_shared->updateChecker->Poll())
	{
		m_shared->update.result = m_shared->updateChecker->State();
		const auto& result = m_shared->update.result;
		if (result.status == UpdateStatus::Available)
			m_notices.Push({"Update available", "Version " + result.version, "View",
							[url = result.releaseUrl]
							{
								if (!SDL_OpenURL(url.c_str()))
									throw std::runtime_error(SDL_GetError());
							}});
	}
	if (ImGui::GetIO().AppFocusLost)
		m_dividerDrag.reset();
	if (std::exchange(ImGui::GetIO().WantSaveIniSettings, false) && m_enabled)
	{
		try
		{
			SaveLayout();
		}
		catch (const std::exception& failure)
		{
			m_notices.Push({"Layout not saved", failure.what()});
		}
	}
	if (!m_styleCaptured)
	{
		m_originalStyle = ImGui::GetStyle();
		m_styleCaptured = true;
	}
	if (m_appliedScale != m_settings.scale)
	{
		ImGui::GetStyle() = m_originalStyle;
		ImGui::GetStyle().ScaleAllSizes(m_settings.scale);
		ImGui::GetStyle().FontScaleMain = m_settings.scale;
		m_appliedScale = m_settings.scale;
	}
}

static std::int64_t Now()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

void WorkspacePanels::Remember(LaunchProfile launch)
{
	try
	{
		RememberRecentApp(m_settings, std::move(launch), Now());
		Save();
	}
	catch (const std::exception& failure)
	{
		m_notices.Push({"Recent app not saved", failure.what()});
	}
}

void WorkspacePanels::Launch(const LaunchProfile& profile)
{
	if (m_openLaunch && m_openLaunch(profile))
		return;
	try
	{
		const auto resolved = ResolveProfile(profile);
		auto pending = std::make_unique<LaunchAttachment>(m_launchDiscovery, resolved);
#ifdef SIMXR_COMPANION_AUTOMATION
		m_rigs.PrepareLaunch(profile);
#else
		if (!profile.rig.empty())
			throw std::runtime_error("This build does not support launch rigs");
#endif
		if (m_launcher.Launch(resolved))
		{
			m_launchedProfile = profile;
			m_workspaceProfile = profile.name;
			m_launchName = profile.name.empty() ? PathText(Utf8Path(profile.executable).stem()) : profile.name;
			m_attachment = std::move(pending);
			m_launchError.clear();
			Remember(profile);
		}
	}
	catch (const std::exception& failure)
	{
		m_launchError = failure.what();
#ifdef SIMXR_COMPANION_AUTOMATION
		m_rigs.PrepareLaunch({});
#endif
	}
}

void WorkspacePanels::SetTabBar(std::function<void()> draw)
{
	m_tabBar = std::move(draw);
}
void WorkspacePanels::SetOpenHandlers(std::function<bool(const RunningApp&)> connect,
									 std::function<bool(const LaunchProfile&)> launch)
{
	m_openRunning = std::move(connect);
	m_openLaunch = std::move(launch);
}
void WorkspacePanels::SetEndpointAdmission(std::function<bool(const std::string&)> available)
{
	m_endpointAvailable = std::move(available);
}
void WorkspacePanels::OpenAppPicker()
{
	m_shell.openAppPicker = true;
}
void WorkspacePanels::LaunchApp(const LaunchProfile& profile)
{
	Launch(profile);
}
bool WorkspacePanels::RequestApplicationClose(const RuntimePanels& runtime)
{
	if (m_restart || (!runtime.Endpoint().empty() && runtime.Endpoint() != m_launchedEndpoint))
		return false;
	return m_launcher.RequestClose();
}
bool WorkspacePanels::RequestApplicationRestart(RuntimePanels& runtime, std::uint64_t expectedProcess)
{
	if (!m_launchedProfile)
	{
		m_launchError = "No simulator-launched profile is available for restart";
		return false;
	}
	return Restart(runtime, {*m_launchedProfile, expectedProcess});
}
bool WorkspacePanels::ConnectionMatchesLaunch(const RuntimePanels& runtime) const
{
	return m_launchedEndpoint.empty() || runtime.Endpoint() != m_launchedEndpoint || !m_attachedProcess ||
		   m_launcher.Snapshot().process == m_attachedProcess;
}
const std::string& WorkspacePanels::LaunchError() const noexcept
{
	return m_launchError;
}
LaunchSnapshot WorkspacePanels::Process() const
{
	auto process = m_launcher.Snapshot();
	process.restartPending |= m_restart.has_value();
	return process;
}
std::string WorkspacePanels::LaunchControlEndpoint() const
{
	const auto process = Process();
	if (!m_launchedProfile || process.restartPending || process.state != LaunchState::Running || !process.process)
		return {};
	return LaunchRuntimeEndpoint(*m_launchedProfile, process.process);
}
std::string WorkspacePanels::LaunchDataDirectory() const
{
	if (!m_launchedProfile)
		return {};
	const auto path = LaunchRuntimeDataDirectory(*m_launchedProfile);
	return path ? PathText(*path) : std::string{};
}
bool WorkspacePanels::McpEnabled() const
{
	return m_settings.mcpEnabled;
}
McpUiState& WorkspacePanels::McpState()
{
	return m_shared->mcp;
}
std::string WorkspacePanels::ApplicationName(const RuntimePanels& runtime) const
{
	const auto view = runtime.Snapshot();
	if (runtime.Endpoint() == m_launchedEndpoint && !m_launchedName.empty())
		return m_launchedName;
	if (runtime.Endpoint() == m_connectedEndpoint && !m_connectedName.empty())
		return m_connectedName;
	if (!view.appName.empty())
		return view.appName;
	const auto process = m_launcher.Snapshot();
	if (!process.profile.empty())
		return process.profile;
	return "Apps";
}
bool WorkspacePanels::HasActiveAutomation() const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return m_replay.has_value() || m_rigs.IsActive();
#else
	return false;
#endif
}
bool WorkspacePanels::HasPendingFiles() const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return m_library.Busy() || m_rigs.ReportBusy();
#else
	return false;
#endif
}
bool WorkspacePanels::ClosingWorkDone() const
{
	return m_picker.DiscoveryStopped() && !HasActiveAutomation() && !HasPendingFiles();
}
void WorkspacePanels::BeginClose()
{
	m_picker.StopDiscovery();
	StopAutomation();
}
void WorkspacePanels::StopAutomation()
{
	CancelRestart();
	m_attachment.reset();
#ifdef SIMXR_COMPANION_AUTOMATION
	m_replay.reset();
	m_rigs.PrepareLaunch({});
	m_rigs.Cancel();
#endif
}

void WorkspacePanels::CancelRestart()
{
	m_launcher.CancelRestart();
	if (!m_restart)
		return;
	m_restart.reset();
	m_attachment.reset();
#ifdef SIMXR_COMPANION_AUTOMATION
	m_rigs.PrepareLaunch({});
#endif
}

bool WorkspacePanels::Restart(RuntimePanels& runtime, LaunchRestartRequest request)
{
	try
	{
		const auto view = runtime.Snapshot();
		const auto process = m_launcher.Snapshot();
		if (m_restart || m_attachment || !view.connected || view.endpoint != m_launchedEndpoint ||
			process.state != LaunchState::Running || !request.process || process.process != request.process)
			throw std::runtime_error("The connected launched app changed. Review the restart again.");
		ValidateLaunchProfile(request.profile);
		m_restart = PendingRestart{std::move(request),
								   {view.epoch, view.session, view.generation},
								   view.endpoint,
								   std::chrono::steady_clock::now() + std::chrono::seconds(10)};
		runtime.DisconnectHeadset();
		m_launchError.clear();
		return true;
	}
	catch (const std::exception& failure)
	{
		m_launchError = failure.what();
		return false;
	}
}

void WorkspacePanels::PollLaunch(RuntimePanels& runtime)
{
	if (!m_attachment && !m_restart)
		return;
	try
	{
		if (m_restart)
		{
			const auto process = m_launcher.Snapshot();
			if (!m_restart->queued)
			{
				const auto view = runtime.Snapshot();
				if (!view.connected || view.endpoint != m_restart->endpoint ||
					RuntimeFrameRun{view.epoch, view.session, view.generation} != m_restart->run ||
					process.state != LaunchState::Running || process.process != m_restart->request.process)
					throw std::runtime_error("Restart cancelled because the original application changed.");
				const auto host = runtime.HeadsetStatus();
				if (host.forcedStop || host.phase == HeadsetHostPhase::Failed)
					throw std::runtime_error("Restart cancelled because the headset did not stop cleanly.");
				if (host.phase != HeadsetHostPhase::Idle && host.phase != HeadsetHostPhase::Exited)
				{
					if (std::chrono::steady_clock::now() >= m_restart->deadline)
						throw std::runtime_error("Restart cancelled while waiting for headset shutdown.");
					return;
				}
				const auto profile = ResolveProfile(m_restart->request.profile);
				auto pending = std::make_unique<LaunchAttachment>(m_launchDiscovery, profile, m_restart->endpoint);
#ifdef SIMXR_COMPANION_AUTOMATION
				m_rigs.PrepareLaunch(profile);
#else
				if (!profile.rig.empty())
					throw std::runtime_error("This build does not support launch rigs");
#endif
				if (!m_launcher.Restart(m_restart->request.process, profile))
					throw std::runtime_error("The application no longer accepts this restart request.");
				m_attachment = std::move(pending);
				m_launchName = profile.name;
				m_workspaceProfile = profile.name;
				m_restart->queued = true;
				Remember(m_restart->request.profile);
				return;
			}
			if (runtime.Endpoint() != m_restart->endpoint)
				throw std::runtime_error("Restart cancelled because the selected endpoint changed.");
			if (process.restartPending)
			{
				const auto view = runtime.Snapshot();
				if (view.connected && (view.epoch != m_restart->run.epoch ||
									   (view.sessionAvailable &&
										RuntimeFrameRun{view.epoch, view.session, view.generation} != m_restart->run)))
					throw std::runtime_error("Restart cancelled because the selected session changed.");
				return;
			}
			if (process.state == LaunchState::Running && process.process == m_restart->request.process)
				throw std::runtime_error("Application restart cancelled: " + process.detail);
			m_launchedProfile = m_restart->request.profile;
			m_restart.reset();
		}
		m_attachment->Observe(m_launcher.Snapshot());
		if (auto session = m_attachment->Poll())
		{
			m_attachment.reset();
			if (m_endpointAvailable && !m_endpointAvailable(session->endpoint))
				throw std::runtime_error("The launched application's endpoint is already open in another tab");
			runtime.ConnectEndpoint(session->endpoint, session->process);
			m_launchedEndpoint = session->endpoint;
			m_attachedProcess = session->process;
			m_launchedName = m_launchName;
#ifdef SIMXR_COMPANION_AUTOMATION
			m_rigs.AttachLaunch(*session);
#endif
		}
	}
	catch (const std::exception& failure)
	{
		m_launchError = failure.what();
		CancelRestart();
		m_attachment.reset();
#ifdef SIMXR_COMPANION_AUTOMATION
		m_rigs.PrepareLaunch({});
#endif
	}
}

#ifdef SIMXR_COMPANION_AUTOMATION
RuntimeFrameRun WorkspacePanels::ReplayTarget(const RuntimePanels& runtime) const
{
	const auto state = runtime.Snapshot();
	return {state.epoch, state.session, state.generation};
}

void WorkspacePanels::QueueScript(RuntimePanels& runtime, ScriptRun run)
{
	runtime.PrepareReplay();
	m_replay = PendingReplay{
		{run.path}, run.options, std::chrono::steady_clock::now() + std::chrono::seconds(3), ReplayTarget(runtime)};
}
#endif

void WorkspacePanels::Poll(RuntimePanels& runtime)
{
	PollLaunch(runtime);
	runtime.ConfigureSnapshots(m_settings.snapshotBudgetMiB);
	runtime.ConfigurePreviews(m_settings.previewBudgetMiB, m_settings.previewRamMiB);
	m_timeline.Observe(runtime.Snapshot());
#ifdef SIMXR_COMPANION_AUTOMATION
	m_rigs.CancelDisconnected();
	m_library.Advance(m_shell.selection, m_rigs);
	if (auto recording = runtime.TakeRecordingReplay())
	{
		runtime.PrepareReplay();
		m_replay = PendingReplay{std::move(*recording),
								 {},
								 std::chrono::steady_clock::now() + std::chrono::seconds(3),
								 ReplayTarget(runtime)};
	}
	if (m_replay && m_replay->target != ReplayTarget(runtime))
	{
		m_replay.reset();
		runtime.FinishReplayPreparation();
		m_notices.Push({"Replay target changed", "Select the intended session and replay again"});
	}
	const auto rigRuntime = runtime.Snapshot();
	const RigTarget rigTarget{rigRuntime.endpoint, rigRuntime.session, rigRuntime.generation};
	if (m_replay)
	{
		if (runtime.ReplayReady())
		{
			if (m_replay->script)
				m_rigs.RunScript(rigTarget, m_replay->recording.path, *m_replay->script);
			else
			{
				m_rigs.ReplayRecording(rigTarget, m_replay->recording.path, m_replay->recording.seed);
				m_timeline.ReplayTake(m_replay->recording.path, rigRuntime, m_rigs.RunId());
			}
			m_replay.reset();
			runtime.FinishReplayPreparation();
		}
		else if (std::chrono::steady_clock::now() >= m_replay->deadline)
		{
			m_replay.reset();
			runtime.FinishReplayPreparation();
			m_notices.Push({"Replay unavailable", "Could not release all inputs. Try again."});
		}
	}
	const bool matchingReplay = m_rigs.MatchesTarget(rigTarget);
	runtime.SetReplayActive(matchingReplay && m_rigs.IsActive());
	m_timeline.ObserveRig(rigRuntime, m_rigs.RunId(), m_rigs.Snapshot(), matchingReplay);
	m_shell.timeline.playing = matchingReplay && m_rigs.IsActive() && m_rigs.ReplayingTake();
	m_shell.timeline.playbackPaused =
		m_shell.timeline.playing && m_rigs.Snapshot().phase == automation::RigPhase::Paused;
	// A replay or script that fails says so once; its page has the details.
	const auto rig = m_rigs.Snapshot();
	const bool failed = rig.phase == automation::RigPhase::Failed || !m_rigs.Error().empty();
	if (failed && !std::exchange(m_replayFailed, true))
	{
		const bool take = m_rigs.ReplayingTake();
		m_notices.Push({take ? "Replay failed" : "Script failed", m_rigs.Error().empty() ? rig.error : m_rigs.Error(),
						"Details", [this, take, scriptPath = m_rigs.Script()]
						{
							m_shell.selection =
								take ? Selection{TakeSel{scriptPath}} : Selection{ScriptSel{scriptPath}};
							ShowInspector();
						}});
	}
	m_replayFailed = failed;
#endif
}

void WorkspacePanels::Draw(RuntimePanels& runtime)
{
	Poll(runtime);
	runtime.BeginFrame();
	DrawShell(runtime);
#ifdef SIMXR_COMPANION_AUTOMATION
	const auto rigRuntime = runtime.Snapshot();
	const bool matchingReplay = m_rigs.MatchesTarget({rigRuntime.endpoint, rigRuntime.session, rigRuntime.generation});
	if (std::exchange(m_shell.timeline.togglePlayback, false) && matchingReplay)
		m_shell.timeline.playbackPaused ? m_rigs.Resume() : m_rigs.Pause();
	if (std::exchange(m_shell.timeline.stopReplay, false) && matchingReplay)
		m_rigs.Cancel();
#endif
	if (auto saved = runtime.TakeSavedTake())
	{
		m_timeline.SavedRecording(*saved, runtime.View());
#ifdef SIMXR_COMPANION_AUTOMATION
		m_library.Refresh();
#endif
		m_shell.selection = TakeSel{std::move(*saved)};
		m_drawer = true;
	}
	if (m_showConnection)
		runtime.DrawConnection(&m_showConnection);
	if (!m_launchError.empty() || m_showError)
	{
		ImGui::OpenPopup("Error details");
		if (ImGui::BeginPopupModal("Error details"))
		{
			ImGui::TextWrapped("%s", (m_launchError.empty() ? m_shell.session.error : m_launchError).c_str());
			if (ImGui::Button("Dismiss"))
			{
				m_launchError.clear();
				m_showError = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	const bool previousMcpPreference = m_settings.mcpEnabled;
	const bool previousUpdatePreference = m_settings.autoUpdateCheck;
	const bool settingsClosed = m_preferences.Draw(m_settings, m_showSettings, m_dirty, runtime.PreviewBytes(), runtime.PreviewBudget(),
						   runtime.PreviewRamBytes(), runtime.PreviewRamBudget(), runtime.PreviewError(),
						   runtime.SnapshotChargeBytes(), PreviewTexture::TotalSnapshotChargeBytes(), &m_shared->mcp,
						   m_shared->updateChecker ? &m_shared->update : nullptr);
	if (settingsClosed || previousMcpPreference != m_settings.mcpEnabled || previousUpdatePreference != m_settings.autoUpdateCheck)
	{
		if (settingsClosed)
			m_settings.endpoint = runtime.Endpoint();
		Save();
	}
	if (m_shared->updateChecker && std::exchange(m_shared->update.checkRequested, false))
	{
		m_shared->updateChecker->Start();
		m_shared->update.result = m_shared->updateChecker->State();
	}
	bool changed = false;
	auto process = m_launcher.Snapshot();
	process.restartPending |= m_restart.has_value();
	if (m_restart && !m_restart->queued)
		process.detail = "Waiting for headset shutdown before restarting";
	const auto view = runtime.Snapshot();
	const bool canRestart = !m_restart && !m_attachment && view.connected && view.endpoint == m_launchedEndpoint;
	if (m_profiles.Draw(m_settings, process, m_showLaunch, changed, runtime.HeadsetStatus().recommendedEyes,
						canRestart))
		Launch(m_profiles.Profile());
	if (m_profiles.TakeRestartCancellation())
		CancelRestart();
	if (auto request = m_profiles.TakeRestartRequest())
		Restart(runtime, std::move(*request));
	if (changed)
	{
		if (!view.connected && process.state == LaunchState::Idle && !m_attachment)
			m_workspaceProfile = m_settings.activeProfile;
		m_dirty = true;
		Save();
	}
	if (m_profiles.TakeCopyCommandRequest() && m_enabled)
	{
		Save();
		if (!m_dirty)
		{
			try
			{
				const auto* base = SDL_GetBasePath();
				if (!base)
					throw std::runtime_error("Cannot locate the OpenXR Simulator installation");
#ifdef _WIN32
				const auto executable = Utf8Path(base) / "simxr_companion.exe";
				constexpr auto shell = LaunchCommandShell::PowerShell;
#else
				const auto executable = Utf8Path(base) / "simxr_companion";
				constexpr auto shell = LaunchCommandShell::Posix;
#endif
				const auto command = ProfileLaunchCommand(executable, m_profiles.Profile().name, shell);
				if (!SDL_SetClipboardText(command.c_str()))
					throw std::runtime_error(SDL_GetError());
				m_notices.Push({"Launch command copied", "The command opens a new simulator window."});
			}
			catch (const std::exception& failure)
			{
				m_notices.Push({"Command not copied", failure.what()});
			}
		}
	}
	runtime.DrawWindows();
	runtime.EndFrame();
}

std::string WorkspacePanels::InitialEndpoint() const
{
	return m_settings.endpoint;
}
const ShellState& WorkspacePanels::State() const
{
	return m_shell;
}
void WorkspacePanels::Select(Selection selection)
{
	m_shell.selection = std::move(selection);
	ShowInspector();
	m_wasOverlay = true;
}
void WorkspacePanels::ShowLibrary(bool open)
{
	if (m_shell.library != open)
		ToggleLibrary();
}
void WorkspacePanels::SetPreviewExpanded(bool expanded)
{
	m_previewExpanded = expanded;
}
void WorkspacePanels::SetScale(float scale)
{
	if (!(scale >= 0.75f && scale <= 2.5f))
		throw std::invalid_argument("UI scale must be between 0.75 and 2.5");
	m_settings.scale = scale;
	m_dirty = true;
}
void WorkspacePanels::Finish()
{
	if (!m_enabled)
		return;
	std::string failure;
	try
	{
		if (m_dirty)
		{
			if (!m_shared->savedSettings)
				throw std::runtime_error("Settings could not be loaded. Reopen this window after resolving the error.");
			m_shared->savedSettings = SaveSettingsIfUnchanged(m_path, *m_shared->savedSettings, m_settings);
			m_dirty = false;
		}
	}
	catch (const std::exception& error)
	{
		failure = std::string("Settings not saved: ") + error.what();
	}
	try
	{
		SaveLayout();
	}
	catch (const std::exception& error)
	{
		if (!failure.empty())
			failure += '\n';
		failure += std::string("Layout not saved: ") + error.what();
	}
	if (!failure.empty())
		throw std::runtime_error(failure);
}

} // namespace simxr::app
