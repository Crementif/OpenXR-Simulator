#include "app/Application.h"
#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/WorkspacePanels.h"
#include "app/platform/Window.h"
#include "app/qa/RuntimeSmoke.h"
#include "app/render/UiRenderer.h"
#include "app/services/ProcessSupervisor.h"
#include "app/services/Utf8Path.h"
#ifdef SIMXR_COMPANION_PROOF
#include "app/render/ProofConsumer.h"
#endif

#include <SDL3/SDL.h>
#include <cstdio>
#include <imgui.h>
#include <stdexcept>

namespace simxr::app
{
Application::Application(ApplicationOptions options) : m_options(std::move(options)) {}

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
		std::optional<ProofConsumer> proof;
		if (m_options.proofDescriptor >= 0)
			proof.emplace(renderer, m_options.proofDescriptor, m_options.proofVerifyPixels);
#else
		if (m_options.proofDescriptor >= 0)
			throw std::runtime_error("FD companion proof is available only in the Linux build");
#endif
		WorkspacePanels workspace(processes,
								  !m_options.smokeFrames && !m_options.runtimeSmoke && m_options.proofDescriptor < 0,
								  m_options.controlSocket);
		std::optional<RuntimePanels> panels;
		if (m_options.proofDescriptor < 0)
			panels.emplace(renderer, workspace.InitialEndpoint(), &window);
		std::optional<RuntimeSmoke> runtimeSmoke;
		if (m_options.runtimeSmoke)
			runtimeSmoke.emplace(m_options.imageExport);
		bool captureRequested = false;
		bool captureCompleted = false;
		std::printf("Companion adapter: %s; SDL driver: %s\n", renderer.DeviceName().c_str(),
					SDL_GetCurrentVideoDriver());
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		bool running = true;
		auto lastFrame = std::chrono::steady_clock::time_point{};
		auto lastInput = lastFrame;
		while (running)
		{
			running = window.PollEvents(
				[&](const SDL_Event& event)
				{
					renderer.ProcessEvent(event);
					if ((event.type >= SDL_EVENT_KEY_DOWN && event.type <= SDL_EVENT_TEXT_EDITING_CANDIDATES) ||
						(event.type >= SDL_EVENT_MOUSE_MOTION && event.type <= SDL_EVENT_MOUSE_WHEEL) ||
						(event.type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && event.type <= SDL_EVENT_GAMEPAD_BUTTON_UP) ||
						event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
						event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
						lastInput = std::chrono::steady_clock::now();
				});
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
			const auto now = std::chrono::steady_clock::now();
			const bool interactive =
				(panels && panels->PilotActive()) || now - lastInput < std::chrono::milliseconds(500);
			const auto interval = std::chrono::nanoseconds(1000000000 / (interactive ? 60 : 30));
			// Poll native events while pacing; queued ImGui input is consumed only by an admitted frame.
			if (now - lastFrame < interval)
			{
				SDL_Delay(1);
				continue;
			}
			if (!renderer.BeginFrame())
			{
				SDL_Delay(1);
				continue;
			}
			lastFrame = now;
			if (panels)
			{
				panels->Poll();
				workspace.BeginFrame(*panels);
				workspace.Draw(*panels);
				if (runtimeSmoke)
					runtimeSmoke->Poll(*panels, renderer);
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
			const bool runtimeFinished = !runtimeSmoke || runtimeSmoke->Finished();
			bool captureReady = m_options.proofDescriptor < 0 && runtimeFinished;
#ifdef SIMXR_COMPANION_PROOF
			if (proof)
				captureReady = proof->ReadyForScreenshot();
#endif
			if (captureReady && !m_options.screenshot.empty() && !captureRequested &&
				(!m_options.smokeFrames || renderer.SubmittedFrames() + 1 >= m_options.smokeFrames))
			{
				renderer.RequestScreenshot(Utf8Path(m_options.screenshot));
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
				rendered = renderer.Render(waits,
										   [&](VkCommandBuffer command)
										   {
											   if (panels)
												   panels->RecordTransfers(command);
										   });
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
				if ((frame == 2 || frame == 4) && !SDL_SyncWindow(window.Native()))
					throw std::runtime_error(std::string("Companion smoke resize did not finish: ") + SDL_GetError());
				if (frame >= m_options.smokeFrames)
					running = false;
			}
		}
		if (runtimeSmoke && !runtimeSmoke->Finished())
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
