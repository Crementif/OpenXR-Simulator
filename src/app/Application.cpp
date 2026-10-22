#include "app/Application.h"
#include "app/ProcessSupervisor.h"
#include "app/RuntimePanels.h"
#include "app/UiRenderer.h"
#include "app/Window.h"
#include "app/WorkspacePanels.h"
#ifdef SIMXR_COMPANION_PROOF
#include "app/ProofConsumer.h"
#endif

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <stdexcept>

namespace simxr::app
{
Application::Application(ApplicationOptions options) : m_options(options) {}

int Application::Run()
{
	auto validation = std::make_shared<ValidationLog>();
	ProcessSupervisor processes;
	std::uint64_t submitted = 0;
	std::uint32_t rebuilds = 0;
	bool proofFinished = m_options.proofDescriptor < 0;
	{
		Window window(m_options.hidden);
		UiRenderer renderer(window, m_options.validation, validation, !m_options.screenshot.empty(),
							m_options.proofDescriptor >= 0);
#ifdef SIMXR_COMPANION_PROOF
		std::unique_ptr<ProofConsumer> proof;
		if (m_options.proofDescriptor >= 0)
			proof = std::make_unique<ProofConsumer>(renderer, m_options.proofDescriptor, m_options.proofVerifyPixels);
#else
		if (m_options.proofDescriptor >= 0)
			throw std::runtime_error("FD companion proof is available only in the Linux build");
#endif
		WorkspacePanels workspace(processes,
								  !m_options.smokeFrames && !m_options.runtimeSmoke && m_options.proofDescriptor < 0,
								  m_options.controlSocket);
		std::unique_ptr<RuntimePanels> panels;
		if (m_options.proofDescriptor < 0)
			panels = std::make_unique<RuntimePanels>(renderer, workspace.InitialEndpoint(), &window);
		unsigned runtimeStage = 0;
		std::uint64_t rawCapture = 0, holdUntil = 0;
		bool runtimeFinished = !m_options.runtimeSmoke;
		bool captureRequested = false;
		bool captureCompleted = false;
		std::printf("Companion adapter: %s; SDL driver: %s\n", renderer.DeviceName().c_str(),
					SDL_GetCurrentVideoDriver());
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		bool running = true;
		while (running)
		{
			running = window.PollEvents();
			if (window.TakeFocusLost() && panels)
				panels->FocusLost();
			if (!running)
				break;
			if ((m_options.smokeFrames || m_options.proofDescriptor >= 0 || m_options.runtimeSmoke) &&
				std::chrono::steady_clock::now() > deadline)
				throw std::runtime_error("Companion smoke timed out waiting for actual Vulkan presentation requests");
#ifdef SIMXR_COMPANION_PROOF
			if (proof)
			{
				proof->Poll();
				if ((m_options.screenshot.empty() || captureCompleted) &&
					(!m_options.smokeFrames || renderer.SubmittedFrames() >= m_options.smokeFrames) &&
					proof->FinishIfReady())
				{
					proofFinished = true;
					break;
				}
			}
#endif
			if (!renderer.BeginFrame())
			{
				SDL_Delay(1);
				continue;
			}
			if (panels)
			{
				panels->Poll();
				workspace.BeginFrame(*panels);
				workspace.Draw(*panels);
				if (m_options.runtimeSmoke)
				{
					const auto state = panels->Snapshot();
					if (state.phase == "failed")
						throw std::runtime_error("Runtime smoke operation failed: " + state.error);
					if (runtimeStage == 0 && state.sessionAvailable)
					{
						panels->Pause();
						runtimeStage = 1;
					}
					else if (runtimeStage == 1 && !state.operationPending && state.progress == "paused")
					{
						std::puts("READY companion production controls paused");
						std::fflush(stdout);
						panels->Command(control::CommandKind::Step);
						runtimeStage = 2;
					}
					else if (runtimeStage == 2 && !state.operationPending)
					{
						if (state.progress == "clear")
							panels->Command(control::CommandKind::Step);
						else if (state.selectedReady && state.selectedFrame == 3)
						{
							panels->InspectSelected();
							holdUntil = renderer.SubmittedFrames() + 4;
							runtimeStage = 3;
						}
					}
					else if (runtimeStage == 3 && state.inspectedFrame == 3 && renderer.CompletedFrames() >= holdUntil)
					{
						panels->Command(control::CommandKind::Step, 0, true);
						runtimeStage = 4;
					}
					else if (runtimeStage == 4 && !state.operationPending && state.selectedReady &&
							 state.selectedFrame == 4 && state.selectedLayers == 1)
					{
						rawCapture = state.selectedCapture;
						panels->Command(control::CommandKind::Step);
						runtimeStage = 5;
					}
					else if (runtimeStage == 5 && !state.operationPending && state.selectedReady &&
							 state.selectedFrame == 4 && state.selectedLayers == 2)
					{
						for (const auto capture : panels->CaptureIds())
							panels->Command(control::CommandKind::Release, capture);
						runtimeStage = 6;
					}
					else if (runtimeStage == 6 && !state.operationPending)
					{
						panels->SelectCapture(rawCapture);
						panels->Command(control::CommandKind::Resume);
						runtimeStage = 7;
					}
					else if (runtimeStage == 7 && !state.sessionAvailable && state.selectedReady && state.images >= 3)
					{
						if (!m_options.imageExport.empty())
							panels->RequestExport(m_options.imageExport);
						holdUntil = renderer.SubmittedFrames() + 4;
						runtimeStage = 8;
					}
					else if (runtimeStage == 8 && renderer.CompletedFrames() >= holdUntil &&
							 (m_options.imageExport.empty() || std::filesystem::exists(m_options.imageExport)))
					{
						runtimeFinished = true;
						if (m_options.screenshot.empty())
							running = false;
					}
				}
			}
			else
			{
				const auto* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				ImGui::Begin("OpenXR Simulator", nullptr,
							 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
				ImGui::TextUnformatted("OpenXR Simulator");
				ImGui::Separator();
				ImGui::Text("Vulkan adapter: %s", renderer.DeviceName().c_str());
				ImGui::TextUnformatted("Runtime: disconnected");
				ImGui::TextUnformatted("No runtime transport is connected.");
				ImGui::Text("UI submissions: %llu", static_cast<unsigned long long>(renderer.SubmittedFrames()));
#ifdef SIMXR_COMPANION_PROOF
				if (proof)
					proof->Draw();
#endif
				if (ImGui::Button("Exit"))
					running = false;
				ImGui::End();
			}
			bool captureReady = m_options.proofDescriptor < 0 && runtimeFinished;
#ifdef SIMXR_COMPANION_PROOF
			if (proof)
				captureReady = proof->ReadyForScreenshot();
#endif
			if (captureReady && !m_options.screenshot.empty() && !captureRequested &&
				(!m_options.smokeFrames || renderer.SubmittedFrames() + 1 >= m_options.smokeFrames))
			{
				renderer.RequestScreenshot(m_options.screenshot);
				captureRequested = true;
			}
			std::vector<VkSemaphore> waits;
			if (panels)
				waits = panels->ReadyWaits();
#ifdef SIMXR_COMPANION_PROOF
			if (proof)
				waits = proof->ReadyWaits();
#endif
			bool rendered = false;
			try
			{
				rendered = renderer.Render(waits);
			}
			catch (...)
			{
				if (panels)
					panels->Submitted(renderer.SubmittedFrames());
				throw;
			}
			if (!rendered)
			{
				SDL_Delay(1);
				continue;
			}
			if (captureRequested)
				captureCompleted = true;
			const auto frame = renderer.SubmittedFrames();
			if (panels)
				panels->Submitted(frame);
			if (m_options.runtimeSmoke && runtimeFinished && (m_options.screenshot.empty() || captureCompleted))
			{
				running = false;
			}
#ifdef SIMXR_COMPANION_PROOF
			if (proof)
				proof->Submitted(frame);
#endif
			if (m_options.smokeFrames && m_options.proofDescriptor < 0 && !m_options.runtimeSmoke)
			{
				if (frame == 2)
					window.Resize(800, 600);
				if (frame == 4)
					window.Resize(1024, 576);
				if (frame >= m_options.smokeFrames)
					running = false;
			}
		}
		if (!runtimeFinished)
			throw std::runtime_error("Runtime companion qualification did not complete");
		if (m_options.runtimeSmoke)
			std::puts("PASS companion production controls, composed/raw GPU snapshots, release/resume, application "
					  "device destroyed; normalReadback=0");
		if (!proofFinished)
			throw std::runtime_error("Proof consumer closed before all retained images were sampled");
		workspace.Finish();
		submitted = renderer.SubmittedFrames();
		rebuilds = renderer.RebuildCount();
	}
	// Validation ownership outlives renderer teardown, including all Vulkan destruction.
	std::printf(
		"Companion: %llu UI submissions, %u swapchain creations, %u validation errors; runtime connection mode=%s\n",
		static_cast<unsigned long long>(submitted), rebuilds, validation->errors.load(),
		m_options.controlSocket.empty() ? "interactive" : "local");
	if (validation->errors.load() ||
		(m_options.smokeFrames && (submitted < m_options.smokeFrames ||
								   (m_options.proofDescriptor < 0 && !m_options.runtimeSmoke && rebuilds < 3))))
		return 1;
	return 0;
}
} // namespace simxr::app
