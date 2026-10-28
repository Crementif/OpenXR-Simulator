#include "app/pages/PhysicalHeadsetPage.h"
#include "app/UiPch.h"
#include "app/services/SimulationEdits.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"

namespace simxr::app
{
struct PhysicalHeadsetPage::FileResult
{
	std::mutex mutex;
	bool done = false, failed = false;
	std::string path, error;
	static void SDLCALL Picked(void* user, const char* const* files, int) noexcept
	{
		const std::unique_ptr<std::shared_ptr<FileResult>> owner(static_cast<std::shared_ptr<FileResult>*>(user));
		std::lock_guard lock((*owner)->mutex);
		(*owner)->done = true;
		try
		{
			if (!files)
			{
				(*owner)->failed = true;
				(*owner)->error = SDL_GetError();
			}
			else if (files[0])
				(*owner)->path = files[0];
		}
		catch (...)
		{
			(*owner)->failed = true;
		}
	}
};
void PhysicalHeadsetPage::SetError(std::string error)
{
	m_error = std::move(error);
}
std::optional<HeadsetPageRequest> PhysicalHeadsetPage::Draw(const HeadsetHostSnapshot& host, bool canConnect,
															const std::string& conflict, const ObservedSetup* game)
{
	if (m_file)
	{
		const auto file = m_file;
		std::lock_guard lock(file->mutex);
		if (file->done)
		{
			if (file->failed)
				m_error = file->error.empty() ? "Cannot select a runtime manifest" : file->error;
			else if (!file->path.empty())
				m_manifest = file->path;
			m_file.reset();
		}
	}
	if (!Section("Physical headset", ImGuiTreeNodeFlags_DefaultOpen))
		return {};
	ImGui::PushID("physical-headset");
	const bool active = host.phase == HeadsetHostPhase::Starting || host.phase == HeadsetHostPhase::Running ||
						host.phase == HeadsetHostPhase::Stopping;
	const char* state = "Disconnected";
	switch (host.phase)
	{
	case HeadsetHostPhase::Starting:
		state = "Starting host";
		break;
	case HeadsetHostPhase::Running:
		state = host.input == HeadsetInputState::Ready		 ? "Input connected"
				: host.input == HeadsetInputState::Suspended ? "Input suspended"
															 : "Waiting for input";
		break;
	case HeadsetHostPhase::Stopping:
		state = "Disconnecting";
		break;
	case HeadsetHostPhase::Failed:
		state = "Connection failed";
		break;
	default:
		break;
	}
	if (host.discovering && host.phase == HeadsetHostPhase::Running)
		state = host.recommendedEyes ? "Headset detected" : "Detecting headset";
	ImGui::TextUnformatted(state);
	if (host.trackerSupport)
	{
		if (!*host.trackerSupport)
			ImGui::TextDisabled("Trackers unavailable in this runtime");
		else if (host.connectedTrackers)
			ImGui::Text("Trackers connected: %u", *host.connectedTrackers);
		else
			ImGui::TextDisabled("Tracker support available");
	}
	if (host.phase == HeadsetHostPhase::Running && !host.discovering)
	{
		const char* output = host.output == HeadsetOutputState::Live		 ? "Live game output"
							 : host.output == HeadsetOutputState::Inspection ? "Inspection screen"
							 : host.output == HeadsetOutputState::Hidden	 ? "Headset is not rendering"
																			 : "Waiting for game output";
		ImGui::TextUnformatted(output);
		ImGui::SetItemTooltip("Paused or stale game output appears on a fixed inspection screen in the headset.");
	}
	else
		ImGui::TextWrapped("Play through your headset, then inspect or record the session.");
	ImGui::BeginDisabled(active || static_cast<bool>(m_file));
	ImGui::Checkbox("Custom runtime", &m_customRuntime);
	if (m_customRuntime)
	{
		if (ImGui::Button("Choose runtime manifest"))
		{
			m_file = std::make_shared<FileResult>();
			auto owner = std::make_unique<std::shared_ptr<FileResult>>(m_file);
			SDL_ShowOpenFileDialog(FileResult::Picked, owner.release(), nullptr, nullptr, 0,
								   m_manifest.empty() ? nullptr : m_manifest.c_str(), false);
		}
		ImGui::TextWrapped("%s", m_manifest.empty() ? "No manifest selected" : m_manifest.c_str());
	}
	else
		ImGui::TextDisabled("System runtime");
	ImGui::Checkbox("Forward trackers", &m_trackers);
	ImGui::SetItemTooltip(
		"Use physical HTCX trackers. Requires runtime support and takes ownership of the tracker inventory.");
	if (m_trackers && game && game->appTrackersKnown && !game->appTrackers)
		ImGui::TextWrapped(
			"The game did not enable tracker access. Restart the game with tracker support to use these devices.");
	ImGui::SetNextItemWidth(-1);
	ImGui::Combo("##body-provider", &m_body,
				 "Body: Off\0FB upper body\0META full body\0BD without arms\0BD full body\0");
	ImGui::SetItemTooltip(
		"Forward runtime-estimated body joints. Takes ownership of Body; requires the selected runtime extension.");
	if (m_body)
	{
		const auto provider = m_body <= 2 ? m_body - 1 : 2;
		if (host.bodySupport[provider] == false)
			ImGui::TextWrapped("Selected body provider is unavailable in the detected runtime.");
	}
	if (m_body && game)
	{
		if (!game->appBodyExtensions)
			ImGui::TextWrapped("Game body access is unknown with this simulator version.");
		else
		{
			const auto& enabled = *game->appBodyExtensions;
			ImGui::TextWrapped("Game body access: FB %s, META full %s, BD %s.", enabled[0] ? "on" : "off",
							   enabled[1] ? "on" : "off", enabled[2] ? "on" : "off");
			if (!enabled[0] && !enabled[1] && !enabled[2])
				ImGui::TextWrapped(
					"Restart the game with body tracking support to use forwarded joints. Capture remains available.");
		}
	}
	ImGui::Checkbox("Calibrate LOCAL height", &m_calibrateLocal);
	ImGui::SetItemTooltip("Only needed when the physical runtime supplies no floor-based tracking space.");
	if (m_calibrateLocal)
	{
		ImGui::SetNextItemWidth(-1);
		ImGui::InputFloat("##eye-height", &m_eyeHeight, 0, 0, "%.3f m");
		ImGui::SetItemTooltip("Measured eye height above the floor. Used only with LOCAL tracking.");
	}
	ImGui::EndDisabled();
	std::optional<HeadsetPageRequest> request;
	if (active)
	{
		ImGui::BeginDisabled(host.phase == HeadsetHostPhase::Stopping);
		if (ImGui::Button(host.discovering ? "Stop detection" : "Disconnect"))
			request = HeadsetPageRequest{};
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(host.discovering || host.phase != HeadsetHostPhase::Running ||
							 host.input != HeadsetInputState::Ready || !canConnect);
		if (ImGui::Button("Re-align"))
		{
			m_error.clear();
			request = HeadsetPageRequest{false, true};
		}
		ImGui::SetItemTooltip(
			"Reconnect the headset at the current simulated pose. The game stays running; input pauses briefly.");
		ImGui::EndDisabled();
	}
	else
	{
		ImGui::BeginDisabled(static_cast<bool>(m_file) || (m_customRuntime && m_manifest.empty()));
		if (ImGui::Button("Detect headset"))
		{
			m_error.clear();
			request = HeadsetPageRequest{true};
			request->options.discover = true;
			if (m_customRuntime)
				request->options.runtimeManifest = Utf8Path(m_manifest);
		}
		ImGui::SetItemTooltip("Find render-size recommendations before launching a game. "
							  "Use headset size in Launch profiles saves them for the next launch.");
		ImGui::EndDisabled();
	}
	if (!active || host.discovering)
	{
		ImGui::BeginDisabled(active && host.phase != HeadsetHostPhase::Running);
		ImGui::BeginDisabled(!canConnect || static_cast<bool>(m_file) || (m_customRuntime && m_manifest.empty()));
		if (!conflict.empty())
		{
			ImGui::TextWrapped("%s", conflict.c_str());
			if (m_body)
				ImGui::TextWrapped("Takeover also replaces the Body owner.");
			ImGui::TextWrapped(m_trackers ? "Takeover replaces head, view, hand and tracker owners."
										  : "Takeover replaces head, view and hand owners.");
		}
		if (ImGui::Button(conflict.empty() ? "Connect" : "Take over and connect"))
		{
			m_error.clear();
			request = HeadsetPageRequest{true};
			request->options.takeOver = !conflict.empty();
			request->options.trackers = m_trackers;
			if (m_body)
				request->options.body = static_cast<BodyJointSet>(m_body - 1);
			if (m_customRuntime)
				request->options.runtimeManifest = Utf8Path(m_manifest);
			if (m_calibrateLocal)
				request->options.localEyeHeight = m_eyeHeight;
		}
		ImGui::EndDisabled();
		ImGui::TextWrapped("Connect aligns the headset with the current player.");
		ImGui::EndDisabled();
	}
	if (!m_error.empty())
		ImGui::TextWrapped("%s", m_error.c_str());
	if (!host.error.empty())
		ImGui::TextWrapped("%s", host.error.c_str());
	if (!host.diagnostics.empty() && Section("Connection details"))
		ImGui::TextWrapped("%s", host.diagnostics.c_str());
	ImGui::PopID();
	return request;
}
} // namespace simxr::app
