#include "app/WorkspacePanels.h"
#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"

#include <imgui.h>
#include <stdexcept>

namespace simxr::app
{
WorkspacePanels::WorkspacePanels(ProcessSupervisor& processes, bool enabled, std::string endpoint)
	: m_enabled(enabled), m_launcher(processes)
{
	if (m_enabled)
	{
		try
		{
			m_path = CompanionSettingsPath();
			m_settings = LoadSettings(m_path);
		}
		catch (const std::exception& failure)
		{
			m_notices.Push({"Settings unavailable", failure.what()});
		}
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
	for (std::size_t index = 0; index < m_settings.profiles.size(); ++index)
		if (m_settings.profiles[index].name == m_settings.activeProfile)
			m_profiles.Edit(m_settings, static_cast<int>(index));
}

WorkspacePanels::~WorkspacePanels() = default;

void WorkspacePanels::Save()
{
	if (!m_enabled)
		return;
	m_dirty = true;
	try
	{
		if (m_path.empty())
			m_path = CompanionSettingsPath();
		SaveSettings(m_path, m_settings);
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
	try
	{
		auto pending = std::make_unique<LaunchAttachment>(m_launchDiscovery, profile);
#ifdef SIMXR_COMPANION_AUTOMATION
		m_rigs.PrepareLaunch(profile);
#else
		if (!profile.rig.empty())
			throw std::runtime_error("This build does not support launch rigs");
#endif
		if (m_launcher.Launch(profile))
		{
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

void WorkspacePanels::PollLaunch(RuntimePanels& runtime)
{
	if (!m_attachment)
		return;
	try
	{
		m_attachment->Observe(m_launcher.Snapshot());
		if (auto session = m_attachment->Poll())
		{
			m_attachment.reset();
			runtime.ConnectEndpoint(session->endpoint);
			m_launchedEndpoint = session->endpoint;
			m_launchedName = m_launchName;
#ifdef SIMXR_COMPANION_AUTOMATION
			m_rigs.AttachLaunch(*session);
#endif
		}
	}
	catch (const std::exception& failure)
	{
		m_launchError = failure.what();
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

void WorkspacePanels::Draw(RuntimePanels& runtime)
{
	PollLaunch(runtime);
	runtime.BeginFrame();
	m_timeline.Observe(runtime.View());
#ifdef SIMXR_COMPANION_AUTOMATION
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
#endif
	DrawShell(runtime);
#ifdef SIMXR_COMPANION_AUTOMATION
	if (std::exchange(m_shell.timeline.togglePlayback, false) && matchingReplay)
		m_shell.timeline.playbackPaused ? m_rigs.Resume() : m_rigs.Pause();
	if (std::exchange(m_shell.timeline.stopReplay, false) && matchingReplay)
		m_rigs.Cancel();
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
	if (m_preferences.Draw(m_settings, m_showSettings, m_dirty, runtime.PreviewBytes(), runtime.PreviewBudget(),
						   runtime.PreviewRamBytes(), runtime.PreviewRamBudget(), runtime.PreviewError()))
	{
		m_settings.endpoint = runtime.Endpoint();
		Save();
	}
	bool changed = false;
	if (m_profiles.Draw(m_settings, m_launcher.Snapshot(), m_showLaunch, changed))
		Launch(m_profiles.Profile());
	if (changed)
	{
		m_dirty = true;
		Save();
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
			if (m_path.empty())
				m_path = CompanionSettingsPath();
			SaveSettings(m_path, m_settings);
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
