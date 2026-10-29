#pragma once

#include "Fixture.h"

#include <string>
#include <vector>

namespace simxr::tests
{
void CheckDefaultInteraction();
void CheckConnectedPreview(const char* endpoint);
void CheckShellScenario();
#ifdef __linux__
void CheckDiscoveryFailure();
#endif
void CheckNotices(Interaction& test);
void CheckTimelineCanvas(Interaction& test);
void CheckPreferences(Interaction& test);
void CheckTimelineWorkflow(Interaction& test);
void CheckShell();
void PilotLiveRuntime(const char* endpoint);
void CheckDocumentTreeIdentity();
void CheckRigPointer();
void CheckCapturedRig();
void CheckSpatialWidgetLifecycle();
void CheckExpandedTrackerUi();
void ConfigureObservedDevices();
void CheckDebuggerWorkflow(const char* endpoint);
void CheckPreviewPilot(Interaction& test);
void CheckVideoExport(Interaction& test);
void CheckPreviewHistory(Interaction& test);
void CheckToolbarLaunch(const char* executable);
void CaptureLayouts(const std::filesystem::path& directory);
void CheckDistributionOnboarding();
void CheckUpdateStartup();
void CheckProfileCopyButton(const std::filesystem::path& executable);
void CheckProfileClipboard(const std::filesystem::path& executable);
void CheckHeadsetLaunchProfile();
void CheckHeadsetRestartProfile();
void CheckPhysicalHeadsetPage();
void CheckArchiveRecordingUi();
void CheckPortableLiveTabs(const std::vector<std::string>& endpoints, const std::filesystem::path& directory);
#ifdef __linux__
void CheckManualEdges(const RuntimeView& target);
void CheckNativePlayerRig(const RuntimeView& target);
void CheckSpatialEditing(Interaction& test);
void CheckInputTake(Interaction& test, const std::filesystem::path& directory);
void CheckLayouts(Interaction& test, const std::filesystem::path& qa);
void CheckShell(Interaction& test, const char* qa);
void CheckIdConflicts(Interaction& test, bool frameOnly);
void CheckLibrary(Interaction& test, const std::filesystem::path& directory);
void CheckInputPad(Interaction& test);
void CapturePolish(Interaction& test, const std::filesystem::path& qa);
void CheckMeasuredTiming(Interaction& test);
void CheckSnapshotRetention(Interaction& test);
void CheckAppPicker(const char* executable);
void CheckBodyRig(Interaction& test);
void CheckSoak(Interaction& test, const std::filesystem::path& directory);
void CheckHeadsetRestartWorkflow(Interaction& test, const std::filesystem::path& directory);
int RunTabLaunchFixture();
void CheckTabLaunches();
void CheckGpuTabInput(const std::string& firstEndpoint, const std::string& secondEndpoint);
void CheckMultiGpuTabs(const std::string& firstEndpoint, const std::string& secondEndpoint);
void CheckApplicationTabs();
void CheckLiveHaptics();
#endif
}
