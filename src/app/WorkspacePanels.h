#pragma once

#include "app/dialogs/LaunchProfilesDialog.h"
#include "app/dialogs/PreferencesDialog.h"
#include "app/services/CompanionSettings.h"
#include "app/services/LaunchAttachment.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/RuntimeConnection.h"
#include "app/services/SessionView.h"
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
#include <imgui.h>
#include <memory>
#include <optional>
#include <string>

namespace simxr::app
{
class RuntimePanels;
class ProcessSupervisor;
struct RuntimeView;
class WorkspacePanels
{
  public:
	// Disabled for finite QA/proof runs, including both reading and writing config.
	WorkspacePanels(ProcessSupervisor& processes, bool enabled, std::string endpoint);
	~WorkspacePanels();
	std::string InitialEndpoint() const;
	void BeginFrame(RuntimePanels& runtime);
	void Draw(RuntimePanels& runtime);
	const ShellState& State() const;
	void Select(Selection selection);
	void SetPreviewExpanded(bool expanded);
	void ShowLibrary(bool open);
	void SetScale(float scale);
	void Finish();

  private:
	void Save();
	void SaveLayout();
	void Remember(LaunchProfile launch);
	void Launch(const LaunchProfile& profile);
	void PollLaunch(RuntimePanels& runtime);
	void DrawAppPicker(RuntimePanels& runtime, const RuntimeView& view, float maximumWidth);
	void DrawHistory(RuntimePanels& runtime);
	void DrawToolbar(RuntimePanels& runtime);
	void ToggleLibrary();
	void ShowInspector();
	void FoldTimeline();
	void FoldInspector();
	void PickTab(Selection picked, bool overlay);
	void StoreCrop(const std::optional<ImVec4>& crop);
	bool Divider(const char* id, ShellRect strip, bool vertical, float& value, float minimum, float maximum,
				 float defaultValue, bool folded, float foldBelow);
	void DrawShell(RuntimePanels& runtime);
	void DrawInspectorPanel(RuntimePanels& runtime, bool rail);
	void DrawLibrary(RuntimePanels& runtime);
	void DrawInspector(RuntimePanels& runtime);
#ifdef SIMXR_COMPANION_AUTOMATION
	RuntimeFrameRun ReplayTarget(const RuntimePanels& runtime) const;
	void QueueScript(RuntimePanels& runtime, ScriptRun run);
#endif
	bool m_enabled = false, m_showLaunch = false, m_showSettings = false, m_dirty = false;
	bool m_previewExpanded = false, m_showConnection = false, m_showError = false;
	AppPicker m_picker;
	ShellState m_shell;
	bool m_drawer = true;
	std::string m_launchName, m_launchedName, m_launchedEndpoint;
	std::string m_connectedName, m_connectedEndpoint;
	std::optional<std::string> m_cropProfile;
	bool m_wasOverlay = false;
	struct DividerDrag
	{
		ImGuiID id;
		float request, travel;
	};
	std::optional<DividerDrag> m_dividerDrag;
	std::filesystem::path m_path, m_layoutPath;
	CompanionSettings m_settings;
	ProcessLauncher m_launcher;
	TimelinePanel m_timeline;
	RuntimeConnection m_launchDiscovery;
	std::unique_ptr<LaunchAttachment> m_attachment;
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
	float m_appliedScale = 1;
	ImGuiStyle m_originalStyle;
	bool m_styleCaptured = false;
};
} // namespace simxr::app
