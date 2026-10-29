#pragma once

#include "app/dialogs/LaunchProfilesDialog.h"
#include "app/dialogs/PreferencesDialog.h"
#include "app/mcp/McpUiState.h"
#include "app/services/CompanionSettings.h"
#include "app/services/LaunchAttachment.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/RuntimeConnection.h"
#include "app/services/SessionView.h"
#include "app/services/UpdateChecker.h"
#include "app/services/ShellState.h"
#include "app/widgets/AppPicker.h"
#include "app/widgets/Notices.h"
#include "app/widgets/TimelinePanel.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/pages/Library.h"
#include "app/services/RigController.h"
#include "automation/Recording.h"
#endif

#include <chrono>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <memory>
#include <optional>
#include <string>

namespace simxr::app
{
class RuntimePanels;
class ProcessSupervisor;
struct RuntimeView;

struct SharedWorkspaceSettings
{
	bool enabled = false, dirty = false, styleCaptured = false;
	std::filesystem::path path, layoutPath;
	CompanionSettings settings;
	std::optional<CompanionSettings> savedSettings;
	McpUiState mcp;
	UpdateUiState update;
	std::unique_ptr<UpdateChecker> updateChecker;
	float appliedScale = 1;
	ImGuiStyle originalStyle;
};

class WorkspacePanels
{
public:
	// Disabled for finite QA/proof runs, including both reading and writing config.
	WorkspacePanels(ProcessSupervisor& processes, bool enabled, std::string endpoint);
	// Each application keeps its own workspace state, while preferences are shared.
	WorkspacePanels(ProcessSupervisor& processes, WorkspacePanels& existing);
	~WorkspacePanels();

	void BeginFrame(RuntimePanels& runtime);
	// Advances launch and automation work even while this workspace is hidden.
	void Poll(RuntimePanels& runtime);
	void Draw(RuntimePanels& runtime);
	void Finish();

	void SetTabBar(std::function<void()> draw);
	void SetOpenHandlers(std::function<bool(const RunningApp&)> connect,
						 std::function<bool(const LaunchProfile&)> launch);
	void SetEndpointAdmission(std::function<bool(const std::string&)> available);
	void SetScale(float scale);

	void OpenAppPicker();
	void ConnectApp(RuntimePanels& runtime, const RunningApp& app);
	void LaunchApp(const LaunchProfile& profile);
	bool RequestApplicationClose(const RuntimePanels& runtime);
	bool RequestApplicationRestart(RuntimePanels& runtime, std::uint64_t expectedProcess);
	bool ConnectionMatchesLaunch(const RuntimePanels& runtime) const;

	std::string InitialEndpoint() const;
	const std::string& LaunchError() const noexcept;
	LaunchSnapshot Process() const;
	std::string LaunchControlEndpoint() const;
	std::string LaunchDataDirectory() const;
	std::string ApplicationName(const RuntimePanels& runtime) const;
	bool McpEnabled() const;
	McpUiState& McpState();

	const ShellState& State() const;
	void Select(Selection selection);
	void SetPreviewExpanded(bool expanded);
	void ShowLibrary(bool open);

	bool HasActiveAutomation() const;
	bool HasPendingFiles() const;
	bool ClosingWorkDone() const;
	void BeginClose();
	void StopAutomation();
#ifdef SIMXR_COMPANION_AUTOMATION
	RigController& Rigs() noexcept
	{
		return m_rigs;
	}

	const RigController& Rigs() const noexcept
	{
		return m_rigs;
	}
#endif

private:
	struct DividerDrag
	{
		ImGuiID id;
		float request, travel;
	};

	struct PendingRestart
	{
		LaunchRestartRequest request;
		RuntimeFrameRun run;
		std::string endpoint;
		std::chrono::steady_clock::time_point deadline;
		bool queued = false;
	};

	WorkspacePanels(ProcessSupervisor& processes, std::shared_ptr<SharedWorkspaceSettings> shared);
	void Save();
	void SaveLayout();
	void Remember(LaunchProfile launch);
	void Launch(const LaunchProfile& profile);
	void PollLaunch(RuntimePanels& runtime);
	bool Restart(RuntimePanels& runtime, LaunchRestartRequest request);
	void CancelRestart();
	void ToggleLibrary();
	void ShowInspector();
	void FoldTimeline();
	void FoldInspector();
	void PickTab(Selection picked, bool overlay);
	void StoreCrop(const std::optional<ImVec4>& crop);
	bool Divider(const char* id, ShellRect strip, bool vertical, float& value, float minimum, float maximum,
				 float defaultValue, bool folded, float foldBelow);
	void DrawShell(RuntimePanels& runtime);
	void DrawToolbar(RuntimePanels& runtime);
	void DrawAppPicker(RuntimePanels& runtime, const RuntimeView& view, float maximumWidth, bool compact = false);
	void DrawHistory(RuntimePanels& runtime);
	void DrawInspectorPanel(RuntimePanels& runtime, bool rail);
	void DrawLibrary(RuntimePanels& runtime);
	void DrawInspector(RuntimePanels& runtime);
#ifdef SIMXR_COMPANION_AUTOMATION
	RuntimeFrameRun ReplayTarget(const RuntimePanels& runtime) const;
	void QueueScript(RuntimePanels& runtime, ScriptRun run);
#endif

	std::shared_ptr<SharedWorkspaceSettings> m_shared;
	std::function<void()> m_tabBar;
	std::function<bool(const RunningApp&)> m_openRunning;
	std::function<bool(const LaunchProfile&)> m_openLaunch;
	std::function<bool(const std::string&)> m_endpointAvailable;
	bool& m_enabled;
	bool& m_dirty;
	bool m_showLaunch = false, m_showSettings = false;
	bool m_previewExpanded = false, m_showConnection = false, m_showError = false;
	AppPicker m_picker;
	ShellState m_shell;
	bool m_drawer = true;
	std::string m_launchName, m_launchedName, m_launchedEndpoint;
	std::uint64_t m_attachedProcess = 0;
	std::string m_connectedName, m_connectedEndpoint;
	std::string m_workspaceProfile;
	std::optional<std::string> m_cropProfile;
	bool m_wasOverlay = false;
	std::optional<DividerDrag> m_dividerDrag;
	std::filesystem::path& m_path;
	std::filesystem::path& m_layoutPath;
	CompanionSettings& m_settings;
	ProcessLauncher m_launcher;
	TimelinePanel m_timeline;
	RuntimeConnection m_launchDiscovery;
	std::unique_ptr<LaunchAttachment> m_attachment;
	std::optional<LaunchProfile> m_launchedProfile;
	std::optional<PendingRestart> m_restart;
#ifdef SIMXR_COMPANION_AUTOMATION
	RigController m_rigs;
	Library m_library;

	struct PendingReplay
	{
		automation::SavedRecording recording;
		std::optional<ScriptOptions> script;
		std::chrono::steady_clock::time_point deadline;
		RuntimeFrameRun target;
	};

	std::optional<PendingReplay> m_replay;
	bool m_replayFailed = false;
#endif
	PreferencesDialog m_preferences;
	LaunchProfilesDialog m_profiles;
	Notices m_notices;
	std::string m_launchError;
	float& m_appliedScale;
	ImGuiStyle& m_originalStyle;
	bool& m_styleCaptured;
};
} // namespace simxr::app
